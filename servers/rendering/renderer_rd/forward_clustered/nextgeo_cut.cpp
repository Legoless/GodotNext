/**************************************************************************/
/*  nextgeo_cut.cpp                                                       */
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

#include "nextgeo_cut.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/math/math_funcs_binary.h"
#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/variant/array.h"
#include "servers/rendering/renderer_rd/environment/nextgeo_cull.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_skinning.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_sw_raster.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_visibility.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"
// GodotNext: spline_meshes (#161).
#include "servers/rendering/storage/spline_deform.h"
#include "servers/rendering/storage/mesh_storage.h"
// GodotNext: vsm_nanite (#217).
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

#include <cstddef>

namespace {
struct CutParams {
	float view_proj[16];
	float prev_view_proj[16];
	float frustum[6][4];
	float camera[4];
	float view[4];
	uint32_t cull[4];
	// GodotNext: nextgeo_sw_raster (#162). The triangle-size switch; zero for shadow views.
	float raster_forward[4]; // xyz = camera forward axis, w = z_near.
	float raster_params[4]; // x = T px, y = px scale, z = 1 for an orthographic view.
	// GodotNext: nextgeo_masked_two_sided (#153), nextgeo_vertex_offset (#164).
	// xy = pixel-programmable bins (1..64) demotable past programmable_raster_distance;
	// zw = vertex-offset bins (1..64) demotable past vertex_offset_disable_distance.
	uint32_t raster_bin_demote_mask[4];
	// GodotNext: nextgeo_tessellation (#163). xy = tessellated displaced bins (1..64).
	uint32_t raster_bin_tess_mask[4];
};
static_assert(sizeof(CutParams) == 336, "CutParams must match NextGeoCullParamsData (336 bytes).");

struct CutCounters {
	uint32_t phase1;
	uint32_t phase2;
	uint32_t overflow;
	uint32_t cluster_count;
	uint32_t cut_lo;
	uint32_t cut_hi;
	uint32_t group_count;
	uint32_t level_counts[8];
	uint32_t hash_lo;
	uint32_t hash_hi;
	uint32_t pad[2];
	uint32_t phase1_clusters;
	uint32_t phase2_clusters;
	uint32_t phase2_visible_count;
	uint32_t phase2_group_count;
	uint32_t phase2_level_counts[8];
	// GodotNext: nextgeo_sw_raster (#162). sw_raster.glsl reads these by word index.
	uint32_t raster_sw[2];
	uint32_t raster_hw[2];
	uint32_t raster_sw_writes;
	// GodotNext: nextgeo_raster_bins (#155). Clusters of programmable bins per phase.
	uint32_t raster_bin_sw[2];
	uint32_t raster_bin_hw[2];
	// GodotNext: nextgeo_masked_two_sided (#153). Clusters demoted to FIXED past programmable_raster_distance per phase.
	uint32_t raster_distance_fixed[2];
	// GodotNext: nextgeo_vertex_offset (#164). WPO clusters demoted to FIXED past vertex_offset_disable_distance and clamped vertices.
	uint32_t vertex_offset_distance_fixed[2];
	uint32_t vertex_offset_clamped;
	uint32_t sink; // #277 D3D12 GBV atomic return sink.
	// GodotNext: nextgeo_voxels (#165). Visible voxel bricks per phase (words 46..47).
	uint32_t voxel_bricks[2];
};
static_assert(sizeof(CutCounters) == 192, "CutCounters must match NextGeoCountersData (192 bytes).");
static_assert(offsetof(CutCounters, raster_sw) == 31 * sizeof(uint32_t), "sw_raster.glsl reads raster_sw at word 31.");
static_assert(offsetof(CutCounters, raster_hw) == 33 * sizeof(uint32_t), "visibility_raster_mesh.glsl reads raster_hw at word 33.");
static_assert(offsetof(CutCounters, raster_sw_writes) == 35 * sizeof(uint32_t), "sw_raster.glsl adds raster_sw_writes at word 35.");
static_assert(offsetof(CutCounters, vertex_offset_clamped) == 44 * sizeof(uint32_t), "raster_material_inc.glsl adds vertex_offset_clamped at word 44.");
static_assert(offsetof(CutCounters, sink) == 45 * sizeof(uint32_t), "raster_material_inc.glsl writes sink at word 45.");
static_assert(offsetof(CutCounters, voxel_bricks) == 46 * sizeof(uint32_t), "voxel_raster.glsl reads voxel_bricks at word 46.");
static_assert(offsetof(RendererRD::NextGeoVSMCounters, hw_clusters) == 13 * sizeof(uint32_t), "VSM mesh task copies hw_clusters from word 13.");

struct RasterParams {
	float proj[16];
	float view[16];
	float prev_proj[16];
	float prev_view[16];
	float taa_jitter[2];
	float prev_taa_jitter[2];
	uint32_t visible_capacity;
	uint32_t reset_velocity;
	float cull_sign;
	uint32_t bucket_table_words;
	// GodotNext: nextgeo_sw_raster (#162). Read by visibility_export.glsl; the raster shaders declare
	// only the fields above.
	float inv_view_proj[16];
	uint32_t size[2];
	uint32_t raster_region;
	uint32_t patch_capacity; // GodotNext: nextgeo_tessellation (#163)
	// GodotNext: nextgeo_voxels (#165). Read by visibility_export.glsl.
	uint32_t voxel_brick_capacity;
	uint32_t pad_v0;
	uint32_t pad_v1;
	uint32_t pad_v2;
};
static_assert(sizeof(RasterParams) == 384, "RasterParams must stay 384 bytes.");

// GodotNext: nextgeo_sw_raster (#162), nextgeo_mesh_raster (#154). Words of raster_args
// (raster_features_inc.glsl): per phase and bin a software dispatch (0: x, y, 1, 0), a hardware VS
// draw record (1), and a hardware mesh-task dispatch record (2: ceil(hw/32), 1, 1, 0).
constexpr uint32_t RASTER_BIN_COUNT = RendererVirtualMeshStorage::RASTER_BIN_COUNT;
constexpr uint32_t RASTER_ARGS_KINDS = 3u;
constexpr uint32_t RASTER_ARGS_WORDS = 2u * RASTER_BIN_COUNT * RASTER_ARGS_KINDS * 4u;
constexpr uint32_t raster_args_word(uint32_t p_phase, uint32_t p_bin, uint32_t p_kind) {
	return ((p_phase * RASTER_BIN_COUNT + p_bin) * RASTER_ARGS_KINDS + p_kind) * 4u;
}

struct ShadowRasterParams {
	float proj[16];
	float view[16];
	float z_far;
	float dp_side;
	float cull_sign;
	uint32_t use_pancake;
	uint32_t visible_capacity;
	uint32_t pad0;
	uint32_t pad1;
	uint32_t pad2;
};
static_assert(sizeof(ShadowRasterParams) == 160, "ShadowRasterParams must stay 160 bytes.");

Mutex s_stats_mutex;
Dictionary s_last_stats;
uint32_t s_resolve_active_buckets = 0;
uint32_t s_resolve_stage_a_draws = 0;
uint32_t s_resolve_stage_b_draws = 0;
bool s_resolve_stage_b_wrote = false;
// GodotNext: nextgeo_shading_bins (#158)
Dictionary s_shading_stats;
Array s_last_shadow_views;
int64_t s_last_shadow_tick = 0;
int64_t s_last_shadow_render_frame = 0;
uint32_t s_shadow_views_dispatched = 0;
uint32_t s_shadow_raster_draws = 0;
double s_shadow_cull_gpu_ms = 0.0;
double s_shadow_raster_gpu_ms = 0.0;
// GodotNext: nextgeo_sw_raster (#162). The last main view's raster path, for the debug stats.
const char *s_raster_mode = "hardware";
const char *s_raster_reason = "NO_INT64_ATOMICS";
const char *s_vis64_storage = "none";
float s_sw_raster_max_edge_px = 0.0f;
// GodotNext: nextgeo_raster_bins (#155). The last main view's programmable raster bins.
uint32_t s_programmable_bins = 0;
// GodotNext: nextgeo_mesh_raster (#154). The last main view's HW raster path, for stats and tests.
const char *s_hw_raster_path = "vertex";
const char *s_hw_raster_reason = "AUTO_PENDING_PQ";
// GodotNext: nextgeo_tessellation (#163). Tessellation debug stats exposed via virtual_geometry_get_debug_stats().
bool s_tessellation_enabled = false;
bool s_tessellation_buffers_allocated = false;
uint32_t s_tessellation_dicing_dispatches = 0;
float s_tessellation_dicing_rate = RendererRD::NEXTGEO_TESS_DEFAULT_DICING_RATE;
uint32_t s_tess_gpu_stats[RendererRD::NEXTGEO_TESS_STATS_WORDS] = {};
bool s_tess_have_gpu_stats = false;
// GodotNext: nextgeo_voxels (#165). Voxel debug stats exposed via virtual_geometry_get_debug_stats().
bool s_voxels_enabled = false;
bool s_voxels_buffers_allocated = false;
uint32_t s_voxel_raster_dispatches = 0;
uint32_t s_voxel_bricks_phase1 = 0;
uint32_t s_voxel_bricks_phase2 = 0;
// GodotNext: nextgeo_dynamic_wind (#296). Dynamic Wind GPU dispatch & floor telemetry.
bool s_dynamic_wind_buffers_allocated = false;
uint32_t s_dynamic_wind_dispatches = 0u;
uint64_t s_dynamic_wind_total_dispatches = 0u;
const char *s_dynamic_wind_floor_reason = "DISABLED";

Mutex s_page_req_registry_mutex;
HashMap<uint64_t, NextGeoCut *> s_page_req_registry;
uint64_t s_page_req_next_token = 1;
constexpr uint32_t PAGE_REQ_BUFFER_BYTES = (4u + NextGeoCut::REQUEST_USED_WORDS + NextGeoCut::REQUEST_CAPACITY * 4u) * sizeof(uint32_t);

uint32_t read_u32(const uint8_t *p_data, uint32_t p_offset) {
	uint32_t value = 0;
	memcpy(&value, p_data + p_offset, sizeof(uint32_t));
	return value;
}

float read_f32(const uint8_t *p_data, uint32_t p_offset) {
	float value = 0.0f;
	memcpy(&value, p_data + p_offset, sizeof(float));
	return value;
}
} // namespace

// GodotNext: nextgeo_gpu_cull (#76). The CPU reference for the GPU cut: the
// same instance/BVH/group predicates over the same records and DAG blobs.
static uint64_t CPU_cut_hash(const RendererVirtualMeshStorage *p_storage, const CutParams &p_cut, uint32_t p_record_count, uint32_t p_max_levels, uint32_t &r_instances, uint32_t &r_groups, uint32_t &r_clusters, float *r_max_cut_error = nullptr) {
	if (r_max_cut_error != nullptr) {
		*r_max_cut_error = 0.0f;
	}
	LocalVector<uint64_t> pairs;
	const uint32_t cull_mask = p_cut.cull[0];
	const uint32_t cut_flags = p_cut.cull[3];
	const bool is_shadow_view = (cut_flags & RendererRD::NEXTGEO_CUT_FLAG_SHADOW_VIEW) != 0;
	const float tau = p_cut.camera[3];
	const float height = p_cut.view[1];
	const float znear = p_cut.view[2];
	const float lod_bias = p_cut.view[3];
	const float p11 = Math::abs(p_cut.view_proj[5]); // column-major [1][1]; the depth correction flips y
	const Vector3 eye(p_cut.camera[0], p_cut.camera[1], p_cut.camera[2]);
	auto plane_distance = [&](const float p_plane[4], const Vector3 &p_point) {
		return p_plane[0] * p_point.x + p_plane[1] * p_point.y + p_plane[2] * p_point.z + p_plane[3];
	};
	// GodotNext: nextgeo_foliage_instancing (#151). Expand MultiMeshes and 2-level Assemblies on CPU to match GPU cut.
	LocalVector<RendererVirtualMeshStorage::Record> eff_records;
	LocalVector<RendererVirtualMeshStorage::InstanceAttributes> eff_attrs;
	p_storage->get_effective_records_cpu(eff_records, eff_attrs);
	const uint32_t eff_count = MIN(uint32_t(eff_records.size()), p_record_count);
	for (uint32_t record_index = 0; record_index < eff_count; record_index++) {
		const RendererVirtualMeshStorage::Record &record = eff_records[record_index];
		const RendererVirtualMeshStorage::InstanceAttributes &attr = eff_attrs[record_index];
		const RendererVirtualMeshStorage::Record *parent_ptr = (attr.meta[0] < eff_records.size()) ? &eff_records[attr.meta[0]] : nullptr;
		const float wind_sway = p_storage->is_any_wind_active() ? p_storage->mesh_index_get_wind_max_sway(record.virtual_mesh) : 0.0f;
		if (!RendererVirtualMeshStorage::cull_instance_cpu(record, attr, parent_ptr, p_cut.frustum, eye, cull_mask, is_shadow_view, wind_sway)) {
			continue;
		}
		const float wpo_bound = RendererVirtualMeshStorage::get_record_vertex_offset_bound(record);
		const float wpo_margin = (wpo_bound + wind_sway) * Math::abs(record.world_aabb_min[3]);
		const Vector3 lo(record.world_aabb_min[0] - wpo_margin, record.world_aabb_min[1] - wpo_margin, record.world_aabb_min[2] - wpo_margin);
		const Vector3 hi(record.world_aabb_max[0] + wpo_margin, record.world_aabb_max[1] + wpo_margin, record.world_aabb_max[2] + wpo_margin);
		const uint32_t mesh_index = record.virtual_mesh;
		const RendererVirtualMeshStorage::MeshInfo *mesh = p_storage->get_mesh_info(mesh_index);
		if (mesh == nullptr || mesh->dag_size < 8) {
			continue;
		}
		const uint8_t *dag = mesh->desc.dag_header.ptr();
		const uint32_t group_count = read_u32(dag, 0);
		const uint32_t bvh_count = read_u32(dag, 4);
		if (bvh_count == 0 || mesh->record.bvh_level_count == 0) {
			continue;
		}
		const uint32_t node_base = 8 + group_count * 60;
		const float scale = record.world_aabb_min[3];
		const bool has_spline = (record.flags & RendererVirtualMeshStorage::RECORD_HAS_SPLINE) != 0u;
		const SplineDeformPose spline_pose = has_spline ? p_storage->get_spline_entry(record.flags >> RendererVirtualMeshStorage::RECORD_SPLINE_INDEX_SHIFT).current : SplineDeformPose();
		const bool is_skinned = (record.flags & RendererVirtualMeshStorage::RECORD_SKINNED_PALETTE) != 0u;
		float inst_bone_scale = 1.0f;
		const float *skin_palette = nullptr;
		uint32_t skin_palette_bones = 0u;
		if (is_skinned) {
			const RendererVirtualMeshStorage::SkinInstanceInfo sinfo = p_storage->get_skin_instance_info(record_index);
			if (sinfo.bone_count > 0u) {
				float s = 1.0f;
				memcpy(&s, &sinfo.max_bone_scale_bits, sizeof(float));
				inst_bone_scale = s > 0.0f ? s : 1.0f;
			}
			const RID skel = p_storage->instance_record_get_skeleton(record_index);
			if (skel.is_valid()) {
				skin_palette = p_storage->get_skeleton_palette_data(skel, false, skin_palette_bones);
			} else {
				// GodotNext: nextgeo_dynamic_wind (#296). Use CPU mirror of simulated bone palette for CPU_cut_hash parity.
				skin_palette = p_storage->get_record_dynamic_wind_palette_data(record_index, false, skin_palette_bones);
			}
		}
		auto projected = [&](const float p_sphere[4], float p_error, float p_bone_scale, bool p_use_world_aabb_dist) {
			if (p_error >= 3.4e38f) {
				return tau + 1.0f;
			}
			Vector3 local_center(p_sphere[0], p_sphere[1], p_sphere[2]);
			float local_radius = p_sphere[3];
			float eff_error = p_error;
			if (has_spline) {
				const float raw_radius = local_radius;
				spline_deform_bounds_sphere(spline_pose, local_center, local_radius);
				eff_error = eff_error * spline_pose.up_max_stretch[3] + spline_deform_curvature_error(spline_pose, raw_radius);
			}
			const float eff_scale = scale * p_bone_scale;
			if ((cut_flags & RendererRD::NEXTGEO_CUT_FLAG_ORTHOGRAPHIC) != 0) {
				return eff_error * eff_scale * p_cut.view[0] * lod_bias;
			}
			float distance = znear;
			if (p_use_world_aabb_dist) {
				const Vector3 closest(
						CLAMP(eye.x, lo.x, hi.x),
						CLAMP(eye.y, lo.y, hi.y),
						CLAMP(eye.z, lo.z, hi.z));
				distance = MAX(closest.distance_to(eye), znear);
			} else {
				const Vector3 center(
						record.transform[0][0] * local_center.x + record.transform[0][1] * local_center.y + record.transform[0][2] * local_center.z + record.transform[0][3],
						record.transform[1][0] * local_center.x + record.transform[1][1] * local_center.y + record.transform[1][2] * local_center.z + record.transform[1][3],
						record.transform[2][0] * local_center.x + record.transform[2][1] * local_center.y + record.transform[2][2] * local_center.z + record.transform[2][3]);
				// GodotNext: nextgeo_displacement (#150), nextgeo_vertex_offset (#164), nextgeo_foliage_wind (#152). Mirror cluster_cull.glsl bound inflation.
				const float expand = MAX(record.prev_world_aabb_max[3], 0.0f) + wpo_bound + wind_sway;
				const float radius = (local_radius + expand) * scale;
				distance = MAX(center.distance_to(eye) - radius, znear);
			}
			if ((cut_flags & RendererRD::NEXTGEO_CUT_FLAG_DUAL_PARABOLOID) != 0) {
				return eff_error * eff_scale * 0.5f * height / distance * lod_bias;
			}
			return eff_error * eff_scale * 0.5f * height * p11 / distance * lod_bias;
		};
		auto skin_aabb_visible = [&](const Vector3 &p_lmin, const Vector3 &p_lmax) {
			const float expand = MAX(record.prev_world_aabb_max[3], 0.0f) + wpo_bound + wind_sway;
			const Vector3 lmin = p_lmin - Vector3(expand, expand, expand);
			const Vector3 lmax = p_lmax + Vector3(expand, expand, expand);
			Vector3 corners[8];
			for (int c = 0; c < 8; c++) {
				const Vector3 lp((c & 1) ? lmax.x : lmin.x, (c & 2) ? lmax.y : lmin.y, (c & 4) ? lmax.z : lmin.z);
				corners[c] = Vector3(
						record.transform[0][0] * lp.x + record.transform[0][1] * lp.y + record.transform[0][2] * lp.z + record.transform[0][3],
						record.transform[1][0] * lp.x + record.transform[1][1] * lp.y + record.transform[1][2] * lp.z + record.transform[1][3],
						record.transform[2][0] * lp.x + record.transform[2][1] * lp.y + record.transform[2][2] * lp.z + record.transform[2][3]);
			}
			for (int plane = 0; plane < 6; plane++) {
				bool any_in = false;
				for (int c = 0; c < 8; c++) {
					if (plane_distance(p_cut.frustum[plane], corners[c]) >= 0.0f) {
						any_in = true;
						break;
					}
				}
				if (!any_in) {
					return false;
				}
			}
			return true;
		};
		r_instances++;
		LocalVector<uint32_t> stack;
		stack.push_back(0);
		LocalVector<uint32_t> groups;
		while (!stack.is_empty()) {
			const uint32_t node = stack[stack.size() - 1];
			stack.remove_at(stack.size() - 1);
			const uint32_t base = node_base + node * 80;
			const float sphere[4] = { read_f32(dag, base), read_f32(dag, base + 4), read_f32(dag, base + 8), read_f32(dag, base + 12) };
			const float error = read_f32(dag, base + 16);
			if (projected(sphere, error, inst_bone_scale, is_skinned) <= tau) {
				continue;
			}
			const uint32_t child_count = MIN(read_u32(dag, base + 44), 8u);
			for (uint32_t child = 0; child < child_count; child++) {
				const uint32_t value = read_u32(dag, base + 48 + child * 4);
				if ((value & 0x80000000u) != 0) {
					groups.push_back(value & ~0x80000000u);
					r_groups++;
				} else if (value < bvh_count) {
					stack.push_back(value);
				}
			}
		}
		const uint32_t slot_page_size = p_storage->get_page_slot_size_bytes();
		for (const uint32_t &group_index : groups) {
			if (group_index >= group_count || !p_storage->mesh_is_group_refine_ok(mesh_index, group_index)) {
				continue;
			}
			const uint32_t base = 8 + group_index * 60;
			const float sphere[4] = { read_f32(dag, base + 16), read_f32(dag, base + 20), read_f32(dag, base + 24), read_f32(dag, base + 28) };
			const float error = read_f32(dag, base + 32);
			if (projected(sphere, error, inst_bone_scale, is_skinned) <= tau) {
				continue;
			}
			const uint32_t cluster_offset = read_u32(dag, base);
			const uint32_t cluster_count = read_u32(dag, base + 4);
			const uint32_t group_page_id = read_u32(dag, base + 8);
			const bool page_has_slot = group_page_id < mesh->page_to_slot.size() && mesh->page_to_slot[group_page_id] != RendererVirtualMeshStorage::INVALID_PAGE_SLOT && slot_page_size >= 96u;
			for (uint32_t cluster = 0; cluster < cluster_count; cluster++) {
				const uint32_t global_cluster = cluster_offset + cluster;
				bool has_skin_bounds = false;
				float cluster_bone_scale = inst_bone_scale;
				float skin_sphere[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				if (is_skinned && skin_palette != nullptr && skin_palette_bones > 0u && global_cluster >= mesh->record.cluster_offset) {
					const uint32_t local_cluster = global_cluster - mesh->record.cluster_offset;
					if (local_cluster < mesh->cluster_skin_bones.size() && !mesh->cluster_skin_bones[local_cluster].is_empty()) {
						const LocalVector<RendererVirtualMeshStorage::ClusterSkinBone> &bones = mesh->cluster_skin_bones[local_cluster];
						LocalVector<RendererRD::NextGeoClusterSkinBone> ref_bones;
						ref_bones.resize(bones.size());
						for (uint32_t b = 0; b < bones.size(); b++) {
							ref_bones[b].global_bone = bones[b].global_bone;
							ref_bones[b].aabb_min = Vector3(bones[b].aabb_min[0], bones[b].aabb_min[1], bones[b].aabb_min[2]);
							ref_bones[b].aabb_max = Vector3(bones[b].aabb_max[0], bones[b].aabb_max[1], bones[b].aabb_max[2]);
						}
						Vector3 lmin;
						Vector3 lmax;
						if (RendererRD::nextgeo_skin_cluster_bounds_from_bones(ref_bones.ptr(), ref_bones.size(), skin_palette, skin_palette_bones, lmin, lmax, cluster_bone_scale)) {
							has_skin_bounds = true;
							if (!skin_aabb_visible(lmin, lmax)) {
								continue;
							}
							const Vector3 center = (lmin + lmax) * 0.5f;
							const float radius = (lmax - lmin).length() * 0.5f;
							skin_sphere[0] = center.x;
							skin_sphere[1] = center.y;
							skin_sphere[2] = center.z;
							skin_sphere[3] = radius;
						}
					}
				}
				float cluster_cut_err = 0.0f;
				if (page_has_slot && global_cluster >= mesh->record.cluster_offset) {
					const uint32_t local_cluster = global_cluster - mesh->record.cluster_offset;
					if (local_cluster < mesh->cluster_refine.size() && mesh->cluster_refine[local_cluster].valid) {
						const RendererVirtualMeshStorage::ClusterRefineInfo &ref = mesh->cluster_refine[local_cluster];
						if (ref.refined_group >= 0 && uint32_t(ref.refined_group) < group_count) {
							const float child_err = has_skin_bounds
									? projected(skin_sphere, ref.group_error, cluster_bone_scale, false)
									: projected(ref.group_lod, ref.group_error, inst_bone_scale, is_skinned);
							if (child_err > tau && p_storage->mesh_is_group_refine_ok(mesh_index, uint32_t(ref.refined_group))) {
								continue;
							}
							cluster_cut_err = child_err;
						}
					}
				}
				if (r_max_cut_error != nullptr && cluster_cut_err > *r_max_cut_error) {
					*r_max_cut_error = cluster_cut_err;
				}
				pairs.push_back((uint64_t(record_index) << 32) | uint64_t(global_cluster));
				r_clusters++;
			}
		}
	}
	return RendererRD::nextgeo_cut_hash(pairs);
}

RID NextGeoCut::_storage(uint32_t p_bytes) {
	return RD::get_singleton()->storage_buffer_create(p_bytes);
}

bool NextGeoCut::_ensure() {
	if (initialized) {
		return true;
	}
	if (failed) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}
#ifdef REAL_T_IS_DOUBLE
	const String base_defines = "\n#define REAL_T_IS_DOUBLE\n";
#else
	const String base_defines = "";
#endif
	instance_shader.initialize(Vector<String>({
			base_defines,
			base_defines + "\n#define PHASE2\n",
			base_defines + "\n#define SHADOW_VIEW\n",
			base_defines + "\n#define ASSEMBLY_EXPAND\n",
	}));
	instance_version = instance_shader.version_create();
	multimesh_expand_shader.initialize(Vector<String>({ base_defines }));
	multimesh_expand_version = multimesh_expand_shader.version_create();
	cluster_shader.initialize(Vector<String>({
			base_defines,
			base_defines + "\n#define CLUSTER_CULL\n",
			base_defines + "\n#define CUT_HASH\n",
			base_defines + "\n#define SHADOW_VIEW\n",
			base_defines + "\n#define SHADOW_VIEW\n#define CLUSTER_CULL\n",
			base_defines + "\n#define SHADOW_VIEW\n#define CUT_HASH\n",
	}));
	cluster_version = cluster_shader.version_create();
	hiz_shader.initialize(Vector<String>({ "", "\n#define CLEAR_VISIBILITY\n" }));
	hiz_version = hiz_shader.version_create();
	raster_shader.initialize(Vector<String>({
			base_defines,
			base_defines + "\n#define WITH_VELOCITY\n",
			base_defines + "\n#define DEBUG_BACKFACE\n",
			base_defines + "\n#define WITH_VELOCITY\n#define DEBUG_BACKFACE\n",
			base_defines + "\n#define MODE_SHADOW\n",
			base_defines + "\n#define MODE_SHADOW\n#define MODE_DUAL_PARABOLOID\n",
	}));
	raster_version = raster_shader.version_create();
	if (!instance_version.is_valid() || !multimesh_expand_version.is_valid() || !cluster_version.is_valid() || !hiz_version.is_valid() || !raster_version.is_valid()) {
		failed = true;
		return false;
	}
	instance_pipeline = rd->compute_pipeline_create(instance_shader.version_get_shader(instance_version, 0));
	instance_phase2 = rd->compute_pipeline_create(instance_shader.version_get_shader(instance_version, 1));
	instance_shadow_pipeline = rd->compute_pipeline_create(instance_shader.version_get_shader(instance_version, 2));
	instance_assembly_expand_pipeline = rd->compute_pipeline_create(instance_shader.version_get_shader(instance_version, 3));
	multimesh_expand_pipeline = rd->compute_pipeline_create(multimesh_expand_shader.version_get_shader(multimesh_expand_version, 0));
	cluster_bvh = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 0));
	cluster_leaf = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 1));
	cluster_hash = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 2));
	cluster_shadow_bvh = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 3));
	cluster_shadow_leaf = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 4));
	cluster_shadow_hash = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 5));
	hiz_pipeline = rd->compute_pipeline_create(hiz_shader.version_get_shader(hiz_version, 0));
	clear_vis_pipeline = rd->compute_pipeline_create(hiz_shader.version_get_shader(hiz_version, 1));
	if (!instance_pipeline.is_valid() || !instance_phase2.is_valid() || !instance_shadow_pipeline.is_valid() || !instance_assembly_expand_pipeline.is_valid() || !multimesh_expand_pipeline.is_valid() || !cluster_bvh.is_valid() || !cluster_leaf.is_valid() || !cluster_hash.is_valid() || !cluster_shadow_bvh.is_valid() || !cluster_shadow_leaf.is_valid() || !cluster_shadow_hash.is_valid() || !hiz_pipeline.is_valid() || !clear_vis_pipeline.is_valid()) {
		failed = true;
		return false;
	}
	for (int variant = 0; variant < 4; variant++) {
		RD::PipelineRasterizationState raster_state;
		raster_state.cull_mode = (variant & 2) != 0 ? RD::POLYGON_CULL_DISABLED : RD::POLYGON_CULL_BACK;
		RD::PipelineMultisampleState multisample_state;
		RD::PipelineDepthStencilState depth_state;
		depth_state.enable_depth_test = true;
		depth_state.enable_depth_write = true;
		depth_state.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
		RD::PipelineColorBlendState blend_state = RD::PipelineColorBlendState::create_disabled((variant & 1) != 0 ? 3 : 2);
		raster_pipelines[variant].setup(raster_shader.version_get_shader(raster_version, variant), RD::RENDER_PRIMITIVE_TRIANGLES, raster_state, multisample_state, depth_state, blend_state);
	}
	for (int variant = 0; variant < 2; variant++) {
		RD::PipelineRasterizationState raster_state;
		raster_state.cull_mode = RD::POLYGON_CULL_BACK;
		RD::PipelineMultisampleState multisample_state;
		RD::PipelineDepthStencilState depth_state;
		depth_state.enable_depth_test = true;
		depth_state.enable_depth_write = true;
		depth_state.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
		RD::PipelineColorBlendState blend_state = RD::PipelineColorBlendState::create_disabled(0);
		shadow_raster_pipelines[variant].setup(raster_shader.version_get_shader(raster_version, 4 + variant), RD::RENDER_PRIMITIVE_TRIANGLES, raster_state, multisample_state, depth_state, blend_state);
	}

	params = _storage(sizeof(CutParams));
	raster_params = _storage(sizeof(RasterParams));
	counters = _storage(sizeof(CutCounters));
	aux_counters = _storage(sizeof(CutCounters));
	shadow_params = _storage(MAX_SHADOW_VIEWS * sizeof(CutParams));
	shadow_raster_params = _storage(MAX_SHADOW_VIEWS * sizeof(ShadowRasterParams));
	shadow_counters = _storage(MAX_SHADOW_VIEWS * sizeof(CutCounters));
	for (int ring = 0; ring < 3; ring++) {
		counters_ring[ring] = _storage(sizeof(CutCounters));
		counters_ring_pending[ring] = false;
		shadow_counters_ring[ring] = _storage(MAX_SHADOW_VIEWS * sizeof(CutCounters));
		shadow_counters_ring_pending[ring] = false;
		request_ring[ring] = _storage(PAGE_REQ_BUFFER_BYTES);
		request_ring_pending[ring] = false;
	}
	counters_ring_cursor = 0;
	shadow_counters_ring_cursor = 0;
	request_ring_cursor = 0;
	nodes = _storage(NODE_CAPACITY * 8);
	phase2_nodes = _storage(NODE_CAPACITY * 8);
	groups = _storage(GROUP_CAPACITY * 8);
	clusters = _storage(CLUSTER_CAPACITY * 8);
	level_a = _storage(NODE_CAPACITY * 8);
	level_b = _storage(NODE_CAPACITY * 8);
	shadow_nodes = _storage(MAX_SHADOW_VIEWS * SHADOW_NODES_PER_VIEW * 8);
	shadow_level_a = _storage(MAX_SHADOW_VIEWS * SHADOW_NODES_PER_VIEW * 8);
	shadow_level_b = _storage(MAX_SHADOW_VIEWS * SHADOW_NODES_PER_VIEW * 8);
	shadow_groups = _storage(MAX_SHADOW_VIEWS * SHADOW_GROUPS_PER_VIEW * 8);
	shadow_clusters = _storage(MAX_SHADOW_VIEWS * SHADOW_CLUSTERS_PER_VIEW * 8);
	bvh_args[0] = rd->storage_buffer_create(18 * 4 * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	bvh_args[1] = rd->storage_buffer_create(18 * 4 * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	draw_args = rd->storage_buffer_create(2 * sizeof(RendererRD::NextGeoDrawIndirectCommand), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	shadow_bvh_args[0] = rd->storage_buffer_create(MAX_SHADOW_VIEWS * 9 * 4 * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	shadow_bvh_args[1] = rd->storage_buffer_create(MAX_SHADOW_VIEWS * 9 * 4 * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	shadow_draw_args = rd->storage_buffer_create(MAX_SHADOW_VIEWS * sizeof(RendererRD::NextGeoDrawIndirectCommand), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	Vector<uint8_t> dummy_zero;
	dummy_zero.resize(256);
	dummy_zero.fill(0);
	dummy_page_buf = rd->storage_buffer_create(256, dummy_zero);
	multimesh_expand_params = _storage(80);
	assembly_counter_buf = rd->storage_buffer_create(16, dummy_zero.slice(0, 16));

	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R32_SFLOAT;
	format.width = 1;
	format.height = 1;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	Vector<uint8_t> zero;
	zero.resize(4);
	zero.fill(0);
	Vector<Vector<uint8_t>> data;
	data.push_back(zero);
	dummy_hiz = rd->texture_create(format, RD::TextureView(), data);
	RD::SamplerState sampler_state;
	sampler_state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
	sampler_state.min_filter = RD::SAMPLER_FILTER_NEAREST;
	sampler_state.mip_filter = RD::SAMPLER_FILTER_NEAREST;
	sampler_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler_state.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	dummy_sampler = rd->sampler_create(sampler_state);
	initialized = params.is_valid() && raster_params.is_valid() && counters.is_valid() && aux_counters.is_valid() && shadow_params.is_valid() && shadow_raster_params.is_valid() && shadow_counters.is_valid() && counters_ring[0].is_valid() && counters_ring[1].is_valid() && counters_ring[2].is_valid() && shadow_counters_ring[0].is_valid() && shadow_counters_ring[1].is_valid() && shadow_counters_ring[2].is_valid() && request_ring[0].is_valid() && request_ring[1].is_valid() && request_ring[2].is_valid() && nodes.is_valid() && phase2_nodes.is_valid() && groups.is_valid() && clusters.is_valid() && level_a.is_valid() && level_b.is_valid() && shadow_nodes.is_valid() && shadow_level_a.is_valid() && shadow_level_b.is_valid() && shadow_groups.is_valid() && shadow_clusters.is_valid() && bvh_args[0].is_valid() && bvh_args[1].is_valid() && draw_args.is_valid() && shadow_bvh_args[0].is_valid() && shadow_bvh_args[1].is_valid() && shadow_draw_args.is_valid() && dummy_page_buf.is_valid() && multimesh_expand_params.is_valid() && assembly_counter_buf.is_valid() && dummy_hiz.is_valid() && dummy_sampler.is_valid();
	if (!initialized) {
		failed = true;
		return false;
	}
	// GodotNext: nextgeo_sw_raster (#162). Below the Nanite device tier, or when its shaders cannot be
	// built, every view keeps the v2.4 hardware raster.
	_ensure_hybrid(rd);
	return initialized;
}

bool NextGeoCut::_ensure_hybrid(RenderingDevice *p_rd) {
	hybrid_ready = false;
	vis64_image_ready = false;
	vis64_buffer_ready = false;
	vis64_mesh_image_ready = false;
	vis64_mesh_buffer_ready = false;
	raster_tier = RendererRD::nextgeo_raster_tier(p_rd);
	String detail;
	if (!raster_tier.hybrid) {
		PackedStringArray missing;
		if (!p_rd->has_feature(RD::SUPPORTS_IMAGE_INT64_ATOMICS) && !p_rd->has_feature(RD::SUPPORTS_BUFFER_INT64_ATOMICS)) {
			missing.push_back("SUPPORTS_IMAGE_INT64_ATOMICS");
			missing.push_back("SUPPORTS_BUFFER_INT64_ATOMICS");
		}
		if (!p_rd->has_feature(RD::SUPPORTS_DRAW_INDIRECT_COUNT)) {
			missing.push_back("SUPPORTS_DRAW_INDIRECT_COUNT");
		}
		detail = "the device lacks " + String(", ").join(missing);
	} else {
		const bool image_int64 = p_rd->has_feature(RD::SUPPORTS_IMAGE_INT64_ATOMICS);
		const bool buffer_int64 = p_rd->has_feature(RD::SUPPORTS_BUFFER_INT64_ATOMICS);
#ifdef REAL_T_IS_DOUBLE
		const String base_defines = "\n#define REAL_T_IS_DOUBLE\n";
#else
		const String base_defines = "";
#endif
		// Variants a device cannot create stay disabled: #175 fails shader creation for a missing
		// 64-bit atomic feature. The hardware variants live in their own ShaderRD of
		// visibility_raster.glsl, so they can never invalidate the v2.4 raster's version.
		if (sw_shader.get_variant_count() == 0) {
			sw_shader.initialize(Vector<String>({
					base_defines,
					base_defines + "\n#define VIS64_BUFFER\n",
					base_defines + "\n#define CLEAR_VIS64\n",
					base_defines + "\n#define CLEAR_VIS64\n#define VIS64_BUFFER\n",
			}));
		}
		if (export_shader.get_variant_count() == 0) {
			export_shader.initialize(Vector<String>({
					base_defines,
					base_defines + "\n#define WITH_VELOCITY\n",
					base_defines + "\n#define VIS64_BUFFER\n",
					base_defines + "\n#define VIS64_BUFFER\n#define WITH_VELOCITY\n",
			}));
		}
		if (vis64_raster_shader.get_variant_count() == 0) {
			vis64_raster_shader.initialize(Vector<String>({
					base_defines + "\n#define MODE_VIS64\n",
					base_defines + "\n#define MODE_VIS64\n#define DEBUG_BACKFACE\n",
					base_defines + "\n#define MODE_VIS64\n#define VIS64_BUFFER\n",
					base_defines + "\n#define MODE_VIS64\n#define VIS64_BUFFER\n#define DEBUG_BACKFACE\n",
			}));
		}
		for (int storage = 0; storage < 2; storage++) {
			const bool supported = storage == 0 ? image_int64 : buffer_int64;
			sw_shader.set_variant_enabled(storage, supported);
			sw_shader.set_variant_enabled(2 + storage, supported);
			for (int sub = 0; sub < 2; sub++) {
				export_shader.set_variant_enabled(storage * 2 + sub, supported);
				vis64_raster_shader.set_variant_enabled(storage * 2 + sub, supported);
			}
		}
		sw_version = sw_shader.version_create();
		export_version = export_shader.version_create();
		vis64_raster_version = vis64_raster_shader.version_create();
		raster_args = p_rd->storage_buffer_create(RASTER_ARGS_WORDS * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		raster_counts = p_rd->storage_buffer_create(4 * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		raster_lists = _storage(CLUSTER_CAPACITY * sizeof(uint32_t));
		if (sw_version.is_valid() && export_version.is_valid() && vis64_raster_version.is_valid() && raster_args.is_valid() && raster_counts.is_valid() && raster_lists.is_valid()) {
			for (int storage = 0; storage < 2; storage++) {
				const bool supported = storage == 0 ? image_int64 : buffer_int64;
				if (!supported) {
					continue;
				}
				const RID sw = sw_shader.version_get_shader(sw_version, storage);
				const RID clear = sw_shader.version_get_shader(sw_version, 2 + storage);
				if (!sw.is_valid() || !clear.is_valid()) {
					continue;
				}
				sw_pipelines[storage] = p_rd->compute_pipeline_create(sw);
				vis64_clear_pipelines[storage] = p_rd->compute_pipeline_create(clear);
				bool ok = sw_pipelines[storage].is_valid() && vis64_clear_pipelines[storage].is_valid();
				for (int sub = 0; sub < 2 && ok; sub++) {
					const RID export_variant = export_shader.version_get_shader(export_version, storage * 2 + sub);
					const RID raster_variant = vis64_raster_shader.version_get_shader(vis64_raster_version, storage * 2 + sub);
					ok = export_variant.is_valid() && raster_variant.is_valid();
					if (!ok) {
						break;
					}
					// Export: GREATER_OR_EQUAL against the depth pre-pass, as the v2.4 raster tests.
					RD::PipelineRasterizationState export_raster_state;
					RD::PipelineMultisampleState multisample_state;
					RD::PipelineDepthStencilState export_depth;
					export_depth.enable_depth_test = true;
					export_depth.enable_depth_write = true;
					export_depth.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
					export_pipelines[storage * 2 + sub].setup(export_variant, RD::RENDER_PRIMITIVE_TRIANGLES, export_raster_state, multisample_state, export_depth, RD::PipelineColorBlendState::create_disabled(sub != 0 ? 3 : 2));
					// Hardware path: no attachments; the back-face debug variant disables culling.
					RD::PipelineRasterizationState vis64_raster_state;
					vis64_raster_state.cull_mode = sub != 0 ? RD::POLYGON_CULL_DISABLED : RD::POLYGON_CULL_BACK;
					vis64_raster_pipelines[storage * 2 + sub].setup(raster_variant, RD::RENDER_PRIMITIVE_TRIANGLES, vis64_raster_state, multisample_state, RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(0));
				}
				if (ok) {
					(storage == 0 ? vis64_image_ready : vis64_buffer_ready) = true;
				}
			}
		}
		hybrid_ready = vis64_image_ready || vis64_buffer_ready;
		if (!hybrid_ready) {
			detail = "its shaders could not be built";
		} else {
			// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9). Build the task/mesh HW
			// visibility variants only when the device supports indirect mesh draws within our limits.
			const RendererRD::NextGeoHWRasterCapabilities mesh_caps = RendererRD::nextgeo_hw_raster_capabilities_from_device(p_rd);
			if (RendererRD::nextgeo_select_hw_raster_path(RendererRD::NEXTGEO_HW_RASTER_PATH_MESH, mesh_caps).use_mesh) {
				if (vis64_mesh_shader.get_variant_count() == 0) {
					vis64_mesh_shader.initialize(Vector<String>({
							base_defines,
							base_defines + "\n#define DEBUG_BACKFACE\n",
							base_defines + "\n#define VIS64_BUFFER\n",
							base_defines + "\n#define VIS64_BUFFER\n#define DEBUG_BACKFACE\n",
					}));
				}
				for (int storage = 0; storage < 2; storage++) {
					const bool supported = storage == 0 ? image_int64 : buffer_int64;
					for (int sub = 0; sub < 2; sub++) {
						vis64_mesh_shader.set_variant_enabled(storage * 2 + sub, supported);
					}
				}
				vis64_mesh_version = vis64_mesh_shader.version_create();
				if (vis64_mesh_version.is_valid()) {
					for (int storage = 0; storage < 2; storage++) {
						const bool supported = storage == 0 ? image_int64 : buffer_int64;
						if (!supported) {
							continue;
						}
						bool ok = true;
						for (int sub = 0; sub < 2 && ok; sub++) {
							const RID mesh_variant = vis64_mesh_shader.version_get_shader(vis64_mesh_version, storage * 2 + sub);
							ok = mesh_variant.is_valid();
							if (!ok) {
								break;
							}
							RD::PipelineRasterizationState vis64_raster_state;
							vis64_raster_state.cull_mode = sub != 0 ? RD::POLYGON_CULL_DISABLED : RD::POLYGON_CULL_BACK;
							vis64_mesh_pipelines[storage * 2 + sub].setup(mesh_variant, RD::RENDER_PRIMITIVE_TRIANGLES, vis64_raster_state, RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(0));
						}
						if (ok) {
							(storage == 0 ? vis64_mesh_image_ready : vis64_mesh_buffer_ready) = true;
						}
					}
				}
			}
		}
	}
	if (!hybrid_ready) {
		raster_tier.hybrid = false;
		raster_tier.storage = RendererRD::NEXTGEO_VIS64_NONE;
		raster_tier.reason = RendererRD::NEXTGEO_FALLBACK_NO_INT64_ATOMICS;
		// GN-13: the reason is logged once, and NextGeo keeps the v2.4 hardware raster.
		print_line(vformat("NextGeo: the hybrid HW/SW raster is off (%s): %s. NextGeo keeps the v2.4 hardware raster.", RendererRD::nextgeo_fallback_name(raster_tier.reason), detail));
	}
	{
		MutexLock lock(s_stats_mutex);
		s_raster_reason = RendererRD::nextgeo_fallback_name(raster_tier.reason);
	}
	return hybrid_ready;
}

bool NextGeoCut::is_hybrid_raster_available() {
	return _ensure() && hybrid_ready;
}

RendererRD::NextGeoVis64Storage NextGeoCut::get_vis64_storage(bool p_prefer_buffer) const {
	if (!hybrid_ready) {
		return RendererRD::NEXTGEO_VIS64_NONE;
	}
	if (vis64_buffer_ready && (p_prefer_buffer || !vis64_image_ready)) {
		return RendererRD::NEXTGEO_VIS64_BUFFER;
	}
	return vis64_image_ready ? RendererRD::NEXTGEO_VIS64_IMAGE : RendererRD::NEXTGEO_VIS64_NONE;
}

RendererRD::NextGeoHWRasterSelection NextGeoCut::select_hw_raster_path(RendererRD::NextGeoHWRasterPath p_setting, bool p_multiview) const {
	const RendererRD::NextGeoHWRasterCapabilities caps = RendererRD::nextgeo_hw_raster_capabilities_from_device(RD::get_singleton());
	return RendererRD::nextgeo_select_hw_raster_path(p_setting, caps, p_multiview);
}

void NextGeoCut::_clear_vis64(const NextGeoRasterInputs &p_raster, const Size2i &p_internal_size) {
	RenderingDevice *rd = RD::get_singleton();
	const int storage = p_raster.vis64_storage == RendererRD::NEXTGEO_VIS64_BUFFER ? 1 : 0;
	RID shader = sw_shader.version_get_shader(sw_version, 2 + storage);
	RD::Uniform u_vis64(storage == 1 ? RD::UNIFORM_TYPE_STORAGE_BUFFER : RD::UNIFORM_TYPE_IMAGE, 0, p_raster.vis64);
	RID set = UniformSetCacheRD::get_singleton()->get_cache(shader, 0, u_vis64);
	if (!set.is_valid()) {
		return;
	}
	const uint32_t width = uint32_t(MAX(p_internal_size.x, 1));
	const uint32_t height = uint32_t(MAX(p_internal_size.y, 1));
	const uint32_t push[4] = { width, height, 0u, 0u };
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, vis64_clear_pipelines[storage]);
	rd->compute_list_bind_uniform_set(list, set, 0);
	rd->compute_list_set_push_constant(list, push, sizeof(push));
	rd->compute_list_dispatch(list, (width + 63u) / 64u, height, 1u);
	rd->compute_list_end();
}

RID NextGeoCut::_get_vis64_framebuffer(const Size2i &p_size) {
	RenderingDevice *rd = RD::get_singleton();
	if (vis64_framebuffer.is_valid() && vis64_framebuffer_size == p_size && rd->framebuffer_is_valid(vis64_framebuffer)) {
		return vis64_framebuffer;
	}
	if (vis64_framebuffer.is_valid() && rd->framebuffer_is_valid(vis64_framebuffer)) {
		rd->free_rid(vis64_framebuffer);
	}
	vis64_framebuffer = rd->framebuffer_create_empty(p_size);
	vis64_framebuffer_size = p_size;
	return vis64_framebuffer;
}

// One phase of the hybrid raster (N2): the software dispatch and the hardware indirect-count draw of
// each raster bin write the visibility buffer, then the export writes the v2.4 targets and depth.
bool NextGeoCut::_draw_hybrid_phase(uint32_t p_phase, uint32_t p_list_base, const NextGeoRasterInputs &p_raster, RID p_counters, const Size2i &p_internal_size, bool p_stats) {
	RenderingDevice *rd = RD::get_singleton();
	FramebufferCacheRD *fb_cache = FramebufferCacheRD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (rd == nullptr || fb_cache == nullptr || uniform_set_cache == nullptr) {
		return false;
	}
	const int storage = p_raster.vis64_storage == RendererRD::NEXTGEO_VIS64_BUFFER ? 1 : 0;
	const RD::UniformType vis64_type = storage == 1 ? RD::UNIFORM_TYPE_STORAGE_BUFFER : RD::UNIFORM_TYPE_IMAGE;
	const Size2i size(MAX(p_internal_size.x, 1), MAX(p_internal_size.y, 1));
	const RID page_buf = RSG::virtual_mesh_storage->get_page_buffer().is_valid() ? RSG::virtual_mesh_storage->get_page_buffer() : dummy_page_buf;
	const RID cluster_buf = RSG::virtual_mesh_storage->get_cluster_buffer().is_valid() ? RSG::virtual_mesh_storage->get_cluster_buffer() : dummy_page_buf;
	const RID bucket_buf = p_raster.bucket_table.is_valid() ? p_raster.bucket_table : dummy_page_buf;
	const RID spline_buf = RSG::virtual_mesh_storage->get_spline_buffer();
	const RID skin_palette_buf = RSG::virtual_mesh_storage->get_skin_palette_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_palette_buffer() : dummy_page_buf;
	const RID skin_inst_buf = RSG::virtual_mesh_storage->get_skin_instance_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_instance_buffer() : dummy_page_buf;
	const RID cluster_skin_buf = RSG::virtual_mesh_storage->get_cluster_skin_buffer().is_valid() ? RSG::virtual_mesh_storage->get_cluster_skin_buffer() : dummy_page_buf;
	RD::Uniform u_records(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, RSG::virtual_mesh_storage->get_record_buffer());
	RD::Uniform u_meshes(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RSG::virtual_mesh_storage->get_mesh_record_buffer());
	RD::Uniform u_pages(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, page_buf);
	RD::Uniform u_cluster_table(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, cluster_buf);
	RD::Uniform u_visible(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, clusters);
	RD::Uniform u_raster(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, raster_params);
	RD::Uniform u_buckets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, bucket_buf);
	RD::Uniform u_lists(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, raster_lists);
	const RID wind_params_buf = RSG::virtual_mesh_storage->get_wind_params_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_params_buffer() : dummy_page_buf;
	const RID wind_meshes_buf = RSG::virtual_mesh_storage->get_wind_meshes_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_meshes_buffer() : dummy_page_buf;
	RD::Uniform u_skin_palette_11(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, skin_palette_buf);
	RD::Uniform u_skin_instances_12(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, skin_inst_buf);
	RD::Uniform u_cluster_skin_13(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, cluster_skin_buf);
	RD::Uniform u_wind_params_14(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, wind_params_buf);
	RD::Uniform u_wind_meshes_15(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, wind_meshes_buf);
	RD::Uniform u_skin_palette_10(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, skin_palette_buf);
	RD::Uniform u_skin_instances_11(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, skin_inst_buf);
	RD::Uniform u_cluster_skin_12(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, cluster_skin_buf);
	RD::Uniform u_wind_params_13(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, wind_params_buf);
	RD::Uniform u_wind_meshes_14(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, wind_meshes_buf);

	// Software clusters: one workgroup each, dispatched indirectly per bin.
	{
		RID shader = sw_shader.version_get_shader(sw_version, storage);
		LocalVector<RD::Uniform> sw_u;
		sw_u.push_back(u_records);
		sw_u.push_back(u_meshes);
		sw_u.push_back(u_pages);
		sw_u.push_back(u_cluster_table);
		sw_u.push_back(u_visible);
		sw_u.push_back(u_raster);
		sw_u.push_back(u_buckets);
		sw_u.push_back(u_lists);
		sw_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, p_counters));
		sw_u.push_back(RD::Uniform(vis64_type, 9, p_raster.vis64));
		sw_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, spline_buf));
		sw_u.push_back(u_skin_palette_11);
		sw_u.push_back(u_skin_instances_12);
		sw_u.push_back(u_cluster_skin_13);
		sw_u.push_back(u_wind_params_14);
		sw_u.push_back(u_wind_meshes_15);
		RID set = uniform_set_cache->get_cache_vec(shader, 0, sw_u);
		if (!set.is_valid()) {
			return false;
		}
		const uint32_t flags = (p_raster.debug_backface ? 1u : 0u) | (p_stats ? 2u : 0u);
		const uint32_t push[8] = { p_list_base, p_phase, flags, 0u, uint32_t(size.x), uint32_t(size.y), 0u, 0u };
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, sw_pipelines[storage]);
		rd->compute_list_bind_uniform_set(list, set, 0);
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		for (uint32_t bin = 0; bin < RASTER_BIN_COUNT; bin++) {
			rd->compute_list_dispatch_indirect(list, raster_args, raster_args_word(p_phase, bin, 0u) * sizeof(uint32_t));
		}
		rd->compute_list_end();
	}

	// Hardware clusters: one indirect-count draw over the bin records, with no attachments.
	// GodotNext: nextgeo_mesh_raster (#154). Uses task/mesh shaders when active, otherwise keeps VS.
	{
		const int variant = storage * 2 + (p_raster.debug_backface ? 1 : 0);
		RID fb = _get_vis64_framebuffer(size);
		if (!fb.is_valid()) {
			return false;
		}
		const bool use_mesh_hw = p_raster.use_mesh_hw_raster && (storage == 1 ? vis64_mesh_buffer_ready : vis64_mesh_image_ready);
		RD::Uniform u_vis64(vis64_type, 8, p_raster.vis64);
		if (use_mesh_hw) {
			RID pipeline = vis64_mesh_pipelines[variant].get_render_pipeline(RD::INVALID_ID, rd->framebuffer_get_format(fb));
			RID shader = vis64_mesh_shader.version_get_shader(vis64_mesh_version, variant);
			LocalVector<RD::Uniform> hw_u;
			hw_u.push_back(u_records);
			hw_u.push_back(u_meshes);
			hw_u.push_back(u_pages);
			hw_u.push_back(u_cluster_table);
			hw_u.push_back(u_visible);
			hw_u.push_back(u_raster);
			hw_u.push_back(u_buckets);
			hw_u.push_back(u_lists);
			hw_u.push_back(u_vis64);
			hw_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, p_counters));
			hw_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, spline_buf));
			hw_u.push_back(u_skin_palette_11);
			hw_u.push_back(u_skin_instances_12);
			hw_u.push_back(u_cluster_skin_13);
			hw_u.push_back(u_wind_params_14);
			hw_u.push_back(u_wind_meshes_15);
			RID set = uniform_set_cache->get_cache_vec(shader, 0, hw_u);
			if (!pipeline.is_valid() || !set.is_valid()) {
				return false;
			}
			RD::DrawListID dl = rd->draw_list_begin(fb, RD::DRAW_DEFAULT_ALL);
			rd->draw_list_bind_render_pipeline(dl, pipeline);
			rd->draw_list_bind_uniform_set(dl, set, 0);
			const uint32_t push[4] = { p_list_base, PHASE2_CLUSTER_BASE, uint32_t(size.x), p_phase };
			rd->draw_list_set_push_constant(dl, push, sizeof(push));
			rd->draw_list_draw_mesh_tasks_indirect_count(dl, raster_args, raster_args_word(p_phase, 0u, 2u) * sizeof(uint32_t), raster_counts, p_phase * sizeof(uint32_t), RASTER_BIN_COUNT, RASTER_ARGS_KINDS * 4u * sizeof(uint32_t));
			rd->draw_list_end();
		} else {
			RID pipeline = vis64_raster_pipelines[variant].get_render_pipeline(RD::INVALID_ID, rd->framebuffer_get_format(fb));
			RID shader = vis64_raster_shader.version_get_shader(vis64_raster_version, variant);
			LocalVector<RD::Uniform> hw_u;
			hw_u.push_back(u_records);
			hw_u.push_back(u_meshes);
			hw_u.push_back(u_pages);
			hw_u.push_back(u_cluster_table);
			hw_u.push_back(u_visible);
			hw_u.push_back(u_raster);
			hw_u.push_back(u_buckets);
			hw_u.push_back(u_lists);
			hw_u.push_back(u_vis64);
			hw_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, spline_buf));
			hw_u.push_back(u_skin_palette_10);
			hw_u.push_back(u_skin_instances_11);
			hw_u.push_back(u_cluster_skin_12);
			hw_u.push_back(u_wind_params_13);
			hw_u.push_back(u_wind_meshes_14);
			RID set = uniform_set_cache->get_cache_vec(shader, 0, hw_u);
			if (!pipeline.is_valid() || !set.is_valid()) {
				return false;
			}
			RD::DrawListID dl = rd->draw_list_begin(fb, RD::DRAW_DEFAULT_ALL);
			rd->draw_list_bind_render_pipeline(dl, pipeline);
			rd->draw_list_bind_uniform_set(dl, set, 0);
			const uint32_t push[4] = { p_list_base, PHASE2_CLUSTER_BASE, uint32_t(size.x), 0u };
			rd->draw_list_set_push_constant(dl, push, sizeof(push));
			rd->draw_list_draw_indirect_count(dl, false, raster_args, raster_args_word(p_phase, 0u, 1u) * sizeof(uint32_t), raster_counts, p_phase * sizeof(uint32_t), RASTER_BIN_COUNT, RASTER_ARGS_KINDS * 4u * sizeof(uint32_t));
			rd->draw_list_end();
		}
	}

	// GodotNext: nextgeo_raster_bins (#155). The programmable bins: compaction, then each bin's software
	// dispatch and hardware draw, into the same visibility buffer before the export.
	if (raster_bins_active) {
		_compact_raster_bins(p_phase, p_list_base);
		_draw_raster_bins(p_phase, p_list_base, p_raster, p_counters, p_internal_size);
	}

	// GodotNext: nextgeo_voxels (#165, plan §21.4 contract N10). Software-only compute splat of
	// visible sparse 4^3 voxel bricks into vis64 before export (0 dispatches when off).
	if (voxels_active && voxel_pipelines[storage].is_valid() && voxel_bricks.is_valid() && voxel_raster_args.is_valid()) {
		const RID vshader = voxel_shader.version_get_shader(voxel_version, storage);
		LocalVector<RD::Uniform> vox_u;
		vox_u.push_back(u_records);
		vox_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, page_buf));
		vox_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, voxel_bricks));
		vox_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, raster_params));
		vox_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, bucket_buf));
		vox_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_counters));
		vox_u.push_back(RD::Uniform(vis64_type, 6, p_raster.vis64));
		const RID vset = uniform_set_cache->get_cache_vec(vshader, 0, vox_u);
		if (vset.is_valid()) {
			const uint32_t phase_cap = RendererRD::NEXTGEO_VOXEL_BRICK_CAPACITY / 2u;
			const uint32_t voxel_list_base = p_phase == 0u ? 0u : phase_cap;
			const uint32_t vflags = p_stats ? 2u : 0u;
			const uint32_t vpush[8] = { voxel_list_base, p_phase, vflags, phase_cap, uint32_t(size.x), uint32_t(size.y), 0u, 0u };
			RD::ComputeListID vlist = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(vlist, voxel_pipelines[storage]);
			rd->compute_list_bind_uniform_set(vlist, vset, 0);
			rd->compute_list_set_push_constant(vlist, vpush, sizeof(vpush));
			rd->compute_list_dispatch_indirect(vlist, voxel_raster_args, p_phase * 4u * sizeof(uint32_t));
			rd->compute_list_end();
			voxel_dispatches_last_frame++;
		}
	}

	// Export: depth, the R32 visibility ID, the instance ID and velocity.
	{
		const bool with_velocity = p_raster.velocity.is_valid();
		const int variant = storage * 2 + (with_velocity ? 1 : 0);
		RID fb = with_velocity ? fb_cache->get_cache(p_raster.visibility, p_raster.instance_id, p_raster.velocity, p_raster.depth) : fb_cache->get_cache(p_raster.visibility, p_raster.instance_id, p_raster.depth);
		if (!fb.is_valid()) {
			return false;
		}
		RID pipeline = export_pipelines[variant].get_render_pipeline(RD::INVALID_ID, rd->framebuffer_get_format(fb));
		RID shader = export_shader.version_get_shader(export_version, variant);
		LocalVector<RD::Uniform> export_u;
		export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, RSG::virtual_mesh_storage->get_record_buffer()));
		export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, clusters));
		export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, raster_params));
		export_u.push_back(RD::Uniform(vis64_type, 3, p_raster.vis64));
		export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, (tess_active && tess_diced_patches.is_valid()) ? tess_diced_patches : dummy_page_buf));
		if (with_velocity) {
			export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, RSG::virtual_mesh_storage->get_mesh_record_buffer()));
			export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, page_buf));
			export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, cluster_buf));
			export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, skin_palette_buf));
			export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, skin_inst_buf));
			export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, cluster_skin_buf));
			export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, spline_buf));
			export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, wind_params_buf));
			export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, wind_meshes_buf));
		}
		// GodotNext: nextgeo_voxels (#165). Set 0 binding 14 (skipping 12..13 reserved for #152).
		export_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, (voxels_active && voxel_bricks.is_valid()) ? voxel_bricks : dummy_page_buf));
		RID set = uniform_set_cache->get_cache_vec(shader, 0, export_u);
		if (!pipeline.is_valid() || !set.is_valid()) {
			return false;
		}
		// The v2.4 load/clear rules: load visibility, instance ID and depth; clear velocity in Phase 1
		// only when the legacy prepass did not already pre-clear it. Never DRAW_IGNORE_ALL.
		BitField<RD::DrawFlags> draw_flags = RD::DRAW_DEFAULT_ALL;
		Vector<Color> clear_colors;
		if (p_phase == 0 && p_raster.clear_velocity && with_velocity) {
			draw_flags = RD::DRAW_CLEAR_COLOR_2;
			clear_colors.push_back(Color(0, 0, 0, 0));
			clear_colors.push_back(Color(0, 0, 0, 0));
			clear_colors.push_back(p_raster.velocity_clear);
		}
		RD::DrawListID dl = rd->draw_list_begin(fb, draw_flags, clear_colors, 0.0f);
		rd->draw_list_bind_render_pipeline(dl, pipeline);
		rd->draw_list_bind_uniform_set(dl, set, 0);
		rd->draw_list_draw(dl, false, 1u, 3u);
		rd->draw_list_end();
	}
	return true;
}

// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N3, N4).
bool NextGeoCut::_ensure_raster_bins(RenderingDevice *p_rd) {
	if (raster_bins_ready) {
		return true;
	}
	if (raster_bin_compact_shader.get_variant_count() == 0) {
#ifdef REAL_T_IS_DOUBLE
		const String base_defines = "\n#define REAL_T_IS_DOUBLE\n";
#else
		const String base_defines = "";
#endif
		raster_bin_compact_shader.initialize(Vector<String>({
				base_defines + "\n#define PREFIX\n",
				base_defines,
		}));
	}
	if (raster_bin_compact_version.is_null()) {
		raster_bin_compact_version = raster_bin_compact_shader.version_create();
	}
	if (raster_bin_compact_version.is_null()) {
		return false;
	}
	const RID prefix = raster_bin_compact_shader.version_get_shader(raster_bin_compact_version, 0);
	const RID scatter = raster_bin_compact_shader.version_get_shader(raster_bin_compact_version, 1);
	if (prefix.is_null() || scatter.is_null()) {
		return false;
	}
	raster_bin_prefix_pipeline = p_rd->compute_pipeline_create(prefix);
	raster_bin_scatter_pipeline = p_rd->compute_pipeline_create(scatter);
	if (raster_bin_state.is_null()) {
		raster_bin_state = _storage(RendererRD::NEXTGEO_RASTER_BIN_STATE_WORDS * sizeof(uint32_t));
	}
	if (raster_bin_args.is_null()) {
		raster_bin_args = p_rd->storage_buffer_create(RendererRD::NEXTGEO_RASTER_BIN_ARGS_WORDS * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	if (raster_bin_hw_counts.is_null()) {
		raster_bin_hw_counts = p_rd->storage_buffer_create(RendererRD::NEXTGEO_RASTER_BIN_HW_COUNTS_WORDS * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	if (raster_bin_append.is_null()) {
		raster_bin_append = _storage(CLUSTER_CAPACITY * sizeof(uint32_t));
	}
	if (raster_bin_lists.is_null()) {
		raster_bin_lists = _storage(CLUSTER_CAPACITY * sizeof(uint32_t));
	}
	raster_bins_ready = raster_bin_prefix_pipeline.is_valid() && raster_bin_scatter_pipeline.is_valid() && raster_bin_state.is_valid() && raster_bin_args.is_valid() && raster_bin_hw_counts.is_valid() && raster_bin_append.is_valid() && raster_bin_lists.is_valid();
	return raster_bins_ready;
}

bool NextGeoCut::_prepare_raster_bins(const NextGeoRasterInputs &p_raster) {
	if (p_raster.raster_bin_programs.is_empty() || p_raster.raster_bin_table == nullptr || p_raster.raster_bin_table_words == 0u || p_raster.environment_uniforms.is_empty()) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || !_ensure_raster_bins(rd)) {
		return false;
	}
	if (raster_bin_table_buffer.is_null() || raster_bin_table_capacity < p_raster.raster_bin_table_words) {
		if (raster_bin_table_buffer.is_valid()) {
			rd->free_rid(raster_bin_table_buffer);
		}
		raster_bin_table_capacity = MAX(Math::next_power_of_2(p_raster.raster_bin_table_words), 256u);
		raster_bin_table_buffer = _storage(raster_bin_table_capacity * sizeof(uint32_t));
		if (raster_bin_table_buffer.is_null()) {
			raster_bin_table_capacity = 0;
			return false;
		}
	}
	rd->buffer_update(raster_bin_table_buffer, 0, p_raster.raster_bin_table_words * sizeof(uint32_t), p_raster.raster_bin_table);
	rd->buffer_clear(raster_bin_state, 0, RendererRD::NEXTGEO_RASTER_BIN_STATE_WORDS * sizeof(uint32_t));
	rd->buffer_clear(raster_bin_args, 0, RendererRD::NEXTGEO_RASTER_BIN_ARGS_WORDS * sizeof(uint32_t));
	rd->buffer_clear(raster_bin_hw_counts, 0, RendererRD::NEXTGEO_RASTER_BIN_HW_COUNTS_WORDS * sizeof(uint32_t));
	return true;
}

// The per-bin compaction of one phase: the prefix pass writes the ranges and arguments, then the scatter
// pass, dispatched from the prefix pass's arguments, fills the ranges.
void NextGeoCut::_compact_raster_bins(uint32_t p_phase, uint32_t p_list_base) {
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	const RID prefix = raster_bin_compact_shader.version_get_shader(raster_bin_compact_version, 0);
	const RID scatter = raster_bin_compact_shader.version_get_shader(raster_bin_compact_version, 1);
	RD::Uniform u_state(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, raster_bin_state);
	RD::Uniform u_append(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, raster_bin_append);
	RD::Uniform u_lists(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, raster_bin_lists);
	RD::Uniform u_args(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, raster_bin_args);
	RD::Uniform u_hw_counts(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, raster_bin_hw_counts);
	const RID prefix_set = uniform_set_cache->get_cache(prefix, 0, u_state, u_append, u_lists, u_args, u_hw_counts);
	const RID scatter_set = uniform_set_cache->get_cache(scatter, 0, u_state, u_append, u_lists);
	if (prefix_set.is_null() || scatter_set.is_null()) {
		return;
	}
	const uint32_t region_size = p_phase == 0u ? PHASE2_CLUSTER_BASE : CLUSTER_CAPACITY - PHASE2_CLUSTER_BASE;
	const uint32_t push[4] = { p_phase, p_list_base, region_size, RendererRD::NEXTGEO_RASTER_VERTEX_COUNT };

	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, raster_bin_prefix_pipeline);
	rd->compute_list_bind_uniform_set(list, prefix_set, 0);
	rd->compute_list_set_push_constant(list, push, sizeof(push));
	rd->compute_list_dispatch(list, 1u, 1u, 1u);
	rd->compute_list_end();

	list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, raster_bin_scatter_pipeline);
	rd->compute_list_bind_uniform_set(list, scatter_set, 0);
	rd->compute_list_set_push_constant(list, push, sizeof(push));
	rd->compute_list_dispatch_indirect(list, raster_bin_args, RendererRD::nextgeo_raster_bin_scatter_args_word(p_phase) * sizeof(uint32_t));
	rd->compute_list_end();
}

RID NextGeoCut::_raster_bin_compute_pipeline(RID p_shader) {
	RenderingDevice *rd = RD::get_singleton();
	RID *existing = raster_bin_compute_pipelines.getptr(p_shader);
	if (existing != nullptr && rd->compute_pipeline_is_valid(*existing)) {
		return *existing;
	}
	const RID pipeline = rd->compute_pipeline_create(p_shader);
	if (pipeline.is_valid()) {
		raster_bin_compute_pipelines[p_shader] = pipeline;
	}
	return pipeline;
}

RID NextGeoCut::_raster_bin_render_pipeline(RID p_shader, RD::FramebufferFormatID p_format) {
	RenderingDevice *rd = RD::get_singleton();
	if (raster_bin_render_format != p_format) {
		for (const KeyValue<RID, RID> &E : raster_bin_render_pipelines) {
			if (rd->render_pipeline_is_valid(E.value)) {
				rd->free_rid(E.value);
			}
		}
		raster_bin_render_pipelines.clear();
		raster_bin_render_format = p_format;
	}
	RID *existing = raster_bin_render_pipelines.getptr(p_shader);
	if (existing != nullptr && rd->render_pipeline_is_valid(*existing)) {
		return *existing;
	}
	// The fixed-function bin's hardware state: no attachments, back faces culled after the vertex
	// stage's winding.
	RD::PipelineRasterizationState raster_state;
	raster_state.cull_mode = RD::POLYGON_CULL_BACK;
	const RID pipeline = rd->render_pipeline_create(p_shader, p_format, RD::INVALID_ID, RD::RENDER_PRIMITIVE_TRIANGLES, raster_state, RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(0));
	if (pipeline.is_valid()) {
		raster_bin_render_pipelines[p_shader] = pipeline;
	}
	return pipeline;
}

// Each programmable bin's software dispatch and hardware draw (N3): one indirect dispatch and one
// indirect draw per bin, with the material's raster target and parameters bound.
void NextGeoCut::_draw_raster_bins(uint32_t p_phase, uint32_t p_list_base, const NextGeoRasterInputs &p_raster, RID p_counters, const Size2i &p_internal_size) {
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	const int storage = p_raster.vis64_storage == RendererRD::NEXTGEO_VIS64_BUFFER ? 1 : 0;
	const RD::UniformType vis64_type = storage == 1 ? RD::UNIFORM_TYPE_STORAGE_BUFFER : RD::UNIFORM_TYPE_IMAGE;
	const Size2i size(MAX(p_internal_size.x, 1), MAX(p_internal_size.y, 1));
	const RID page_buf = RSG::virtual_mesh_storage->get_page_buffer().is_valid() ? RSG::virtual_mesh_storage->get_page_buffer() : dummy_page_buf;
	const RID cluster_buf = RSG::virtual_mesh_storage->get_cluster_buffer().is_valid() ? RSG::virtual_mesh_storage->get_cluster_buffer() : dummy_page_buf;
	const RID bucket_buf = p_raster.bucket_table.is_valid() ? p_raster.bucket_table : dummy_page_buf;
	const RID attr_buf = RSG::virtual_mesh_storage->get_instance_attributes_buffer().is_valid() ? RSG::virtual_mesh_storage->get_instance_attributes_buffer() : dummy_page_buf;
	const RID counters_buf = p_counters.is_valid() ? p_counters : counters;
	const RID skin_palette_buf = RSG::virtual_mesh_storage->get_skin_palette_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_palette_buffer() : dummy_page_buf;
	const RID skin_inst_buf = RSG::virtual_mesh_storage->get_skin_instance_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_instance_buffer() : dummy_page_buf;
	const RID cluster_skin_buf = RSG::virtual_mesh_storage->get_cluster_skin_buffer().is_valid() ? RSG::virtual_mesh_storage->get_cluster_skin_buffer() : dummy_page_buf;
	const RID wind_params_buf = RSG::virtual_mesh_storage->get_wind_params_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_params_buffer() : dummy_page_buf;
	const RID wind_meshes_buf = RSG::virtual_mesh_storage->get_wind_meshes_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_meshes_buffer() : dummy_page_buf;
	LocalVector<RD::Uniform> geometry_uniforms;
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, RSG::virtual_mesh_storage->get_record_buffer()));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RSG::virtual_mesh_storage->get_mesh_record_buffer()));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, page_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, cluster_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, clusters));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, raster_params));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, bucket_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, RSG::virtual_mesh_storage->get_spline_buffer()));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, skin_palette_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, skin_inst_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, cluster_skin_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, attr_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, wind_params_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, wind_meshes_buf));
	LocalVector<RD::Uniform> bin_uniforms;
	bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, raster_bin_lists));
	bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, raster_bin_state));
	bin_uniforms.push_back(RD::Uniform(vis64_type, 2, p_raster.vis64));
	bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, counters_buf));

	struct BinPush {
		uint32_t list_base;
		uint32_t phase;
		uint32_t bin;
		uint32_t flags;
		int32_t size[2];
		uint32_t pad[2]; // pad[1] = floatBitsToUint(vertex_offset_max) (#164)
	};
	static_assert(sizeof(BinPush) == 32, "BinPush must match raster_material_inc.glsl's RasterBinPush.");

	struct TessPush {
		uint32_t list_base;
		uint32_t phase;
		uint32_t bin;
		uint32_t flags;
		int32_t size[2];
		uint32_t cluster_capacity;
		uint32_t wpo_max_bits;
		float dicing_rate;
		float px_scale;
		float sw_max_edge;
		float z_near;
		float disp_magnitude;
		float disp_center;
		uint32_t hw_bin_base;
		uint32_t hw_bin_capacity;
		uint32_t patch_capacity;
		uint32_t tess_pad[3];
	};
	static_assert(sizeof(TessPush) == 80, "TessPush must match raster_material_inc.glsl's NEXTGEO_TESSELLATION RasterBinPush.");

	// The sets of one form of a bin's raster target; false when a set cannot be made.
	auto bind_sets = [&](RID p_shader, const NextGeoRasterBinProgram &p_program, RID r_sets[4]) -> bool {
		r_sets[0] = uniform_set_cache->get_cache_vec(p_shader, 0, geometry_uniforms);
		r_sets[1] = uniform_set_cache->get_cache_vec(p_shader, 1, p_raster.environment_uniforms);
		r_sets[2] = uniform_set_cache->get_cache_vec(p_shader, 2, bin_uniforms);
		r_sets[3] = p_program.material_uniforms.is_empty() ? RID() : uniform_set_cache->get_cache_vec(p_shader, 3, p_program.material_uniforms);
		return r_sets[0].is_valid() && r_sets[1].is_valid() && r_sets[2].is_valid() && (p_program.material_uniforms.is_empty() || r_sets[3].is_valid());
	};

	const uint32_t base_bin_flags = p_raster.debug_backface ? 1u : 0u;
	auto make_bin_push = [&](const NextGeoRasterBinProgram &p_program) -> BinPush {
		const bool has_wpo = (p_program.features & RendererVirtualMeshStorage::RASTER_FEATURE_VERTEX_OFFSET) != 0u;
		uint32_t flags = base_bin_flags;
		uint32_t wpo_bits = 0u;
		if (has_wpo) {
			flags |= (1u << 1u) | (1u << 2u); // NEXTGEO_RASTER_BIN_FLAG_VERTEX_OFFSET | COUNT_CLAMPED
			const float limit = MAX(p_program.vertex_offset_max, 0.0f);
			memcpy(&wpo_bits, &limit, sizeof(uint32_t));
		}
		return BinPush{ p_list_base, p_phase, p_program.bin, flags, { size.x, size.y }, { 0u, wpo_bits } };
	};

	auto is_tess_program = [&](const NextGeoRasterBinProgram &p_program) -> bool {
		return tess_active &&
				(p_program.features & RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) != 0u &&
				p_program.tess_dice_shader[storage].is_valid() &&
				p_program.tess_hw_shader[storage].is_valid();
	};

	uint32_t active_tess_bins = 0u;
	if (tess_active) {
		for (const NextGeoRasterBinProgram &program : p_raster.raster_bin_programs) {
			if (is_tess_program(program)) {
				active_tess_bins++;
			}
		}
		if (active_tess_bins > 0u) {
			uint32_t hw_args_init[RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX * 4u] = {};
			for (uint32_t b = 0; b < RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX; b++) {
				hw_args_init[b * 4u + 0u] = RendererRD::NEXTGEO_RASTER_VERTEX_COUNT;
			}
			rd->buffer_update(tess_hw_args, 0, sizeof(hw_args_init), hw_args_init);
			// Clear only hw_bin_alloc[0..63] per phase; preserve patch_count and diced_patches across Phase 1 & Phase 2.
			rd->buffer_clear(tess_state, offsetof(RendererRD::NextGeoTessState, hw_bin_alloc), sizeof(uint32_t) * 64u);
		}
	}

	LocalVector<RD::Uniform> tess_dice_bin_uniforms;
	LocalVector<RD::Uniform> tess_hw_bin_uniforms;
	if (active_tess_bins > 0u) {
		tess_dice_bin_uniforms = bin_uniforms;
		tess_dice_bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, tess_diced_patches));
		tess_dice_bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, tess_hw_patches));
		tess_dice_bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, tess_state));
		tess_dice_bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, tess_hw_args));

		tess_hw_bin_uniforms = bin_uniforms;
		tess_hw_bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, tess_diced_patches));
		tess_hw_bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, tess_hw_patches));
	}

	auto bind_tess_sets = [&](RID p_shader, const NextGeoRasterBinProgram &p_program, const LocalVector<RD::Uniform> &p_set2, RID r_sets[4]) -> bool {
		r_sets[0] = uniform_set_cache->get_cache_vec(p_shader, 0, geometry_uniforms);
		r_sets[1] = uniform_set_cache->get_cache_vec(p_shader, 1, p_raster.environment_uniforms);
		r_sets[2] = uniform_set_cache->get_cache_vec(p_shader, 2, p_set2);
		r_sets[3] = p_program.material_uniforms.is_empty() ? RID() : uniform_set_cache->get_cache_vec(p_shader, 3, p_program.material_uniforms);
		return r_sets[0].is_valid() && r_sets[1].is_valid() && r_sets[2].is_valid() && (p_program.material_uniforms.is_empty() || r_sets[3].is_valid());
	};

	const uint32_t tess_base_flags = base_bin_flags | (p_raster.orthographic ? (1u << 3u) : 0u);
	const float tess_dicing_rate = RendererRD::nextgeo_tessellation_clamp_dicing_rate(p_raster.tessellation_dicing_rate);
	const float tess_px_scale = RendererRD::nextgeo_sw_raster_px_scale(p_raster.proj, float(size.x), float(size.y), p_raster.orthographic);
	const float tess_sw_max_edge = RendererRD::nextgeo_sw_raster_switch_px(p_raster.sw_raster_max_edge_px);
	const uint32_t hw_per_bin = active_tess_bins > 0u ? (RendererRD::NEXTGEO_TESS_HW_PATCH_CAPACITY / active_tess_bins) : 0u;
	auto fill_tess_push = [&](const NextGeoRasterBinProgram &p_program, uint32_t p_tess_bin_idx, uint32_t p_hw_per_bin) -> TessPush {
		const uint32_t bslot = (p_program.bin - 1u) & 0x3Fu;
		const bool has_wpo = (p_program.features & RendererVirtualMeshStorage::RASTER_FEATURE_VERTEX_OFFSET) != 0u;
		uint32_t flags = tess_base_flags | (bslot << 16u);
		uint32_t wpo_bits = 0u;
		if (has_wpo) {
			flags |= (1u << 1u) | (1u << 2u);
			const float limit = MAX(p_program.vertex_offset_max, 0.0f);
			memcpy(&wpo_bits, &limit, sizeof(uint32_t));
		}
		TessPush push = {};
		push.list_base = p_list_base;
		push.phase = p_phase;
		push.bin = p_program.bin;
		push.flags = flags;
		push.size[0] = size.x;
		push.size[1] = size.y;
		push.cluster_capacity = CLUSTER_CAPACITY;
		push.wpo_max_bits = wpo_bits;
		push.dicing_rate = tess_dicing_rate;
		push.px_scale = tess_px_scale;
		push.sw_max_edge = tess_sw_max_edge;
		push.z_near = p_raster.z_near;
		push.disp_magnitude = p_program.disp_magnitude;
		push.disp_center = p_program.disp_center;
		push.hw_bin_base = p_tess_bin_idx * p_hw_per_bin;
		push.hw_bin_capacity = p_hw_per_bin;
		push.patch_capacity = tess_effective_patch_capacity;
		return push;
	};

	// Software clusters (and tessellation dicing compute): one workgroup each, dispatched indirectly per bin.
	{
		RD::ComputeListID list = rd->compute_list_begin();
		uint32_t tess_bin_idx = 0u;
		for (const NextGeoRasterBinProgram &program : p_raster.raster_bin_programs) {
			if (is_tess_program(program)) {
				const RID shader = program.tess_dice_shader[storage];
				const RID pipeline = _raster_bin_compute_pipeline(shader);
				RID sets[4];
				if (pipeline.is_null() || !bind_tess_sets(shader, program, tess_dice_bin_uniforms, sets)) {
					tess_bin_idx++;
					continue;
				}
				const TessPush push = fill_tess_push(program, tess_bin_idx, hw_per_bin);
				rd->compute_list_bind_compute_pipeline(list, pipeline);
				for (uint32_t set = 0; set < 4; set++) {
					if (sets[set].is_valid()) {
						rd->compute_list_bind_uniform_set(list, sets[set], set);
					}
				}
				rd->compute_list_set_push_constant(list, &push, sizeof(push));
				rd->compute_list_dispatch_indirect(list, raster_bin_args, RendererRD::nextgeo_raster_bin_args_word(p_phase, program.bin, RendererRD::NEXTGEO_RASTER_BIN_KIND_SW) * sizeof(uint32_t));
				tess_dispatches_last_frame++;
				tess_bin_idx++;
				continue;
			}
			const RID shader = program.sw_shader[storage];
			if (shader.is_null()) {
				continue;
			}
			const RID pipeline = _raster_bin_compute_pipeline(shader);
			RID sets[4];
			if (pipeline.is_null() || !bind_sets(shader, program, sets)) {
				continue;
			}
			const BinPush push = make_bin_push(program);
			rd->compute_list_bind_compute_pipeline(list, pipeline);
			for (uint32_t set = 0; set < 4; set++) {
				if (sets[set].is_valid()) {
					rd->compute_list_bind_uniform_set(list, sets[set], set);
				}
			}
			rd->compute_list_set_push_constant(list, &push, sizeof(push));
			rd->compute_list_dispatch_indirect(list, raster_bin_args, RendererRD::nextgeo_raster_bin_args_word(p_phase, program.bin, RendererRD::NEXTGEO_RASTER_BIN_KIND_SW) * sizeof(uint32_t));
		}
		rd->compute_list_end();
	}

	// Hardware clusters (and bounded HW diced patch streams): one indirect draw per bin, with no attachments.
	{
		const RID fb = _get_vis64_framebuffer(size);
		if (fb.is_null()) {
			return;
		}
		const RD::FramebufferFormatID format = rd->framebuffer_get_format(fb);
		RD::DrawListID dl = rd->draw_list_begin(fb, RD::DRAW_DEFAULT_ALL);
		uint32_t tess_bin_idx = 0u;
		for (const NextGeoRasterBinProgram &program : p_raster.raster_bin_programs) {
			if (is_tess_program(program)) {
				const RID shader = program.tess_hw_shader[storage];
				const RID pipeline = _raster_bin_render_pipeline(shader, format);
				RID sets[4];
				if (pipeline.is_null() || !bind_tess_sets(shader, program, tess_hw_bin_uniforms, sets)) {
					tess_bin_idx++;
					continue;
				}
				const uint32_t bslot = (program.bin - 1u) & 0x3Fu;
				const TessPush push = fill_tess_push(program, tess_bin_idx, hw_per_bin);
				rd->draw_list_bind_render_pipeline(dl, pipeline);
				for (uint32_t set = 0; set < 4; set++) {
					if (sets[set].is_valid()) {
						rd->draw_list_bind_uniform_set(dl, sets[set], set);
					}
				}
				rd->draw_list_set_push_constant(dl, &push, sizeof(push));
				rd->draw_list_draw_indirect(dl, false, tess_hw_args, bslot * 16u, 1u, 0u);
				tess_bin_idx++;
				continue;
			}
			const bool use_mesh = p_raster.use_mesh_hw_raster && program.hw_mesh_shader[storage].is_valid();
			const RID shader = use_mesh ? program.hw_mesh_shader[storage] : program.hw_shader[storage];
			if (shader.is_null()) {
				continue;
			}
			const RID pipeline = _raster_bin_render_pipeline(shader, format);
			RID sets[4];
			if (pipeline.is_null() || !bind_sets(shader, program, sets)) {
				continue;
			}
			const BinPush push = make_bin_push(program);
			rd->draw_list_bind_render_pipeline(dl, pipeline);
			for (uint32_t set = 0; set < 4; set++) {
				if (sets[set].is_valid()) {
					rd->draw_list_bind_uniform_set(dl, sets[set], set);
				}
			}
			rd->draw_list_set_push_constant(dl, &push, sizeof(push));
			if (use_mesh) {
				rd->draw_list_draw_mesh_tasks_indirect_count(dl, raster_bin_args, RendererRD::nextgeo_raster_bin_hw_mesh_args_word(p_phase, program.bin) * sizeof(uint32_t), raster_bin_hw_counts, RendererRD::nextgeo_raster_bin_hw_count_word(p_phase, program.bin) * sizeof(uint32_t), 1u, 0u);
			} else {
				rd->draw_list_draw_indirect(dl, false, raster_bin_args, RendererRD::nextgeo_raster_bin_args_word(p_phase, program.bin, RendererRD::NEXTGEO_RASTER_BIN_KIND_HW) * sizeof(uint32_t), 1u, 0u);
			}
		}
		rd->draw_list_end();
	}
}

// GodotNext: nextgeo_tessellation (#163, plan §21.4 contract N11). Lazy main-view tessellation buffers;
// 0 buffers are allocated while rendering/nextgeo/experimental/tessellation is false.
bool NextGeoCut::_ensure_tessellation(RenderingDevice *p_rd) {
	if (tess_ready) {
		return true;
	}
	if (p_rd == nullptr) {
		return false;
	}
	if (tess_diced_patches.is_null()) {
		tess_diced_patches = _storage(RendererRD::NEXTGEO_TESS_PATCH_CAPACITY * sizeof(RendererRD::NextGeoDicedPatchRecord));
	}
	if (tess_hw_patches.is_null()) {
		tess_hw_patches = _storage(RendererRD::NEXTGEO_TESS_HW_PATCH_CAPACITY * sizeof(uint32_t));
	}
	if (tess_state.is_null()) {
		tess_state = _storage(sizeof(RendererRD::NextGeoTessState));
	}
	if (tess_hw_args.is_null()) {
		tess_hw_args = p_rd->storage_buffer_create(RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX * 4u * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	for (int ring = 0; ring < 3; ring++) {
		if (tess_stats_ring[ring].is_null()) {
			tess_stats_ring[ring] = _storage(RendererRD::NEXTGEO_TESS_STATS_WORDS * sizeof(uint32_t));
		}
		tess_stats_ring_pending[ring] = false;
	}
	tess_ready = tess_diced_patches.is_valid() && tess_hw_patches.is_valid() && tess_state.is_valid() && tess_hw_args.is_valid() &&
			tess_stats_ring[0].is_valid() && tess_stats_ring[1].is_valid() && tess_stats_ring[2].is_valid();
	if (!tess_ready) {
		_free_tessellation_resources();
	}
	return tess_ready;
}

void NextGeoCut::_free_tessellation_resources() {
	RenderingDevice *rd = RD::get_singleton();
	for (int ring = 0; ring < 3; ring++) {
		tess_stats_ring_pending[ring] = false;
	}
	if (rd != nullptr) {
		for (RID *buf : { &tess_diced_patches, &tess_hw_patches, &tess_state, &tess_hw_args, &tess_stats_ring[0], &tess_stats_ring[1], &tess_stats_ring[2] }) {
			if (buf->is_valid()) {
				rd->free_rid(*buf);
				*buf = RID();
			}
		}
	}
	tess_ready = false;
	tess_active = false;
	tess_effective_patch_capacity = 0;
	tess_dispatches_last_frame = 0;
	tess_have_gpu_stats = false;
	memset(tess_last_gpu_stats, 0, sizeof(tess_last_gpu_stats));
}

// GodotNext: nextgeo_voxels (#165, plan §21.4 contract N10). Lazy main-view voxel buffers and
// SW splat pipelines; 0 buffers are allocated while rendering/nextgeo/experimental/voxels is false.
bool NextGeoCut::_ensure_voxels(RenderingDevice *p_rd) {
	if (voxels_ready) {
		return true;
	}
	if (p_rd == nullptr || !hybrid_ready) {
		return false;
	}
	const bool image_int64 = p_rd->has_feature(RD::SUPPORTS_IMAGE_INT64_ATOMICS);
	const bool buffer_int64 = p_rd->has_feature(RD::SUPPORTS_BUFFER_INT64_ATOMICS);
#ifdef REAL_T_IS_DOUBLE
	const String base_defines = "\n#define REAL_T_IS_DOUBLE\n";
#else
	const String base_defines = "";
#endif
	if (voxel_shader.get_variant_count() == 0) {
		voxel_shader.initialize(Vector<String>({
				base_defines,
				base_defines + "\n#define VIS64_BUFFER\n",
		}));
	}
	for (int storage = 0; storage < 2; storage++) {
		voxel_shader.set_variant_enabled(storage, storage == 0 ? image_int64 : buffer_int64);
	}
	if (voxel_version.is_null()) {
		voxel_version = voxel_shader.version_create();
	}
	if (voxel_bricks.is_null()) {
		voxel_bricks = _storage(RendererRD::NEXTGEO_VOXEL_BRICK_CAPACITY * sizeof(RendererRD::NextGeoVisibleVoxelBrick));
	}
	if (voxel_raster_args.is_null()) {
		voxel_raster_args = p_rd->storage_buffer_create(8u * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	bool any_pipe = false;
	if (voxel_version.is_valid()) {
		for (int storage = 0; storage < 2; storage++) {
			const bool supported = storage == 0 ? image_int64 : buffer_int64;
			if (!supported) {
				continue;
			}
			const RID sh = voxel_shader.version_get_shader(voxel_version, storage);
			if (sh.is_valid()) {
				voxel_pipelines[storage] = p_rd->compute_pipeline_create(sh);
				if (voxel_pipelines[storage].is_valid()) {
					any_pipe = true;
				}
			}
		}
	}
	voxels_ready = any_pipe && voxel_bricks.is_valid() && voxel_raster_args.is_valid();
	if (!voxels_ready) {
		_free_voxel_resources();
	}
	return voxels_ready;
}

void NextGeoCut::_free_voxel_resources() {
	RenderingDevice *rd = RD::get_singleton();
	for (int storage = 0; storage < 2; storage++) {
		voxel_pipelines[storage] = RID();
	}
	if (voxel_version.is_valid()) {
		voxel_shader.version_free(voxel_version);
		voxel_version = RID();
	}
	if (rd != nullptr) {
		for (RID *buf : { &voxel_bricks, &voxel_raster_args }) {
			if (buf->is_valid()) {
				rd->free_rid(*buf);
				*buf = RID();
			}
		}
	}
	voxels_ready = false;
	voxels_active = false;
	voxel_dispatches_last_frame = 0;
}

void NextGeoCut::_readback_tess_stats_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot) {
	NextGeoCut *self = nullptr;
	{
		MutexLock lock(s_page_req_registry_mutex);
		NextGeoCut **found = s_page_req_registry.getptr(p_token);
		if (found != nullptr) {
			self = *found;
		}
		s_page_req_registry.erase(p_token);
	}
	if (self != nullptr) {
		self->pending_readback_tokens.erase(p_token);
		self->_publish_tess_stats(p_data, p_ring_slot);
	}
}

void NextGeoCut::_publish_tess_stats(const PackedByteArray &p_data, uint32_t p_ring_slot) {
	uint32_t cap = RendererRD::NEXTGEO_TESS_PATCH_CAPACITY;
	if (p_ring_slot < 3u) {
		tess_stats_ring_pending[p_ring_slot] = false;
		cap = tess_stats_ring_cap[p_ring_slot];
	}
	const uint32_t expected = RendererRD::NEXTGEO_TESS_STATS_WORDS * sizeof(uint32_t);
	if (uint32_t(p_data.size()) < expected) {
		return;
	}
	memcpy(tess_last_gpu_stats, p_data.ptr(), expected);
	// Clamp the atomic allocator cursor to the frame's effective patch buffer capacity.
	if (tess_last_gpu_stats[0] > cap) {
		tess_last_gpu_stats[0] = cap;
	}
	tess_have_gpu_stats = true;
	MutexLock lock(s_stats_mutex);
	memcpy(s_tess_gpu_stats, tess_last_gpu_stats, expected);
	s_tess_have_gpu_stats = true;
}

void NextGeoCut::_request_tess_stats(uint32_t p_ring_slot) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_ring_slot >= 3u || tess_stats_ring_pending[p_ring_slot] || tess_stats_ring[p_ring_slot].is_null() || tess_state.is_null()) {
		return;
	}
	tess_stats_ring_cap[p_ring_slot] = tess_effective_patch_capacity;
	const uint32_t bytes = RendererRD::NEXTGEO_TESS_STATS_WORDS * sizeof(uint32_t);
	rd->buffer_copy(tess_state, tess_stats_ring[p_ring_slot], 0, 0, bytes);
	uint64_t token = 0;
	{
		MutexLock lock(s_page_req_registry_mutex);
		token = s_page_req_next_token++;
		s_page_req_registry[token] = this;
	}
	pending_readback_tokens.push_back(token);
	tess_stats_ring_pending[p_ring_slot] = true;
	if (rd->buffer_get_data_async(tess_stats_ring[p_ring_slot], callable_mp_static(&NextGeoCut::_readback_tess_stats_static).bind(token, p_ring_slot), 0, bytes) != OK) {
		tess_stats_ring_pending[p_ring_slot] = false;
		pending_readback_tokens.erase(token);
		MutexLock lock(s_page_req_registry_mutex);
		s_page_req_registry.erase(token);
	}
}

void NextGeoCut::_release_hiz() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	for (int texture = 0; texture < 2; texture++) {
		for (uint32_t mip = 0; mip < HIZ_MAX_MIPS; mip++) {
			if (hiz_views[texture][mip].is_valid()) {
				rd->free_rid(hiz_views[texture][mip]);
				hiz_views[texture][mip] = RID();
			}
		}
		if (hiz_texture[texture].is_valid()) {
			rd->free_rid(hiz_texture[texture]);
			hiz_texture[texture] = RID();
		}
	}
	hiz_mips = 0;
	hiz_size = Size2i();
	hiz_primed = false;
}

void NextGeoCut::_ensure_hiz(const Size2i &p_internal_size) {
	const Size2i base(MAX(p_internal_size.x / 2, 1), MAX(p_internal_size.y / 2, 1));
	if (hiz_texture[0].is_valid() && hiz_texture[1].is_valid() && hiz_size == base) {
		return;
	}
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	_release_hiz();
	uint32_t mips = 1;
	int width = base.x;
	int height = base.y;
	while ((width > 1 || height > 1) && mips < HIZ_MAX_MIPS) {
		width = MAX(width / 2, 1);
		height = MAX(height / 2, 1);
		mips++;
	}

	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R32_SFLOAT;
	format.width = base.x;
	format.height = base.y;
	format.mipmaps = mips;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	for (int texture = 0; texture < 2; texture++) {
		hiz_texture[texture] = rd->texture_create(format, RD::TextureView());
		if (!hiz_texture[texture].is_valid()) {
			_release_hiz();
			return;
		}
		for (uint32_t mip = 0; mip < mips; mip++) {
			hiz_views[texture][mip] = rd->texture_create_shared_from_slice(RD::TextureView(), hiz_texture[texture], 0, mip, 1);
		}
	}
	hiz_mips = mips;
	hiz_size = base;
	hiz_primed = false;
}

void NextGeoCut::_build_hiz(RID p_depth, const Size2i &p_internal_size) {
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RID shader = hiz_shader.version_get_shader(hiz_version, 0);
	Size2i source_size = p_internal_size;
	for (uint32_t mip = 0; mip < hiz_mips; mip++) {
		const Size2i target_size(MAX(source_size.x / 2, 1), MAX(source_size.y / 2, 1));
		const RID source = mip == 0 ? p_depth : hiz_views[1][mip - 1];
		RD::Uniform u_src(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ dummy_sampler, source }));
		RD::Uniform u_dst(RD::UNIFORM_TYPE_IMAGE, 1, hiz_views[1][mip]);
		RID set = uniform_set_cache->get_cache(shader, 0, u_src, u_dst);
		if (!set.is_valid()) {
			return;
		}
		const uint32_t push[4] = { uint32_t(MAX(source_size.x, 1)), uint32_t(MAX(source_size.y, 1)), uint32_t(target_size.x), uint32_t(target_size.y) };
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, hiz_pipeline);
		rd->compute_list_bind_uniform_set(list, set, 0);
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		rd->compute_list_dispatch(list, (target_size.x + 7) / 8, (target_size.y + 7) / 8, 1);
		rd->compute_list_end();
		source_size = target_size;
	}
}

void NextGeoCut::_clear_visibility_targets(RID p_visibility, RID p_instance_id, const Size2i &p_internal_size) {
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RID shader = hiz_shader.version_get_shader(hiz_version, 1);
	RD::Uniform u_vis(RD::UNIFORM_TYPE_IMAGE, 0, p_visibility);
	RD::Uniform u_inst(RD::UNIFORM_TYPE_IMAGE, 1, p_instance_id);
	RID set = uniform_set_cache->get_cache(shader, 0, u_vis, u_inst);
	if (!set.is_valid()) {
		return;
	}
	const uint32_t width = uint32_t(MAX(p_internal_size.x, 1));
	const uint32_t height = uint32_t(MAX(p_internal_size.y, 1));
	const uint32_t push[4] = { width, height, 0u, 0u };
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, clear_vis_pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	rd->compute_list_set_push_constant(list, push, sizeof(push));
	rd->compute_list_dispatch(list, (width + 7u) / 8u, (height + 7u) / 8u, 1u);
	rd->compute_list_end();
}

bool NextGeoCut::_draw_raster_phase(uint32_t p_phase, uint32_t p_list_base, const NextGeoRasterInputs &p_raster) {
	RenderingDevice *rd = RD::get_singleton();
	FramebufferCacheRD *fb_cache = FramebufferCacheRD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (rd == nullptr || fb_cache == nullptr || uniform_set_cache == nullptr) {
		return false;
	}
	const bool with_velocity = p_raster.velocity.is_valid();
	const int variant = (with_velocity ? 1 : 0) | (p_raster.debug_backface ? 2 : 0);
	RID fb = with_velocity ? fb_cache->get_cache(p_raster.visibility, p_raster.instance_id, p_raster.velocity, p_raster.depth) : fb_cache->get_cache(p_raster.visibility, p_raster.instance_id, p_raster.depth);
	if (!fb.is_valid()) {
		return false;
	}
	RID pipeline = raster_pipelines[variant].get_render_pipeline(RD::INVALID_ID, rd->framebuffer_get_format(fb));
	if (!pipeline.is_valid()) {
		return false;
	}
	const RID page_buf = RSG::virtual_mesh_storage->get_page_buffer().is_valid() ? RSG::virtual_mesh_storage->get_page_buffer() : dummy_page_buf;
	const RID cluster_buf = RSG::virtual_mesh_storage->get_cluster_buffer().is_valid() ? RSG::virtual_mesh_storage->get_cluster_buffer() : dummy_page_buf;
	const RID bucket_buf = p_raster.bucket_table.is_valid() ? p_raster.bucket_table : dummy_page_buf;
	const RID skin_palette_buf = RSG::virtual_mesh_storage->get_skin_palette_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_palette_buffer() : dummy_page_buf;
	const RID skin_inst_buf = RSG::virtual_mesh_storage->get_skin_instance_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_instance_buffer() : dummy_page_buf;
	const RID cluster_skin_buf = RSG::virtual_mesh_storage->get_cluster_skin_buffer().is_valid() ? RSG::virtual_mesh_storage->get_cluster_skin_buffer() : dummy_page_buf;
	const RID wind_params_buf = RSG::virtual_mesh_storage->get_wind_params_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_params_buffer() : dummy_page_buf;
	const RID wind_meshes_buf = RSG::virtual_mesh_storage->get_wind_meshes_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_meshes_buffer() : dummy_page_buf;
	RID shader = raster_shader.version_get_shader(raster_version, variant);
	LocalVector<RD::Uniform> raster_u;
	raster_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, RSG::virtual_mesh_storage->get_record_buffer()));
	raster_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RSG::virtual_mesh_storage->get_mesh_record_buffer()));
	raster_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, page_buf));
	raster_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, cluster_buf));
	raster_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, clusters));
	raster_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, raster_params));
	raster_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, bucket_buf));
	raster_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, RSG::virtual_mesh_storage->get_spline_buffer()));
	raster_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, skin_palette_buf));
	raster_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, skin_inst_buf));
	raster_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, cluster_skin_buf));
	raster_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, wind_params_buf));
	raster_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, wind_meshes_buf));
	RID set = uniform_set_cache->get_cache_vec(shader, 0, raster_u);
	if (!set.is_valid()) {
		return false;
	}

	// Load visibility, instance_id and depth; clear velocity in Phase 1 only
	// when the legacy prepass did not already pre-clear it. Never DRAW_IGNORE_ALL.
	BitField<RD::DrawFlags> draw_flags = RD::DRAW_DEFAULT_ALL;
	Vector<Color> clear_colors;
	if (p_phase == 0 && p_raster.clear_velocity && with_velocity) {
		draw_flags = RD::DRAW_CLEAR_COLOR_2;
		clear_colors.push_back(Color(0, 0, 0, 0));
		clear_colors.push_back(Color(0, 0, 0, 0));
		clear_colors.push_back(p_raster.velocity_clear);
	}

	RD::DrawListID dl = rd->draw_list_begin(fb, draw_flags, clear_colors, 0.0f);
	rd->draw_list_bind_render_pipeline(dl, pipeline);
	rd->draw_list_bind_uniform_set(dl, set, 0);
	const uint32_t push[4] = { p_list_base, 0u, 0u, 0u };
	rd->draw_list_set_push_constant(dl, push, sizeof(push));
	rd->draw_list_draw_indirect(dl, false, draw_args, p_phase * sizeof(RendererRD::NextGeoDrawIndirectCommand), 1, 0);
	rd->draw_list_end();
	return true;
}

void NextGeoCut::_swap_hiz() {
	SWAP(hiz_texture[0], hiz_texture[1]);
	for (uint32_t mip = 0; mip < HIZ_MAX_MIPS; mip++) {
		SWAP(hiz_views[0][mip], hiz_views[1][mip]);
	}
}

RID NextGeoCut::_uniform_set(const LocalVector<RD::Uniform> &p_uniforms, int p_variant) {
	return UniformSetCacheRD::get_singleton()->get_cache_vec(cluster_shader.version_get_shader(cluster_version, p_variant), 0, p_uniforms);
}

void NextGeoCut::_set_common_uniforms(LocalVector<RD::Uniform> &r_uniforms, RID p_counters, RID p_hiz_tex, RID p_input, RID p_output, RID p_groups, RID p_clusters, RID p_bvh_args_out, RID p_request_buf, bool p_include_splines) {
	RID records = RSG::virtual_mesh_storage->get_record_buffer();
	RID mesh_records = RSG::virtual_mesh_storage->get_mesh_record_buffer();
	RID dag = RSG::virtual_mesh_storage->get_dag_buffer();
	RID residency_buf = RSG::virtual_mesh_storage->get_residency_buffer().is_valid() ? RSG::virtual_mesh_storage->get_residency_buffer() : dummy_page_buf;
	RID page_buf = RSG::virtual_mesh_storage->get_page_buffer().is_valid() ? RSG::virtual_mesh_storage->get_page_buffer() : dummy_page_buf;
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, records));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, mesh_records));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, dag));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, params));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, residency_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_counters));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, dummy_page_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ dummy_sampler, p_hiz_tex.is_valid() ? p_hiz_tex : dummy_hiz })));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, p_input));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, p_output));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, p_groups));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, p_clusters));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, p_bvh_args_out));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, draw_args));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, page_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, p_request_buf.is_valid() ? p_request_buf : dummy_page_buf));
	if (p_include_splines) {
		r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 26, RSG::virtual_mesh_storage->get_spline_buffer()));
		const RID wind_params_buf = RSG::virtual_mesh_storage->get_wind_params_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_params_buffer() : dummy_page_buf;
		const RID wind_meshes_buf = RSG::virtual_mesh_storage->get_wind_meshes_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_meshes_buffer() : dummy_page_buf;
		r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, wind_params_buf));
		r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, wind_meshes_buf));
	}
}

void NextGeoCut::_set_shadow_uniforms(LocalVector<RD::Uniform> &r_uniforms, RID p_counters, RID p_input, RID p_output, RID p_groups, RID p_clusters, RID p_bvh_args_out, bool p_include_splines) {
	RID records = RSG::virtual_mesh_storage->get_record_buffer();
	RID mesh_records = RSG::virtual_mesh_storage->get_mesh_record_buffer();
	RID dag = RSG::virtual_mesh_storage->get_dag_buffer();
	RID residency_buf = RSG::virtual_mesh_storage->get_residency_buffer().is_valid() ? RSG::virtual_mesh_storage->get_residency_buffer() : dummy_page_buf;
	RID page_buf = RSG::virtual_mesh_storage->get_page_buffer().is_valid() ? RSG::virtual_mesh_storage->get_page_buffer() : dummy_page_buf;
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, records));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, mesh_records));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, dag));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, shadow_params));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, residency_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_counters));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, dummy_page_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ dummy_sampler, dummy_hiz })));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, p_input));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, p_output));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, p_groups));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, p_clusters));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, p_bvh_args_out));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, shadow_draw_args));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, page_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, dummy_page_buf));
	if (p_include_splines) {
		r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 26, RSG::virtual_mesh_storage->get_spline_buffer()));
		const RID wind_params_buf = RSG::virtual_mesh_storage->get_wind_params_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_params_buffer() : dummy_page_buf;
		const RID wind_meshes_buf = RSG::virtual_mesh_storage->get_wind_meshes_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_meshes_buffer() : dummy_page_buf;
		r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, wind_params_buf));
		r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, wind_meshes_buf));
	}
}

// GodotNext: nextgeo_sw_raster (#162). The main-view CLUSTER_CULL variant's raster-bin outputs.
void NextGeoCut::_set_raster_bin_uniforms(LocalVector<RD::Uniform> &r_uniforms) {
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, raster_args.is_valid() ? raster_args : dummy_page_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, raster_counts.is_valid() ? raster_counts : dummy_page_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, raster_lists.is_valid() ? raster_lists : dummy_page_buf));
	// GodotNext: nextgeo_raster_bins (#155). Read only under NEXTGEO_CUT_FLAG_PROGRAMMABLE_BINS.
	const RID cluster_buf = RSG::virtual_mesh_storage->get_cluster_buffer();
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, raster_bins_active && cluster_buf.is_valid() ? cluster_buf : dummy_page_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, raster_bins_active ? raster_bin_table_buffer : dummy_page_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, raster_bins_active ? raster_bin_append : dummy_page_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, raster_bins_active ? raster_bin_state : dummy_page_buf));
	// GodotNext: nextgeo_voxels (#165). Bindings 29..30 (skipping 27..28 reserved for #152).
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, (voxels_active && voxel_bricks.is_valid()) ? voxel_bricks : dummy_page_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 30, (voxels_active && voxel_raster_args.is_valid()) ? voxel_raster_args : dummy_page_buf));
}

void NextGeoCut::_harvest_shadow_timestamps() {
	if (RSG::utilities == nullptr) {
		return;
	}
	const uint64_t ts_frame = RSG::utilities->get_captured_timestamps_frame();
	if (ts_frame == last_harvested_ts_frame) {
		return;
	}
	last_harvested_ts_frame = ts_frame;
	const uint32_t ts_count = RSG::utilities->get_captured_timestamps_count();
	if (ts_count < 2) {
		return;
	}
	bool found_cull = false;
	bool found_raster = false;
	double cull_ms = 0.0;
	double raster_ms = 0.0;
	for (uint32_t i = 0; i + 1 < ts_count; i++) {
		const String name = RSG::utilities->get_captured_timestamp_name(i);
		const bool is_cull = (name == "NextGeo Shadow Cull");
		const bool is_raster = (!is_cull && name == "NextGeo Shadow Raster");
		if (!is_cull && !is_raster) {
			continue;
		}
		const String end_name = is_cull ? "<NextGeo Shadow Cull" : "<NextGeo Shadow Raster";
		for (uint32_t j = i + 1; j < ts_count; j++) {
			if (RSG::utilities->get_captured_timestamp_name(j) == end_name) {
				const uint64_t t0_gpu = RSG::utilities->get_captured_timestamp_gpu_time(i);
				const uint64_t t1_gpu = RSG::utilities->get_captured_timestamp_gpu_time(j);
				double delta_ms = 0.0;
				if (t1_gpu >= t0_gpu && t1_gpu > 0) {
					delta_ms = double(t1_gpu - t0_gpu) / 1000000.0;
				} else {
					const uint64_t t0_cpu = RSG::utilities->get_captured_timestamp_cpu_time(i);
					const uint64_t t1_cpu = RSG::utilities->get_captured_timestamp_cpu_time(j);
					if (t1_cpu >= t0_cpu) {
						delta_ms = double(t1_cpu - t0_cpu) / 1000.0;
					}
				}
				if (is_cull) {
					cull_ms += delta_ms;
					found_cull = true;
				} else {
					raster_ms += delta_ms;
					found_raster = true;
				}
				i = j;
				break;
			}
		}
	}
	if (found_cull || found_raster) {
		MutexLock lock(s_stats_mutex);
		if (found_cull) {
			s_shadow_cull_gpu_ms = cull_ms;
			s_last_stats["shadow_cull_gpu_ms"] = s_shadow_cull_gpu_ms;
		}
		if (found_raster) {
			s_shadow_raster_gpu_ms = raster_ms;
			s_last_stats["shadow_raster_gpu_ms"] = s_shadow_raster_gpu_ms;
		}
	}
}

void NextGeoCut::_request_stats(uint32_t p_ring_slot, RID p_counters_buf, uint32_t p_record_count, uint32_t p_max_levels, const void *p_cut, bool p_occlusion, bool p_have_prev, uint32_t p_raster_draws) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_ring_slot >= 3 || !p_counters_buf.is_valid()) {
		return;
	}
	Dictionary &info = stats_ring_info[p_ring_slot];
	info.clear();
	info["render_frame"] = int64_t(Engine::get_singleton()->get_frames_drawn());
	info["record_count"] = int64_t(p_record_count);
	info["bvh_levels"] = int64_t(p_max_levels);
	info["occlusion"] = p_occlusion;
	info["hiz"] = p_have_prev;
	info["hiz_mips"] = int64_t(hiz_mips);
	info["cluster_capacity"] = int64_t(CLUSTER_CAPACITY);
	info["raster_indirect_draws"] = int64_t(p_raster_draws);
	{
		MutexLock lock(s_stats_mutex);
		info["raster_mode"] = s_raster_mode;
		info["vis64_storage"] = s_vis64_storage;
		info["sw_raster_max_edge_px"] = s_sw_raster_max_edge_px;
		info["hw_raster_path"] = s_hw_raster_path;
		info["hw_raster_reason"] = s_hw_raster_reason;
	}
	info["nextgeo_double_dispatch"] = int64_t(RSG::virtual_mesh_storage->nextgeo_get_double_dispatch());
	info["nextgeo_missing"] = int64_t(RSG::virtual_mesh_storage->nextgeo_get_missing());

	uint32_t cpu_instances = 0;
	uint32_t cpu_groups = 0;
	uint32_t cpu_clusters = 0;
	stats_ring_cpu_hash[p_ring_slot] = CPU_cut_hash(RSG::virtual_mesh_storage, *static_cast<const CutParams *>(p_cut), p_record_count, p_max_levels, cpu_instances, cpu_groups, cpu_clusters);
	info["cpu_instances"] = int64_t(cpu_instances);
	info["cpu_groups"] = int64_t(cpu_groups);
	info["cpu_clusters"] = int64_t(cpu_clusters);
	if (bool(GLOBAL_GET("rendering/nextgeo/hiz_debug")) && hiz_texture[0].is_valid() && hiz_texture[1].is_valid()) {
		for (int texture = 0; texture < 2; texture++) {
			const Vector<uint8_t> raw = rd->texture_get_data(hiz_texture[texture], 0);
			float center = -1.0f;
			if (raw.size() >= int(hiz_size.x * hiz_size.y * 4)) {
				const int index = ((hiz_size.y / 2) * hiz_size.x + hiz_size.x / 2) * 4;
				memcpy(&center, raw.ptr() + index, sizeof(float));
			}
			info[texture == 0 ? "hiz_prev_center" : "hiz_cur_center"] = center;
		}
	}

	{
		MutexLock lock(s_stats_mutex);
		if (!s_last_stats.has("raster_indirect_draws")) {
			s_last_stats["raster_indirect_draws"] = int64_t(p_raster_draws);
		}
	}

	uint64_t token = 0;
	{
		MutexLock lock(s_page_req_registry_mutex);
		token = s_page_req_next_token++;
		s_page_req_registry.insert(token, this);
	}
	pending_readback_tokens.push_back(token);
	counters_ring_pending[p_ring_slot] = true;
	if (rd->buffer_get_data_async(p_counters_buf, callable_mp_static(&NextGeoCut::_readback_cut_stats_static).bind(token, p_ring_slot), 0, sizeof(CutCounters)) != OK) {
		counters_ring_pending[p_ring_slot] = false;
		info.clear();
		pending_readback_tokens.erase(token);
		MutexLock lock(s_page_req_registry_mutex);
		s_page_req_registry.erase(token);
	}
}

void NextGeoCut::_readback_cut_stats_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot) {
	NextGeoCut *cut = nullptr;
	{
		MutexLock lock(s_page_req_registry_mutex);
		NextGeoCut **found = s_page_req_registry.getptr(p_token);
		if (found != nullptr) {
			cut = *found;
			s_page_req_registry.erase(p_token);
		}
	}
	if (cut != nullptr) {
		cut->pending_readback_tokens.erase(p_token);
		cut->_publish_stats(p_data, p_ring_slot);
	}
}

void NextGeoCut::_publish_stats(const PackedByteArray &p_data, uint32_t p_ring_slot) {
	if (p_ring_slot >= 3) {
		return;
	}
	counters_ring_pending[p_ring_slot] = false;
	if (p_data.size() < int(sizeof(CutCounters))) {
		stats_ring_info[p_ring_slot].clear();
		return;
	}
	const CutCounters *stats = reinterpret_cast<const CutCounters *>(p_data.ptr());
	Dictionary info = stats_ring_info[p_ring_slot];
	const uint64_t cpu_hash = stats_ring_cpu_hash[p_ring_slot];
	info["phase1_instances"] = int64_t(stats->phase1);
	info["phase2_instances"] = int64_t(stats->phase2);
	info["groups"] = int64_t(stats->group_count);
	info["clusters"] = int64_t(stats->cluster_count);
	info["phase1_clusters"] = int64_t(stats->phase1_clusters);
	info["phase2_clusters"] = int64_t(stats->phase2_clusters);
	info["overflow"] = int64_t(stats->overflow);
	// GodotNext: nextgeo_sw_raster (#162). Clusters per path of the hybrid raster (0 on the v2.4 path).
	info["sw_clusters"] = int64_t(stats->raster_sw[0]) + int64_t(stats->raster_sw[1]);
	info["hw_clusters"] = int64_t(stats->raster_hw[0]) + int64_t(stats->raster_hw[1]);
	info["sw_clusters_phase1"] = int64_t(stats->raster_sw[0]);
	info["sw_clusters_phase2"] = int64_t(stats->raster_sw[1]);
	info["hw_clusters_phase1"] = int64_t(stats->raster_hw[0]);
	info["hw_clusters_phase2"] = int64_t(stats->raster_hw[1]);
	info["sw_raster_writes"] = int64_t(stats->raster_sw_writes);
	// GodotNext: nextgeo_raster_bins (#155). Clusters of the programmable bins (0 without any).
	const int64_t fixed_sw = int64_t(stats->raster_sw[0]) + int64_t(stats->raster_sw[1]);
	const int64_t fixed_hw = int64_t(stats->raster_hw[0]) + int64_t(stats->raster_hw[1]);
	const int64_t prog_sw = int64_t(stats->raster_bin_sw[0]) + int64_t(stats->raster_bin_sw[1]);
	const int64_t prog_hw = int64_t(stats->raster_bin_hw[0]) + int64_t(stats->raster_bin_hw[1]);
	info["programmable_sw_clusters"] = prog_sw;
	info["programmable_hw_clusters"] = prog_hw;
	// GodotNext: nextgeo_masked_two_sided (#153). Clusters demoted to FIXED past programmable_raster_distance.
	info["fixed_clusters"] = fixed_sw + fixed_hw;
	info["programmable_clusters"] = prog_sw + prog_hw;
	info["programmable_distance_fixed_clusters"] = int64_t(stats->raster_distance_fixed[0]) + int64_t(stats->raster_distance_fixed[1]);
	// GodotNext: nextgeo_vertex_offset (#164). WPO distance-demoted clusters and clamped vertex count.
	info["vertex_offset_distance_fixed_clusters"] = int64_t(stats->vertex_offset_distance_fixed[0]) + int64_t(stats->vertex_offset_distance_fixed[1]);
	info["vertex_offset_clamped"] = int64_t(stats->vertex_offset_clamped);
	// GodotNext: nextgeo_voxels (#165). Visible sparse 4^3 voxel bricks emitted per phase.
	info["voxel_bricks_phase1"] = int64_t(stats->voxel_bricks[0]);
	info["voxel_bricks_phase2"] = int64_t(stats->voxel_bricks[1]);
	info["voxel_bricks"] = int64_t(stats->voxel_bricks[0]) + int64_t(stats->voxel_bricks[1]);
	info["hiz_tested"] = int64_t(stats->pad[0]);
	info["hiz_hit"] = int64_t(stats->pad[1]);
	info["hiz_behind"] = int64_t(stats->cut_lo);
	info["hiz_oob"] = int64_t(stats->cut_hi);
	const uint64_t gpu_hash = (uint64_t(stats->hash_hi) << 32) | uint64_t(stats->hash_lo);
	info["cut_hash"] = int64_t(gpu_hash);
	info["cpu_cut_hash"] = int64_t(cpu_hash);
	info["cut_match"] = gpu_hash == cpu_hash;
	stats_tick++;
	info["tick"] = int64_t(stats_tick);
	{
		MutexLock lock(s_stats_mutex);
		info["resolve_active_buckets"] = int64_t(s_resolve_active_buckets);
		info["resolve_stage_a_draws"] = int64_t(s_resolve_stage_a_draws);
		info["resolve_stage_b_draws"] = int64_t(s_resolve_stage_b_draws);
		info["resolve_stage_b_wrote"] = s_resolve_stage_b_wrote;
		// GodotNext: nextgeo_shading_bins (#158)
		for (const KeyValue<Variant, Variant> &E : s_shading_stats) {
			info[E.key] = E.value;
		}
		info["shadow_views"] = s_last_shadow_views.duplicate(true);
		info["shadow_tick"] = s_last_shadow_tick;
		info["shadow_render_frame"] = s_last_shadow_render_frame;
		info["shadow_views_dispatched"] = int64_t(s_shadow_views_dispatched);
		info["shadow_raster_draws"] = int64_t(s_shadow_raster_draws);
		info["shadow_cull_gpu_ms"] = s_shadow_cull_gpu_ms;
		info["shadow_raster_gpu_ms"] = s_shadow_raster_gpu_ms;
		// GodotNext: nextgeo_tessellation (#163).
		info["tessellation_enabled"] = s_tessellation_enabled;
		info["tessellation_buffers_allocated"] = s_tessellation_buffers_allocated;
		info["tessellation_dicing_dispatches"] = int64_t(s_tessellation_dicing_dispatches);
		info["tessellation_dicing_rate"] = double(s_tessellation_dicing_rate);
		info["tessellation_patches"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[0] : 0u);
		info["dicing_overflow"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[1] : 0u);
		info["tessellation_sw_micro_tris"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[2] : 0u);
		info["tessellation_hw_patches"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[3] : 0u);
		info["tessellation_hw_stream_overflow"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[4] : 0u);
		info["tessellation_coarsened_tris"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[5] : 0u);
		info["tessellation_diced_clusters"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[6] : 0u);
		// GodotNext: nextgeo_voxels (#165).
		s_voxel_bricks_phase1 = stats->voxel_bricks[0];
		s_voxel_bricks_phase2 = stats->voxel_bricks[1];
		info["voxels_enabled"] = s_voxels_enabled;
		info["voxels_buffers_allocated"] = s_voxels_buffers_allocated;
		info["voxel_raster_dispatches"] = int64_t(s_voxel_raster_dispatches);
		s_last_stats = info.duplicate();
	}
	stats_ring_info[p_ring_slot].clear();
}

void NextGeoCut::_request_shadow_stats(uint32_t p_ring_slot, RID p_counters_buf, uint32_t p_view_count, uint32_t p_record_count, uint32_t p_max_levels, const void *p_cuts, const NextGeoShadowViewDesc *p_views) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_ring_slot >= 3 || !p_counters_buf.is_valid() || p_view_count == 0) {
		return;
	}
	ShadowStatsMeta &meta = shadow_stats_ring_meta[p_ring_slot];
	meta.view_count = p_view_count;
	meta.render_frame = int64_t(Engine::get_singleton()->get_frames_drawn());
	meta.cpu_hashes.resize(p_view_count);
	meta.cpu_instances.resize(p_view_count);
	meta.cpu_groups.resize(p_view_count);
	meta.cpu_clusters.resize(p_view_count);
	meta.max_cut_errors.resize(p_view_count);
	meta.light_types.resize(p_view_count);
	meta.passes.resize(p_view_count);
	meta.cut_flags.resize(p_view_count);
	meta.tile_sizes.resize(p_view_count);

	const CutParams *cuts = static_cast<const CutParams *>(p_cuts);
	for (uint32_t v = 0; v < p_view_count; v++) {
		uint32_t cpu_inst = 0;
		uint32_t cpu_grp = 0;
		uint32_t cpu_cls = 0;
		float max_err = 0.0f;
		meta.cpu_hashes[v] = CPU_cut_hash(RSG::virtual_mesh_storage, cuts[v], p_record_count, p_max_levels, cpu_inst, cpu_grp, cpu_cls, &max_err);
		meta.cpu_instances[v] = cpu_inst;
		meta.cpu_groups[v] = cpu_grp;
		meta.cpu_clusters[v] = cpu_cls;
		meta.max_cut_errors[v] = max_err;
		meta.light_types[v] = p_views[v].light_type;
		meta.passes[v] = p_views[v].pass;
		meta.cut_flags[v] = cuts[v].cull[3];
		meta.tile_sizes[v] = p_views[v].tile_size;
	}

	uint64_t token = 0;
	{
		MutexLock lock(s_page_req_registry_mutex);
		token = s_page_req_next_token++;
		s_page_req_registry.insert(token, this);
	}
	pending_readback_tokens.push_back(token);
	shadow_counters_ring_pending[p_ring_slot] = true;
	if (rd->buffer_get_data_async(p_counters_buf, callable_mp_static(&NextGeoCut::_readback_shadow_stats_static).bind(token, p_ring_slot), 0, p_view_count * sizeof(CutCounters)) != OK) {
		shadow_counters_ring_pending[p_ring_slot] = false;
		meta.view_count = 0;
		pending_readback_tokens.erase(token);
		MutexLock lock(s_page_req_registry_mutex);
		s_page_req_registry.erase(token);
	}
}

void NextGeoCut::_readback_shadow_stats_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot) {
	NextGeoCut *cut = nullptr;
	{
		MutexLock lock(s_page_req_registry_mutex);
		NextGeoCut **found = s_page_req_registry.getptr(p_token);
		if (found != nullptr) {
			cut = *found;
			s_page_req_registry.erase(p_token);
		}
	}
	if (cut != nullptr) {
		cut->pending_readback_tokens.erase(p_token);
		cut->_publish_shadow_stats(p_data, p_ring_slot);
	}
}

void NextGeoCut::_publish_shadow_stats(const PackedByteArray &p_data, uint32_t p_ring_slot) {
	if (p_ring_slot >= 3) {
		return;
	}
	shadow_counters_ring_pending[p_ring_slot] = false;
	ShadowStatsMeta &meta = shadow_stats_ring_meta[p_ring_slot];
	const uint32_t view_count = meta.view_count;
	if (view_count == 0 || p_data.size() < int(view_count * sizeof(CutCounters))) {
		meta.view_count = 0;
		return;
	}
	const CutCounters *stats = reinterpret_cast<const CutCounters *>(p_data.ptr());
	Array views_arr;
	views_arr.resize(int(view_count));
	for (uint32_t v = 0; v < view_count; v++) {
		Dictionary vd;
		const CutCounters &st = stats[v];
		const uint64_t gpu_hash = (uint64_t(st.hash_hi) << 32) | uint64_t(st.hash_lo);
		const uint64_t cpu_hash = meta.cpu_hashes[v];
		vd["light_type"] = int64_t(meta.light_types[v]);
		vd["pass"] = int64_t(meta.passes[v]);
		vd["cut_flags"] = int64_t(meta.cut_flags[v]);
		vd["tile_size"] = meta.tile_sizes[v];
		vd["instances"] = int64_t(st.phase1);
		vd["groups"] = int64_t(st.group_count);
		vd["clusters"] = int64_t(st.cluster_count);
		vd["overflow"] = int64_t(st.overflow);
		vd["cpu_instances"] = int64_t(meta.cpu_instances[v]);
		vd["cpu_groups"] = int64_t(meta.cpu_groups[v]);
		vd["cpu_clusters"] = int64_t(meta.cpu_clusters[v]);
		vd["cut_hash"] = int64_t(gpu_hash);
		vd["cpu_cut_hash"] = int64_t(cpu_hash);
		vd["cut_match"] = gpu_hash == cpu_hash;
		vd["max_cut_texel_error"] = double(meta.max_cut_errors[v]);
		views_arr[int(v)] = vd;
	}
	shadow_stats_tick++;
	{
		MutexLock lock(s_stats_mutex);
		s_last_shadow_views = views_arr;
		s_last_shadow_tick = int64_t(shadow_stats_tick);
		s_last_shadow_render_frame = meta.render_frame;
		s_last_stats["shadow_views"] = s_last_shadow_views.duplicate(true);
		s_last_stats["shadow_tick"] = s_last_shadow_tick;
		s_last_stats["shadow_render_frame"] = s_last_shadow_render_frame;
		s_last_stats["shadow_views_dispatched"] = int64_t(s_shadow_views_dispatched);
		s_last_stats["shadow_raster_draws"] = int64_t(s_shadow_raster_draws);
		s_last_stats["shadow_cull_gpu_ms"] = s_shadow_cull_gpu_ms;
		s_last_stats["shadow_raster_gpu_ms"] = s_shadow_raster_gpu_ms;
	}
	meta.view_count = 0;
}

void NextGeoCut::prepare_gpu_records(RendererVirtualMeshStorage *p_storage) {
	if (p_storage == nullptr || !_ensure()) {
		return;
	}
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (rd == nullptr || uniform_set_cache == nullptr) {
		return;
	}
	const RID rec_buf = p_storage->get_record_buffer();
	const RID attr_buf = p_storage->get_instance_attributes_buffer();
	if (rec_buf.is_null() || attr_buf.is_null()) {
		return;
	}

	const uint64_t cur_frame = p_storage->get_frame();
	bool any_multimesh_expanded = false;

	if (multimesh_expand_pipeline.is_valid() && multimesh_expand_params.is_valid() && RSG::mesh_storage != nullptr) {
		struct MultiMeshExpandParamsGPU {
			uint32_t range_base;
			uint32_t range_count;
			uint32_t visible_count;
			uint32_t parent_record;
			uint32_t stride;
			uint32_t color_offset;
			uint32_t custom_offset;
			uint32_t cur_offset;
			uint32_t prev_offset;
			uint32_t flags;
			uint32_t pad0;
			uint32_t pad1;
			float local_aabb_min[4];
			float local_aabb_max[4];
		};
		static_assert(sizeof(MultiMeshExpandParamsGPU) == 80, "MultiMeshExpandParamsGPU must match multimesh_expand.glsl ExpandParams (80 bytes).");

		const RID expand_sh = multimesh_expand_shader.version_get_shader(multimesh_expand_version, 0);
		LocalVector<RendererVirtualMeshStorage::MultiMeshExpandEntry> &entries = p_storage->get_multimesh_expand_entries();
		for (uint32_t i = 0; i < entries.size(); i++) {
			RendererVirtualMeshStorage::MultiMeshExpandEntry &entry = entries[i];
			if (!entry.active || entry.range_count == 0u || entry.multimesh.is_null()) {
				continue;
			}
			RendererMeshStorage::MultiMeshNextGeoInfo info;
			if (!RSG::mesh_storage->multimesh_get_nextgeo_info(entry.multimesh, info) || !info.eligible || info.buffer.is_null() || info.stride < 12u || info.instance_count == 0u) {
				continue;
			}
			const bool need_cur = entry.dirty || info.dirty_serial != entry.last_serial;
			const bool need_settle = !need_cur && entry.dirty_last && entry.last_expand_frame != cur_frame;
			if (!need_cur && !need_settle) {
				continue;
			}
			AABB local_aabb = info.mesh_local_aabb.has_surface() ? info.mesh_local_aabb : RSG::mesh_storage->mesh_get_aabb(entry.mesh, RID());
			if (p_storage->are_assemblies_enabled() && p_storage->mesh_has_assembly(entry.mesh)) {
				const AABB asm_aabb = p_storage->mesh_get_assembly_local_aabb(entry.mesh);
				if (asm_aabb.has_surface()) {
					local_aabb = local_aabb.has_surface() ? local_aabb.merge(asm_aabb) : asm_aabb;
				}
			}
			MultiMeshExpandParamsGPU ep = {};
			ep.range_base = entry.range_base;
			ep.range_count = MIN(entry.range_count, info.instance_count);
			ep.visible_count = MIN(info.visible_count, ep.range_count);
			ep.parent_record = entry.parent_record;
			ep.stride = info.stride;
			ep.color_offset = info.color_offset;
			ep.custom_offset = info.custom_offset;
			ep.cur_offset = info.current_offset;
			ep.prev_offset = info.prev_offset;
			ep.flags = (info.uses_colors ? 1u : 0u) | (info.uses_custom_data ? 2u : 0u) | (need_settle ? 4u : 0u);
			ep.local_aabb_min[0] = float(local_aabb.position.x);
			ep.local_aabb_min[1] = float(local_aabb.position.y);
			ep.local_aabb_min[2] = float(local_aabb.position.z);
			ep.local_aabb_min[3] = 0.0f;
			ep.local_aabb_max[0] = float(local_aabb.position.x + local_aabb.size.x);
			ep.local_aabb_max[1] = float(local_aabb.position.y + local_aabb.size.y);
			ep.local_aabb_max[2] = float(local_aabb.position.z + local_aabb.size.z);
			ep.local_aabb_max[3] = 0.0f;
			rd->buffer_update(multimesh_expand_params, 0, sizeof(ep), &ep);

			LocalVector<RD::Uniform> uniforms;
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, rec_buf));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, attr_buf));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, info.buffer));
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, multimesh_expand_params));
			const RID set = uniform_set_cache->get_cache_vec(expand_sh, 0, uniforms);
			if (set.is_valid()) {
				RD::ComputeListID list = rd->compute_list_begin();
				rd->compute_list_bind_compute_pipeline(list, multimesh_expand_pipeline);
				rd->compute_list_bind_uniform_set(list, set, 0);
				rd->compute_list_dispatch(list, (ep.range_count + 63u) / 64u, 1u, 1u);
				rd->compute_list_end();
				if (need_cur) {
					entry.dirty = false;
					entry.dirty_last = true;
					entry.last_serial = info.dirty_serial;
				} else {
					entry.dirty_last = false;
				}
				entry.last_expand_frame = cur_frame;
				any_multimesh_expanded = true;
			}
		}
	}

	if (p_storage->has_active_assemblies() && instance_assembly_expand_pipeline.is_valid() && assembly_counter_buf.is_valid()) {
		if (any_multimesh_expanded || last_assembly_expand_frame != cur_frame) {
			const uint32_t source_count = p_storage->get_record_count();
			const uint32_t expanded_base = p_storage->get_assembly_expanded_base();
			const uint32_t max_leaves = p_storage->get_assembly_expanded_capacity();
			const RID hdr_buf = p_storage->get_assembly_headers_buffer();
			if (source_count > 0u && max_leaves > 0u && hdr_buf.is_valid()) {
				const RID child_buf = p_storage->get_assembly_children_buffer().is_valid() ? p_storage->get_assembly_children_buffer() : dummy_page_buf;
				const RID part_buf = p_storage->get_assembly_parts_buffer().is_valid() ? p_storage->get_assembly_parts_buffer() : dummy_page_buf;
				const RID bmask_buf = p_storage->get_bucket_surface_masks_buffer().is_valid() ? p_storage->get_bucket_surface_masks_buffer() : dummy_page_buf;

				rd->buffer_clear(rec_buf, uint32_t(expanded_base * sizeof(RendererVirtualMeshStorage::Record)), uint32_t(max_leaves * sizeof(RendererVirtualMeshStorage::Record)));
				rd->buffer_clear(assembly_counter_buf, 0, 16u);

				LocalVector<RD::Uniform> uniforms;
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, rec_buf));
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, attr_buf));
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, hdr_buf));
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, child_buf));
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, part_buf));
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, bmask_buf));
				uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, assembly_counter_buf));
				const RID set = uniform_set_cache->get_cache_vec(instance_shader.version_get_shader(instance_version, 3), 0, uniforms);
				if (set.is_valid()) {
					const uint32_t push[4] = { source_count, expanded_base, max_leaves, p_storage->get_bucket_record_count() };
					RD::ComputeListID list = rd->compute_list_begin();
					rd->compute_list_bind_compute_pipeline(list, instance_assembly_expand_pipeline);
					rd->compute_list_bind_uniform_set(list, set, 0);
					rd->compute_list_set_push_constant(list, push, sizeof(push));
					rd->compute_list_dispatch(list, (source_count + 63u) / 64u, 1u, 1u);
					rd->compute_list_end();
					last_assembly_expand_frame = cur_frame;
				}
			}
		}
	}
}

// GodotNext: nextgeo_dynamic_wind (#296, plan §21.4).
RendererRD::NextGeoDynamicWindFloor NextGeoCut::evaluate_dynamic_wind_floor(const RendererVirtualMeshStorage *p_storage, bool p_setting_enabled) {
	RenderingDevice *rd = RD::get_singleton();
	const bool nextgeo_ok = (p_storage != nullptr && rd != nullptr && !failed);
	const bool tier_ok = nextgeo_ok && RendererRD::nextgeo_raster_tier(rd).hybrid;
	const bool skinned_ok = nextgeo_ok;
	const bool foliage_ok = nextgeo_ok;
	const RendererRD::NextGeoDynamicWindFloor floor = RendererRD::nextgeo_dynamic_wind_evaluate_floor(p_setting_enabled, nextgeo_ok, tier_ok, skinned_ok, foliage_ok);
	dynamic_wind_floor_logger.record_and_log_once(floor.reason, true);
	{
		MutexLock lock(s_stats_mutex);
		s_dynamic_wind_floor_reason = RendererRD::nextgeo_dynamic_wind_reason_name(floor.reason);
	}
	return floor;
}

bool NextGeoCut::needs_dynamic_wind_step(const RendererVirtualMeshStorage *p_storage, bool p_force_based_active) const {
	if (p_storage == nullptr) {
		return false;
	}
	if (!p_force_based_active) {
		return p_storage->get_dynamic_wind_stats().active_slots > 0u || dynamic_wind_ready;
	}
	return last_dynamic_wind_frame != p_storage->get_frame();
}

bool NextGeoCut::_ensure_dynamic_wind(RenderingDevice *p_rd, uint32_t p_slot_count, uint32_t p_force_count) {
	if (p_rd == nullptr || p_slot_count == 0u) {
		return false;
	}
	if (!dynamic_wind_ready) {
		if (dynamic_wind_shader.get_variant_count() == 0) {
			dynamic_wind_shader.initialize(Vector<String>({ "" }));
		}
		if (dynamic_wind_version.is_null()) {
			dynamic_wind_version = dynamic_wind_shader.version_create();
		}
		if (dynamic_wind_version.is_null()) {
			return false;
		}
		const RID sh = dynamic_wind_shader.version_get_shader(dynamic_wind_version, 0);
		if (sh.is_null()) {
			return false;
		}
		dynamic_wind_pipeline = p_rd->compute_pipeline_create(sh);
		if (dynamic_wind_pipeline.is_null()) {
			return false;
		}
		if (dynamic_wind_params_buf.is_null()) {
			Vector<uint8_t> zeros;
			zeros.resize_initialized(sizeof(RendererRD::NextGeoDynamicWindSimParamsGPU));
			dynamic_wind_params_buf = p_rd->storage_buffer_create(sizeof(RendererRD::NextGeoDynamicWindSimParamsGPU), zeros);
			if (dynamic_wind_params_buf.is_null()) {
				return false;
			}
		}
		dynamic_wind_ready = true;
	}

	// Ensure slot descriptor buffer.
	const uint32_t need_slots = MAX(p_slot_count, 1u);
	if (dynamic_wind_slots_buf.is_null() || dynamic_wind_slots_capacity < need_slots) {
		if (dynamic_wind_slots_buf.is_valid()) {
			p_rd->free_rid(dynamic_wind_slots_buf);
			dynamic_wind_slots_buf = RID();
		}
		const uint32_t new_cap = MAX(Math::next_power_of_2(need_slots), 16u);
		Vector<uint8_t> zeros;
		zeros.resize_initialized(new_cap * sizeof(RendererVirtualMeshStorage::DynamicWindSlotDescGPU));
		dynamic_wind_slots_buf = p_rd->storage_buffer_create(zeros.size(), zeros);
		if (dynamic_wind_slots_buf.is_null()) {
			dynamic_wind_slots_capacity = 0u;
			return false;
		}
		dynamic_wind_slots_capacity = new_cap;
	}

	// Ensure ping-pong GPU spring state buffers, preserving existing slot states across growth.
	if (dynamic_wind_state_buf[0].is_null() || dynamic_wind_state_buf[1].is_null() || dynamic_wind_state_capacity < need_slots) {
		const uint32_t old_cap = dynamic_wind_state_capacity;
		const RID old_active = dynamic_wind_state_buf[dynamic_wind_state_ping];
		const RID old_other = dynamic_wind_state_buf[1u - dynamic_wind_state_ping];
		const uint32_t new_cap = MAX(Math::next_power_of_2(need_slots), 16u);
		const uint32_t elem_bytes = RendererVirtualMeshStorage::WIND_MAX_BONES * uint32_t(sizeof(RendererVirtualMeshStorage::DynamicWindBoneStateGPU));
		const uint32_t new_bytes = new_cap * elem_bytes;
		Vector<uint8_t> zeros;
		zeros.resize_initialized(new_bytes);
		RID new_buf0 = p_rd->storage_buffer_create(new_bytes, zeros);
		RID new_buf1 = p_rd->storage_buffer_create(new_bytes, zeros);
		if (new_buf0.is_null() || new_buf1.is_null()) {
			if (new_buf0.is_valid()) {
				p_rd->free_rid(new_buf0);
			}
			if (new_buf1.is_valid()) {
				p_rd->free_rid(new_buf1);
			}
			return false;
		}
		if (old_active.is_valid() && old_cap > 0u) {
			p_rd->buffer_copy(old_active, new_buf0, 0u, 0u, old_cap * elem_bytes);
		}
		if (old_active.is_valid()) {
			p_rd->free_rid(old_active);
		}
		if (old_other.is_valid()) {
			p_rd->free_rid(old_other);
		}
		dynamic_wind_state_buf[0] = new_buf0;
		dynamic_wind_state_buf[1] = new_buf1;
		dynamic_wind_state_capacity = new_cap;
		dynamic_wind_state_ping = 0u;
	}

	// Ensure local wind forces buffer (always >= 1 entry so descriptor binding is valid).
	const uint32_t need_forces = MAX(p_force_count, 1u);
	if (dynamic_wind_forces_buf.is_null() || dynamic_wind_forces_capacity < need_forces) {
		if (dynamic_wind_forces_buf.is_valid()) {
			p_rd->free_rid(dynamic_wind_forces_buf);
			dynamic_wind_forces_buf = RID();
		}
		const uint32_t new_cap = MAX(Math::next_power_of_2(need_forces), 8u);
		Vector<uint8_t> zeros;
		zeros.resize_initialized(new_cap * sizeof(RendererVirtualMeshStorage::WindForceGPU));
		dynamic_wind_forces_buf = p_rd->storage_buffer_create(zeros.size(), zeros);
		if (dynamic_wind_forces_buf.is_null()) {
			dynamic_wind_forces_capacity = 0u;
			return false;
		}
		dynamic_wind_forces_capacity = new_cap;
	}
	return true;
}

void NextGeoCut::_free_dynamic_wind_resources() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr) {
		if (dynamic_wind_version.is_valid()) {
			dynamic_wind_shader.version_free(dynamic_wind_version);
			dynamic_wind_version = RID();
		}
		for (RID *buf : { &dynamic_wind_params_buf, &dynamic_wind_slots_buf, &dynamic_wind_state_buf[0], &dynamic_wind_state_buf[1], &dynamic_wind_forces_buf }) {
			if (buf->is_valid()) {
				rd->free_rid(*buf);
				*buf = RID();
			}
		}
	}
	dynamic_wind_pipeline = RID();
	dynamic_wind_slots_capacity = 0u;
	dynamic_wind_state_capacity = 0u;
	dynamic_wind_state_ping = 0u;
	dynamic_wind_forces_capacity = 0u;
	dynamic_wind_ready = false;
	last_dynamic_wind_frame = UINT64_MAX;
	dynamic_wind_dispatches_last_frame = 0u;
}

void NextGeoCut::dispatch_dynamic_wind(RendererVirtualMeshStorage *p_storage, const Vector3 &p_direction, float p_strength, float p_gust_frequency, float p_time, float p_dt, bool p_force_based_active) {
	if (!p_force_based_active || p_storage == nullptr) {
		if (dynamic_wind_ready || dynamic_wind_params_buf.is_valid()) {
			_free_dynamic_wind_resources();
		}
		dynamic_wind_dispatches_last_frame = 0u;
		MutexLock lock(s_stats_mutex);
		s_dynamic_wind_buffers_allocated = false;
		s_dynamic_wind_dispatches = 0u;
		return;
	}

	const uint64_t cur_frame = p_storage->get_frame();
	if (last_dynamic_wind_frame == cur_frame) {
		return;
	}

	const LocalVector<RendererVirtualMeshStorage::DynamicWindSlotDescGPU> &descs = p_storage->get_dynamic_wind_slot_descs();
	const RID skin_palette_buf = p_storage->get_skin_palette_buffer();
	const RID wind_meshes_buf = p_storage->get_wind_meshes_buffer();
	if (descs.is_empty() || skin_palette_buf.is_null() || wind_meshes_buf.is_null()) {
		dynamic_wind_dispatches_last_frame = 0u;
		last_dynamic_wind_frame = cur_frame;
		MutexLock lock(s_stats_mutex);
		s_dynamic_wind_buffers_allocated = dynamic_wind_ready;
		s_dynamic_wind_dispatches = 0u;
		return;
	}

	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (rd == nullptr || uniform_set_cache == nullptr) {
		return;
	}

	const LocalVector<RendererVirtualMeshStorage::WindForceGPU> &forces = p_storage->get_dynamic_wind_forces();
	if (!_ensure_dynamic_wind(rd, descs.size(), forces.size())) {
		dynamic_wind_dispatches_last_frame = 0u;
		return;
	}

	Vector3 gdir = p_direction;
	if (gdir.length_squared() > real_t(1e-10)) {
		gdir.normalize();
	} else {
		gdir = Vector3(1.0f, 0.0f, 0.0f);
	}
	RendererRD::NextGeoDynamicWindSimParamsGPU sim_params;
	sim_params.wind_dir_strength[0] = float(gdir.x);
	sim_params.wind_dir_strength[1] = float(gdir.y);
	sim_params.wind_dir_strength[2] = float(gdir.z);
	sim_params.wind_dir_strength[3] = MAX(Math::is_finite(p_strength) ? p_strength : 0.0f, 0.0f);
	sim_params.time_dt_gust_pad[0] = Math::is_finite(p_time) ? p_time : 0.0f;
	sim_params.time_dt_gust_pad[1] = CLAMP(Math::is_finite(p_dt) ? p_dt : 0.0f, 0.0f, 0.05f);
	sim_params.time_dt_gust_pad[2] = MAX(Math::is_finite(p_gust_frequency) ? p_gust_frequency : 1.0f, 0.05f);
	sim_params.time_dt_gust_pad[3] = 0.0f;
	sim_params.meta[0] = descs.size();
	sim_params.meta[1] = p_storage->get_wind_params().mesh_count;
	sim_params.meta[2] = forces.size();
	sim_params.meta[3] = 0u;

	rd->buffer_update(dynamic_wind_params_buf, 0u, sizeof(sim_params), &sim_params);
	rd->buffer_update(dynamic_wind_slots_buf, 0u, descs.size() * uint32_t(sizeof(RendererVirtualMeshStorage::DynamicWindSlotDescGPU)), descs.ptr());
	if (!forces.is_empty()) {
		rd->buffer_update(dynamic_wind_forces_buf, 0u, forces.size() * uint32_t(sizeof(RendererVirtualMeshStorage::WindForceGPU)), forces.ptr());
	}

	const uint32_t in_idx = dynamic_wind_state_ping;
	const uint32_t out_idx = 1u - dynamic_wind_state_ping;
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, dynamic_wind_params_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, dynamic_wind_slots_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, wind_meshes_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, dynamic_wind_forces_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, dynamic_wind_state_buf[in_idx]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, dynamic_wind_state_buf[out_idx]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, skin_palette_buf));
	const RID sh = dynamic_wind_shader.version_get_shader(dynamic_wind_version, 0);
	const RID set = uniform_set_cache->get_cache_vec(sh, 0, uniforms);
	if (set.is_valid()) {
		const uint32_t total_threads = descs.size() * RendererVirtualMeshStorage::WIND_MAX_BONES;
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, dynamic_wind_pipeline);
		rd->compute_list_bind_uniform_set(list, set, 0);
		rd->compute_list_dispatch(list, (total_threads + 63u) / 64u, 1u, 1u);
		rd->compute_list_end();

		dynamic_wind_state_ping = out_idx;
		last_dynamic_wind_frame = cur_frame;
		dynamic_wind_dispatches_last_frame = 1u;
		dynamic_wind_total_dispatches++;
	} else {
		dynamic_wind_dispatches_last_frame = 0u;
	}

	{
		MutexLock lock(s_stats_mutex);
		s_dynamic_wind_buffers_allocated = dynamic_wind_ready;
		s_dynamic_wind_dispatches = dynamic_wind_dispatches_last_frame;
		s_dynamic_wind_total_dispatches = dynamic_wind_total_dispatches;
	}
}

bool NextGeoCut::dispatch(RendererVirtualMeshStorage *p_storage, RID p_depth, const Projection &p_view_projection, const Projection &p_prev_view_projection, const Vector3 &p_camera, const Size2i &p_internal_size, uint32_t p_cull_mask, float p_max_pixel_error, float p_lod_bias, bool p_occlusion, bool p_debug_stats, const NextGeoRasterInputs *p_raster, bool p_is_main_view) {
	(void)p_storage;
	if (!_ensure()) {
		return false;
	}
	if (p_is_main_view) {
		_harvest_shadow_timestamps();
	}
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	const uint32_t record_count = RSG::virtual_mesh_storage->get_active_record_count();
	if (record_count == 0 || !RSG::virtual_mesh_storage->get_record_buffer().is_valid() || !RSG::virtual_mesh_storage->get_mesh_record_buffer().is_valid() || !RSG::virtual_mesh_storage->get_dag_buffer().is_valid()) {
		return false;
	}
	uint32_t max_levels = RSG::virtual_mesh_storage->get_max_bvh_levels();
	if (max_levels == 0) {
		return false;
	}

	const bool can_raster = p_raster != nullptr && p_raster->visibility.is_valid() && p_raster->instance_id.is_valid() && p_raster->depth.is_valid();
	// GodotNext: nextgeo_sw_raster (#162). The hybrid raster runs when the caller passes a visibility
	// buffer in a storage form this device built; otherwise the view keeps the v2.4 raster.
	const bool hybrid = can_raster && hybrid_ready && p_raster->vis64.is_valid() &&
			((p_raster->vis64_storage == RendererRD::NEXTGEO_VIS64_IMAGE && vis64_image_ready) || (p_raster->vis64_storage == RendererRD::NEXTGEO_VIS64_BUFFER && vis64_buffer_ready));
	const float sw_raster_switch_px = hybrid ? RendererRD::nextgeo_sw_raster_switch_px(p_raster->sw_raster_max_edge_px) : 0.0f;
	const bool mesh_hw_active = hybrid && p_raster->use_mesh_hw_raster &&
			(p_raster->vis64_storage == RendererRD::NEXTGEO_VIS64_BUFFER ? vis64_mesh_buffer_ready : vis64_mesh_image_ready);
	const RendererRD::NextGeoHWRasterReason hw_reason = mesh_hw_active
			? RendererRD::NEXTGEO_HW_RASTER_REASON_NONE
			: ((hybrid && p_raster->use_mesh_hw_raster) ? RendererRD::NEXTGEO_HW_RASTER_REASON_SHADER_UNAVAILABLE : (p_raster != nullptr ? p_raster->hw_raster_reason : RendererRD::NEXTGEO_HW_RASTER_REASON_AUTO_PENDING_PQ));
	if (p_is_main_view) {
		MutexLock lock(s_stats_mutex);
		s_raster_mode = RendererRD::nextgeo_raster_mode_name(hybrid ? RendererRD::NEXTGEO_RASTER_MODE_HYBRID : RendererRD::NEXTGEO_RASTER_MODE_HARDWARE);
		s_vis64_storage = !hybrid ? "none" : (p_raster->vis64_storage == RendererRD::NEXTGEO_VIS64_BUFFER ? "buffer" : "image");
		s_sw_raster_max_edge_px = sw_raster_switch_px;
		s_hw_raster_path = RendererRD::nextgeo_hw_raster_path_name(mesh_hw_active);
		s_hw_raster_reason = RendererRD::nextgeo_hw_raster_reason_name(hw_reason);
	}
	// GodotNext: nextgeo_raster_bins (#155). Programmable bins run on the hybrid main view only; with no
	// programmable bin this frame, cull and raster stay #162's.
	raster_bins_active = hybrid && p_is_main_view && _prepare_raster_bins(*p_raster);
	// GodotNext: nextgeo_tessellation (#163, plan §21.4 contract N11). Feature-off invariant: when
	// experimental/tessellation is false, allocate 0 dicing buffers and run 0 dicing passes.
	const bool tess_requested = p_raster != nullptr && p_raster->tessellation_enabled;
	if (!tess_requested && tess_ready) {
		_free_tessellation_resources();
	}
	tess_active = false;
	tess_effective_patch_capacity = 0;
	tess_dispatches_last_frame = 0;
	const int vis64_storage_idx = (p_raster != nullptr && p_raster->vis64_storage == RendererRD::NEXTGEO_VIS64_BUFFER) ? 1 : 0;
	bool has_tess_bin = false;
	if (tess_requested && raster_bins_active) {
		for (const NextGeoRasterBinProgram &prog : p_raster->raster_bin_programs) {
			if ((prog.features & RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) != 0u &&
					prog.tess_dice_shader[vis64_storage_idx].is_valid() &&
					prog.tess_hw_shader[vis64_storage_idx].is_valid()) {
				has_tess_bin = true;
				break;
			}
		}
		if (has_tess_bin && _ensure_tessellation(rd)) {
			tess_active = true;
			tess_effective_patch_capacity = p_raster->tessellation_debug_max_patches > 0u
					? MIN(p_raster->tessellation_debug_max_patches, RendererRD::NEXTGEO_TESS_PATCH_CAPACITY)
					: RendererRD::NEXTGEO_TESS_PATCH_CAPACITY;
			rd->buffer_clear(tess_state, 0, sizeof(RendererRD::NextGeoTessState));
		}
	}
	// GodotNext: nextgeo_voxels (#165, plan §21.4 contract N10). Feature-off invariant: when
	// experimental/voxels is false, allocate 0 voxel buffers and run 0 voxel dispatches.
	const bool voxels_setting = p_raster != nullptr && p_raster->voxels_enabled;
	if (!voxels_setting && p_is_main_view && voxels_ready) {
		_free_voxel_resources();
	}
	voxels_active = false;
	if (p_is_main_view) {
		voxel_dispatches_last_frame = 0;
	}
	if (hybrid && p_is_main_view && voxels_setting && _ensure_voxels(rd) && voxel_pipelines[vis64_storage_idx].is_valid()) {
		voxels_active = true;
	}
	if (p_is_main_view) {
		MutexLock lock(s_stats_mutex);
		s_programmable_bins = raster_bins_active ? p_raster->raster_bin_programs.size() : 0u;
		s_tessellation_enabled = tess_requested;
		s_tessellation_buffers_allocated = tess_ready;
		s_tessellation_dicing_rate = p_raster != nullptr ? RendererRD::nextgeo_tessellation_clamp_dicing_rate(p_raster->tessellation_dicing_rate) : RendererRD::NEXTGEO_TESS_DEFAULT_DICING_RATE;
		if (!tess_active) {
			s_tessellation_dicing_dispatches = 0;
			s_tess_have_gpu_stats = false;
			memset(s_tess_gpu_stats, 0, sizeof(s_tess_gpu_stats));
		}
		s_voxels_enabled = voxels_setting;
		s_voxels_buffers_allocated = voxels_ready;
		if (!voxels_active) {
			s_voxel_raster_dispatches = 0;
			s_voxel_bricks_phase1 = 0;
			s_voxel_bricks_phase2 = 0;
		}
	}

	if (can_raster) {
		RasterParams rp = {};
		RendererRD::MaterialStorage::store_camera(p_raster->proj, rp.proj);
		RendererRD::MaterialStorage::store_transform(p_raster->view, rp.view);
		RendererRD::MaterialStorage::store_camera(p_raster->prev_proj, rp.prev_proj);
		RendererRD::MaterialStorage::store_transform(p_raster->prev_view, rp.prev_view);
		rp.taa_jitter[0] = p_raster->taa_jitter.x;
		rp.taa_jitter[1] = p_raster->taa_jitter.y;
		rp.prev_taa_jitter[0] = p_raster->prev_taa_jitter.x;
		rp.prev_taa_jitter[1] = p_raster->prev_taa_jitter.y;
		rp.visible_capacity = CLUSTER_CAPACITY;
		rp.reset_velocity = p_raster->reset_velocity ? 1u : 0u;
		rp.cull_sign = p_raster->reverse_cull ? -1.0f : 1.0f;
		rp.bucket_table_words = p_raster->bucket_table.is_valid() ? p_raster->bucket_table_words : 0u;
		// GodotNext: nextgeo_sw_raster (#162). The export reconstructs each pixel's surface point.
		const Projection view_proj = p_raster->proj * Projection(p_raster->view);
		RendererRD::MaterialStorage::store_camera(view_proj.inverse(), rp.inv_view_proj);
		rp.size[0] = uint32_t(MAX(p_internal_size.x, 1));
		rp.size[1] = uint32_t(MAX(p_internal_size.y, 1));
		rp.raster_region = PHASE2_CLUSTER_BASE;
		rp.patch_capacity = tess_effective_patch_capacity;
		rp.voxel_brick_capacity = voxels_active ? RendererRD::NEXTGEO_VOXEL_BRICK_CAPACITY : 0u;
		rd->buffer_update(raster_params, 0, sizeof(RasterParams), &rp);
		_clear_visibility_targets(p_raster->visibility, p_raster->instance_id, p_internal_size);
		if (hybrid) {
			_clear_vis64(*p_raster, p_internal_size);
			uint32_t args_init[RASTER_ARGS_WORDS] = {};
			for (uint32_t phase = 0; phase < 2; phase++) {
				for (uint32_t bin = 0; bin < RASTER_BIN_COUNT; bin++) {
					// Software: (0, 0, 1) groups until a cluster arrives. Hardware VS: 384 vertices, 0 instances.
					// Hardware mesh tasks (#154): (0, 1, 1, 0) until a HW cluster arrives.
					args_init[raster_args_word(phase, bin, 0u) + 2u] = 1u;
					args_init[raster_args_word(phase, bin, 1u) + 0u] = RendererRD::NEXTGEO_RASTER_VERTEX_COUNT;
					args_init[raster_args_word(phase, bin, 2u) + 1u] = 1u;
					args_init[raster_args_word(phase, bin, 2u) + 2u] = 1u;
				}
			}
			rd->buffer_update(raster_args, 0, sizeof(args_init), args_init);
			const uint32_t counts_init[4] = { 0u, 0u, 0u, 0u };
			rd->buffer_update(raster_counts, 0, sizeof(counts_init), counts_init);
			if (voxels_active && voxel_raster_args.is_valid()) {
				const uint32_t voxel_args_init[8] = { 0u, 0u, 1u, 0u, 0u, 0u, 1u, 0u };
				rd->buffer_update(voxel_raster_args, 0, sizeof(voxel_args_init), voxel_args_init);
			}
		}
	}

	const bool use_occlusion = p_is_main_view && p_occlusion;

	CutParams cut;
	memcpy(cut.view_proj, &p_view_projection.columns[0][0], 64);
	memcpy(cut.prev_view_proj, &p_prev_view_projection.columns[0][0], 64);
	// Gribb-Hartmann planes from the view-projection.
	const Projection &m = p_view_projection;
	Vector4 rows[4];
	for (int col = 0; col < 4; col++) {
		rows[0][col] = m.columns[col][0];
		rows[1][col] = m.columns[col][1];
		rows[2][col] = m.columns[col][2];
		rows[3][col] = m.columns[col][3];
	}
	const Vector4 planes[6] = { rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1], rows[3] - rows[1], rows[2], rows[3] - rows[2] };
	for (int plane = 0; plane < 6; plane++) {
		const Vector4 p = planes[plane];
		const float length = Vector3(p.x, p.y, p.z).length();
		const float inv = length > 0.0f ? 1.0f / length : 0.0f;
		cut.frustum[plane][0] = p.x * inv;
		cut.frustum[plane][1] = p.y * inv;
		cut.frustum[plane][2] = p.z * inv;
		cut.frustum[plane][3] = p.w * inv;
	}
	cut.camera[0] = p_camera.x;
	cut.camera[1] = p_camera.y;
	cut.camera[2] = p_camera.z;
	cut.camera[3] = p_max_pixel_error;
	cut.view[0] = float(p_internal_size.x);
	cut.view[1] = float(p_internal_size.y);
	cut.view[2] = 0.001f;
	cut.view[3] = p_lod_bias;
	cut.cull[0] = p_cull_mask;
	cut.cull[1] = record_count;
	cut.cull[2] = NODE_CAPACITY;
	cut.cull[3] = (use_occlusion ? 1u : 0u) | (p_is_main_view ? 2u : 0u);
	// GodotNext: nextgeo_sw_raster (#162). The triangle-size switch (N2).
	memset(cut.raster_forward, 0, sizeof(cut.raster_forward));
	memset(cut.raster_params, 0, sizeof(cut.raster_params));
	memset(cut.raster_bin_demote_mask, 0, sizeof(cut.raster_bin_demote_mask));
	memset(cut.raster_bin_tess_mask, 0, sizeof(cut.raster_bin_tess_mask));
	if (hybrid) {
		const Vector3 forward = p_raster->camera_forward.normalized();
		cut.raster_forward[0] = forward.x;
		cut.raster_forward[1] = forward.y;
		cut.raster_forward[2] = forward.z;
		cut.raster_forward[3] = p_raster->z_near;
		cut.raster_params[0] = sw_raster_switch_px;
		cut.raster_params[1] = RendererRD::nextgeo_sw_raster_px_scale(p_raster->proj, float(p_internal_size.x), float(p_internal_size.y), p_raster->orthographic);
		cut.raster_params[2] = p_raster->orthographic ? 1.0f : 0.0f;
		cut.cull[3] |= RendererRD::NEXTGEO_CUT_FLAG_HYBRID_RASTER;
		if (raster_bins_active) {
			cut.cull[3] |= RendererRD::NEXTGEO_CUT_FLAG_PROGRAMMABLE_BINS;
			// GodotNext: nextgeo_masked_two_sided (#153), nextgeo_vertex_offset (#164). Mark non-displaced
			// programmable bins demotable past programmable_raster_distance ([0..1]) and vertex_offset_disable_distance ([2..3]).
			for (const NextGeoRasterBinProgram &prog : p_raster->raster_bin_programs) {
				if (prog.bin > 0u && prog.bin < RendererVirtualMeshStorage::RASTER_BIN_LIMIT && (prog.features & RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) == 0u) {
					const uint32_t bit = prog.bin - 1u;
					if ((prog.features & RendererVirtualMeshStorage::RASTER_PIXEL_PROGRAMMABLE_FEATURES) != 0u) {
						cut.raster_bin_demote_mask[bit >> 5u] |= 1u << (bit & 31u);
					}
					if ((prog.features & RendererVirtualMeshStorage::RASTER_FEATURE_VERTEX_OFFSET) != 0u) {
						cut.raster_bin_demote_mask[2u + (bit >> 5u)] |= 1u << (bit & 31u);
					}
				}
			}
			if (tess_active) {
				cut.cull[3] |= RendererRD::NEXTGEO_CUT_FLAG_TESSELLATION;
				cut.raster_params[3] = RendererRD::nextgeo_tessellation_clamp_dicing_rate(p_raster->tessellation_dicing_rate);
				for (const NextGeoRasterBinProgram &prog : p_raster->raster_bin_programs) {
					if (prog.bin > 0u && prog.bin < RendererVirtualMeshStorage::RASTER_BIN_LIMIT &&
							(prog.features & RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) != 0u &&
							prog.tess_dice_shader[vis64_storage_idx].is_valid() &&
							prog.tess_hw_shader[vis64_storage_idx].is_valid()) {
						const uint32_t bit = prog.bin - 1u;
						cut.raster_bin_tess_mask[bit >> 5u] |= 1u << (bit & 31u);
					}
				}
			}
		}
		// GodotNext: nextgeo_voxels (#165). Provisional brick screen threshold V px and visible-brick capacity.
		if (voxels_active) {
			cut.cull[3] |= RendererRD::NEXTGEO_CUT_FLAG_VOXELS;
			const float vthresh = RendererRD::NEXTGEO_VOXEL_PROVISIONAL_SCREEN_THRESHOLD_PX;
			memcpy(&cut.raster_bin_tess_mask[2], &vthresh, sizeof(uint32_t));
			cut.raster_bin_tess_mask[3] = RendererRD::NEXTGEO_VOXEL_BRICK_CAPACITY;
		}
	}
	rd->buffer_update(params, 0, sizeof(CutParams), &cut);

	bool capture_stats = false;
	uint32_t stats_slot = 0;
	RID active_counters = p_is_main_view ? counters : aux_counters;
	if (p_is_main_view && p_debug_stats) {
		const uint32_t stats_interval = MAX(uint32_t(GLOBAL_GET("rendering/nextgeo/debug_stats_interval")), 1u);
		if ((stats_counter++ % stats_interval) == 0) {
			stats_slot = counters_ring_cursor % 3u;
			counters_ring_cursor++;
			if (!counters_ring_pending[stats_slot] && counters_ring[stats_slot].is_valid()) {
				active_counters = counters_ring[stats_slot];
				capture_stats = true;
			}
		}
	}
	CutCounters zero;
	memset(&zero, 0, sizeof(CutCounters));
	rd->buffer_update(active_counters, 0, sizeof(CutCounters), &zero);
	uint32_t args_zero[18 * 4] = {};
	for (uint32_t slot = 0; slot < 18; slot++) {
		args_zero[slot * 4] = 0;
		args_zero[slot * 4 + 1] = 1;
		args_zero[slot * 4 + 2] = 1;
	}
	rd->buffer_update(bvh_args[0], 0, sizeof(args_zero), args_zero);
	rd->buffer_update(bvh_args[1], 0, sizeof(args_zero), args_zero);
	const RendererRD::NextGeoDrawIndirectCommand draw_zero[2] = {
		RendererRD::nextgeo_make_draw_indirect(0),
		RendererRD::nextgeo_make_draw_indirect(0),
	};
	rd->buffer_update(draw_args, 0, sizeof(draw_zero), draw_zero);

	uint32_t ring_slot = 0;
	RID request_buf = dummy_page_buf;
	bool capture_requests = false;
	if (p_is_main_view) {
		ring_slot = request_ring_cursor % 3u;
		request_ring_cursor++;
		if (!request_ring_pending[ring_slot] && request_ring[ring_slot].is_valid()) {
			request_buf = request_ring[ring_slot];
			capture_requests = true;
			const uint32_t req_hdr[4] = { 0u, REQUEST_CAPACITY, 0u, REQUEST_USED_WORDS };
			rd->buffer_clear(request_buf, 16, REQUEST_USED_WORDS * sizeof(uint32_t));
			rd->buffer_update(request_buf, 0, sizeof(req_hdr), req_hdr);
		}
	}

	const bool build_hiz = use_occlusion && p_depth.is_valid();
	if (build_hiz) {
		_ensure_hiz(p_internal_size);
	}
	const bool have_prev = p_is_main_view && hiz_texture[0].is_valid() && hiz_primed;
	const RID prev_hiz_tex = have_prev ? hiz_texture[0] : dummy_hiz;
	const RID residency_buf = RSG::virtual_mesh_storage->get_residency_buffer().is_valid() ? RSG::virtual_mesh_storage->get_residency_buffer() : dummy_page_buf;
	const RID attrs_buf = RSG::virtual_mesh_storage->get_instance_attributes_buffer().is_valid() ? RSG::virtual_mesh_storage->get_instance_attributes_buffer() : dummy_page_buf;
	const RID wind_params_buf = RSG::virtual_mesh_storage->get_wind_params_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_params_buffer() : dummy_page_buf;
	const RID wind_meshes_buf = RSG::virtual_mesh_storage->get_wind_meshes_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_meshes_buffer() : dummy_page_buf;

	// Phase 1 instance cull against previous frame's Hi-Z: level 0 seed.
	RENDER_TIMESTAMP("NextGeo Instance Cull");
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, instance_pipeline);
	{
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, RSG::virtual_mesh_storage->get_record_buffer()));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RSG::virtual_mesh_storage->get_mesh_record_buffer()));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, RSG::virtual_mesh_storage->get_dag_buffer()));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, params));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, residency_buf));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, active_counters));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, nodes));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ dummy_sampler, prev_hiz_tex })));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, phase2_nodes));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, bvh_args[0]));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, attrs_buf));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, wind_params_buf));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, wind_meshes_buf));
		RID set = uniform_set_cache->get_cache_vec(instance_shader.version_get_shader(instance_version, 0), 0, uniforms);
		if (!set.is_valid()) {
			rd->compute_list_end();
			return false;
		}
		rd->compute_list_bind_uniform_set(list, set, 0);
		rd->compute_list_dispatch(list, (record_count + 63) / 64, 1, 1);
	}
	rd->compute_list_end();

	// Phase 1 BVH levels: one dispatch per level, ping-ponging node and indirect-arg buffers.
	const RID skin_palette_buf = RSG::virtual_mesh_storage->get_skin_palette_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_palette_buffer() : dummy_page_buf;
	const RID skin_inst_buf = RSG::virtual_mesh_storage->get_skin_instance_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_instance_buffer() : dummy_page_buf;
	const RID cluster_skin_buf = RSG::virtual_mesh_storage->get_cluster_skin_buffer().is_valid() ? RSG::virtual_mesh_storage->get_cluster_skin_buffer() : dummy_page_buf;
	RENDER_TIMESTAMP("NextGeo BVH");
	RID input = nodes;
	for (uint32_t level = 0; level < max_levels; level++) {
		const RID output = (level % 2 == 0) ? level_a : level_b;
		const RID args_in = bvh_args[level & 1u];
		const RID args_out = bvh_args[(level + 1u) & 1u];
		LocalVector<RD::Uniform> uniforms;
		_set_common_uniforms(uniforms, active_counters, prev_hiz_tex, input, output, groups, clusters, args_out, request_buf);
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, skin_inst_buf));
		RID set = _uniform_set(uniforms, 0);
		if (!set.is_valid()) {
			break;
		}
		list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, cluster_bvh);
		rd->compute_list_bind_uniform_set(list, set, 0);
		const uint32_t push[4] = { level, NODE_CAPACITY, 0u, 0u };
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		rd->compute_list_dispatch_indirect(list, args_in, level * 4 * sizeof(uint32_t));
		rd->compute_list_end();
		input = output;
	}

	// Phase 1 leaf groups to compacted clusters [0, PHASE2_CLUSTER_BASE).
	RENDER_TIMESTAMP("NextGeo Cluster Cull");
	{
		LocalVector<RD::Uniform> uniforms;
		_set_common_uniforms(uniforms, active_counters, prev_hiz_tex, groups, level_a, dummy_page_buf, clusters, bvh_args[0], request_buf);
		_set_raster_bin_uniforms(uniforms);
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, skin_palette_buf));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, skin_inst_buf));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, cluster_skin_buf));
		RID set = _uniform_set(uniforms, 1);
		if (set.is_valid()) {
			list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, cluster_leaf);
			rd->compute_list_bind_uniform_set(list, set, 0);
			const uint32_t push[4] = { 0u, PHASE2_CLUSTER_BASE, 0u, 0u };
			rd->compute_list_set_push_constant(list, push, sizeof(push));
			rd->compute_list_dispatch(list, (GROUP_CAPACITY + 63) / 64, 1, 1);
			rd->compute_list_end();
		}
	}

	uint32_t raster_draws = 0;
	if (can_raster) {
		RENDER_TIMESTAMP("NextGeo Raster P1");
		if (hybrid ? _draw_hybrid_phase(0, 0u, *p_raster, active_counters, p_internal_size, capture_stats) : _draw_raster_phase(0, 0u, *p_raster)) {
			raster_draws++;
		}
	}

	// Build Hi-Z from rendered depth (legacy pre-pass + Phase 1 virtual raster),
	// then cull and rasterize newly disoccluded Phase 2 clusters (§7.3, §7.4).
	RENDER_TIMESTAMP("NextGeo HiZ");
	if (build_hiz && hiz_texture[1].is_valid()) {
		_build_hiz(p_depth, p_internal_size);
	}

	RENDER_TIMESTAMP("NextGeo Phase2 Cull");
	if (build_hiz && hiz_texture[1].is_valid()) {
		LocalVector<RD::Uniform> inst_uniforms;
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, RSG::virtual_mesh_storage->get_record_buffer()));
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RSG::virtual_mesh_storage->get_mesh_record_buffer()));
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, RSG::virtual_mesh_storage->get_dag_buffer()));
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, params));
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, residency_buf));
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, active_counters));
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, nodes));
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ dummy_sampler, hiz_texture[1] })));
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, phase2_nodes));
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, bvh_args[0]));
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, attrs_buf));
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, wind_params_buf));
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, wind_meshes_buf));
		RID inst_set = uniform_set_cache->get_cache_vec(instance_shader.version_get_shader(instance_version, 1), 0, inst_uniforms);
		if (inst_set.is_valid()) {
			list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, instance_phase2);
			rd->compute_list_bind_uniform_set(list, inst_set, 0);
			rd->compute_list_dispatch(list, (NODE_CAPACITY + 63) / 64, 1, 1);
			rd->compute_list_end();
		}

		RID phase2_input = nodes;
		for (uint32_t level = 0; level < max_levels; level++) {
			const RID output = (level % 2 == 0) ? level_a : level_b;
			const RID args_in = bvh_args[level & 1u];
			const RID args_out = bvh_args[(level + 1u) & 1u];
			LocalVector<RD::Uniform> uniforms;
			_set_common_uniforms(uniforms, active_counters, prev_hiz_tex, phase2_input, output, groups, clusters, args_out, request_buf);
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, skin_inst_buf));
			RID set = _uniform_set(uniforms, 0);
			if (!set.is_valid()) {
				break;
			}
			list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, cluster_bvh);
			rd->compute_list_bind_uniform_set(list, set, 0);
			const uint32_t push[4] = { level, NODE_CAPACITY, PHASE2_CLUSTER_BASE, 1u };
			rd->compute_list_set_push_constant(list, push, sizeof(push));
			rd->compute_list_dispatch_indirect(list, args_in, (9u + level) * 4 * sizeof(uint32_t));
			rd->compute_list_end();
			phase2_input = output;
		}

		LocalVector<RD::Uniform> leaf_uniforms;
		_set_common_uniforms(leaf_uniforms, active_counters, hiz_texture[1], groups, level_a, dummy_page_buf, clusters, bvh_args[0], request_buf);
		_set_raster_bin_uniforms(leaf_uniforms);
		leaf_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, skin_palette_buf));
		leaf_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, skin_inst_buf));
		leaf_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, cluster_skin_buf));
		RID leaf_set = _uniform_set(leaf_uniforms, 1);
		if (leaf_set.is_valid()) {
			list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, cluster_leaf);
			rd->compute_list_bind_uniform_set(list, leaf_set, 0);
			const uint32_t push[4] = { 0u, PHASE2_CLUSTER_BASE, PHASE2_CLUSTER_BASE, 1u };
			rd->compute_list_set_push_constant(list, push, sizeof(push));
			rd->compute_list_dispatch(list, (GROUP_CAPACITY + 63) / 64, 1, 1);
			rd->compute_list_end();
		}
	}

	// Enqueue asynchronous readback of main-view page requests & used resident slots.
	if (capture_requests) {
		uint64_t token = 0;
		{
			MutexLock lock(s_page_req_registry_mutex);
			token = s_page_req_next_token++;
			s_page_req_registry.insert(token, this);
		}
		pending_readback_tokens.push_back(token);
		request_ring_pending[ring_slot] = true;
		if (rd->buffer_get_data_async(request_buf, callable_mp_static(&NextGeoCut::_readback_page_requests_static).bind(token, ring_slot), 0, PAGE_REQ_BUFFER_BYTES) != OK) {
			request_ring_pending[ring_slot] = false;
			pending_readback_tokens.erase(token);
			MutexLock lock(s_page_req_registry_mutex);
			s_page_req_registry.erase(token);
		}
	}

	if (can_raster) {
		RENDER_TIMESTAMP("NextGeo Raster P2");
		if (hybrid ? _draw_hybrid_phase(1, PHASE2_CLUSTER_BASE, *p_raster, active_counters, p_internal_size, capture_stats) : _draw_raster_phase(1, PHASE2_CLUSTER_BASE, *p_raster)) {
			raster_draws++;
		}
		if (build_hiz && hiz_texture[1].is_valid()) {
			// Refresh the Hi-Z chain so next frame's Phase 1 includes newly
			// disoccluded Phase 2 virtual depth.
			_build_hiz(p_depth, p_internal_size);
		}
	}

	// Order-independent cut hash on the GPU, so a readback only needs the
	// counters instead of the whole cluster list.
	if (p_is_main_view) {
		{
			MutexLock lock(s_stats_mutex);
			s_tessellation_dicing_dispatches = tess_dispatches_last_frame;
			s_voxel_raster_dispatches = voxel_dispatches_last_frame;
		}
		if (capture_stats) {
			LocalVector<RD::Uniform> uniforms;
			_set_common_uniforms(uniforms, active_counters, prev_hiz_tex, level_b, level_a, groups, clusters, bvh_args[0], dummy_page_buf, false);
			RID set = _uniform_set(uniforms, 2);
			if (set.is_valid()) {
				list = rd->compute_list_begin();
				rd->compute_list_bind_compute_pipeline(list, cluster_hash);
				rd->compute_list_bind_uniform_set(list, set, 0);
				rd->compute_list_dispatch(list, (PHASE2_CLUSTER_BASE + 63) / 64, 1, 1);
				rd->compute_list_end();
			}
			_request_stats(stats_slot, active_counters, record_count, max_levels, &cut, use_occlusion, have_prev, raster_draws);
			if (tess_active) {
				_request_tess_stats(tess_stats_ring_cursor++ % 3u);
			}
		}

		if (build_hiz && hiz_texture[1].is_valid()) {
			_swap_hiz();
			hiz_primed = true;
		}
	}
	return raster_draws > 0;
}

void NextGeoCut::_readback_page_requests_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot) {
	NextGeoCut *cut = nullptr;
	{
		MutexLock lock(s_page_req_registry_mutex);
		NextGeoCut **found = s_page_req_registry.getptr(p_token);
		if (found != nullptr) {
			cut = *found;
			s_page_req_registry.erase(p_token);
		}
	}
	if (cut != nullptr) {
		cut->pending_readback_tokens.erase(p_token);
		cut->_on_page_requests_readback(p_data, p_ring_slot);
	}
}

void NextGeoCut::_on_page_requests_readback(const PackedByteArray &p_data, uint32_t p_ring_slot) {
	if (p_ring_slot < 3) {
		request_ring_pending[p_ring_slot] = false;
	}
	if (RSG::virtual_mesh_storage == nullptr || p_data.size() < 16) {
		return;
	}
	uint32_t hdr[4] = { 0, 0, 0, 0 };
	memcpy(hdr, p_data.ptr(), sizeof(hdr));
	const uint32_t raw_count = hdr[0];
	const uint32_t cap = hdr[1];
	const uint32_t used_words = hdr[3];
	const uint64_t used_end_bytes = 16ull + uint64_t(used_words) * sizeof(uint32_t);
	if (used_words > 0 && used_end_bytes <= uint64_t(p_data.size())) {
		RSG::virtual_mesh_storage->touch_used_page_slots(reinterpret_cast<const uint32_t *>(p_data.ptr() + 16), used_words);
	}
	const uint32_t count = MIN(raw_count, cap);
	if (count == 0 || used_end_bytes + uint64_t(count) * 16ull > uint64_t(p_data.size())) {
		return;
	}
	LocalVector<RendererVirtualMeshStorage::PageRequest> reqs;
	reqs.resize(count);
	const uint8_t *base = p_data.ptr() + used_end_bytes;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t w[4] = { 0, 0, 0, 0 };
		memcpy(w, base + i * 16u, sizeof(w));
		RendererVirtualMeshStorage::PageRequest &req = reqs[i];
		req.visible = (w[0] & 0x80000000u) != 0u;
		req.mesh_index = w[0] & 0x7FFFFFFFu;
		req.page_id = w[1];
		memcpy(&req.distance, &w[2], sizeof(float));
		memcpy(&req.px_error, &w[3], sizeof(float));
	}
	RSG::virtual_mesh_storage->submit_page_requests(reqs.ptr(), reqs.size(), true);
}

bool NextGeoCut::dispatch_shadows(RendererVirtualMeshStorage *p_storage, const NextGeoShadowViewDesc *p_views, uint32_t p_view_count, bool p_debug_stats) {
	(void)p_storage;
	shadow_active_view_count = 0;
	shadow_capture_timestamps = false;
	if (p_views == nullptr || p_view_count == 0) {
		return false;
	}
	if (!_ensure()) {
		return false;
	}
	_harvest_shadow_timestamps();

	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	const uint32_t record_count = RSG::virtual_mesh_storage->get_active_record_count();
	if (record_count == 0 || !RSG::virtual_mesh_storage->get_record_buffer().is_valid() || !RSG::virtual_mesh_storage->get_mesh_record_buffer().is_valid() || !RSG::virtual_mesh_storage->get_dag_buffer().is_valid()) {
		return false;
	}
	const uint32_t max_levels = RSG::virtual_mesh_storage->get_max_bvh_levels();
	if (max_levels == 0) {
		return false;
	}

	const uint32_t view_count = MIN(p_view_count, MAX_SHADOW_VIEWS);
	LocalVector<CutParams> cuts;
	cuts.resize(view_count);
	LocalVector<ShadowRasterParams> srasters;
	srasters.resize(view_count);

	for (uint32_t v = 0; v < view_count; v++) {
		const NextGeoShadowViewDesc &vd = p_views[v];
		Projection correction;
		correction.set_depth_correction(!vd.flip_y);
		const Projection corr_proj = correction * vd.projection;

		CutParams &cut = cuts[v];
		memset(&cut, 0, sizeof(CutParams));
		memcpy(cut.view_proj, &corr_proj.columns[0][0], 64);
		for (int plane = 0; plane < 6; plane++) {
			cut.frustum[plane][0] = vd.frustum[plane].x;
			cut.frustum[plane][1] = vd.frustum[plane].y;
			cut.frustum[plane][2] = vd.frustum[plane].z;
			cut.frustum[plane][3] = vd.frustum[plane].w;
		}
		cut.camera[0] = vd.light_position.x;
		cut.camera[1] = vd.light_position.y;
		cut.camera[2] = vd.light_position.z;
		cut.camera[3] = vd.max_texel_error;
		const uint32_t flags = (vd.cut_flags | RendererRD::NEXTGEO_CUT_FLAG_SHADOW_VIEW) & ~(RendererRD::NEXTGEO_CUT_FLAG_OCCLUSION | RendererRD::NEXTGEO_CUT_FLAG_MAIN_VIEW);
		cut.view[0] = (flags & RendererRD::NEXTGEO_CUT_FLAG_ORTHOGRAPHIC) != 0 ? RendererRD::nextgeo_ortho_inv_texel_size(vd.tile_size, vd.projection) : float(MAX(1, vd.tile_size.x));
		cut.view[1] = float(MAX(1, vd.tile_size.y));
		cut.view[2] = MAX(0.001f, vd.z_near);
		cut.view[3] = vd.lod_bias;
		cut.cull[0] = vd.cull_mask;
		cut.cull[1] = record_count;
		cut.cull[2] = SHADOW_NODES_PER_VIEW;
		cut.cull[3] = flags;

		ShadowRasterParams &sr = srasters[v];
		memset(&sr, 0, sizeof(ShadowRasterParams));
		RendererRD::MaterialStorage::store_camera(corr_proj, sr.proj);
		RendererRD::MaterialStorage::store_transform(vd.transform.affine_inverse(), sr.view);
		sr.z_far = MAX(vd.z_far, 1e-6f);
		sr.dp_side = vd.dual_paraboloid_flip ? -1.0f : 1.0f;
		sr.cull_sign = vd.flip_cull ? -1.0f : 1.0f;
		sr.use_pancake = vd.use_pancake ? 1u : 0u;
		sr.visible_capacity = SHADOW_CLUSTERS_PER_VIEW;
	}

	rd->buffer_update(shadow_params, 0, view_count * sizeof(CutParams), cuts.ptr());
	rd->buffer_update(shadow_raster_params, 0, view_count * sizeof(ShadowRasterParams), srasters.ptr());

	bool capture_stats = false;
	uint32_t stats_slot = 0;
	RID active_counters = shadow_counters;
	if (p_debug_stats) {
		const uint32_t stats_interval = MAX(uint32_t(GLOBAL_GET("rendering/nextgeo/debug_stats_interval")), 1u);
		if ((shadow_stats_counter++ % stats_interval) == 0) {
			stats_slot = shadow_counters_ring_cursor % 3u;
			shadow_counters_ring_cursor++;
			if (!shadow_counters_ring_pending[stats_slot] && shadow_counters_ring[stats_slot].is_valid()) {
				active_counters = shadow_counters_ring[stats_slot];
				capture_stats = true;
			}
		}
	}

	LocalVector<CutCounters> counters_zero;
	counters_zero.resize(view_count);
	memset(counters_zero.ptr(), 0, view_count * sizeof(CutCounters));
	rd->buffer_update(active_counters, 0, view_count * sizeof(CutCounters), counters_zero.ptr());

	LocalVector<uint32_t> args_zero;
	args_zero.resize(view_count * 9u * 4u);
	for (uint32_t slot = 0; slot < view_count * 9u; slot++) {
		args_zero[slot * 4u + 0u] = 0u;
		args_zero[slot * 4u + 1u] = 1u;
		args_zero[slot * 4u + 2u] = 1u;
		args_zero[slot * 4u + 3u] = 0u;
	}
	rd->buffer_update(shadow_bvh_args[0], 0, args_zero.size() * sizeof(uint32_t), args_zero.ptr());
	rd->buffer_update(shadow_bvh_args[1], 0, args_zero.size() * sizeof(uint32_t), args_zero.ptr());

	LocalVector<RendererRD::NextGeoDrawIndirectCommand> draw_zero;
	draw_zero.resize(view_count);
	for (uint32_t v = 0; v < view_count; v++) {
		draw_zero[v] = RendererRD::nextgeo_make_draw_indirect(0);
	}
	rd->buffer_update(shadow_draw_args, 0, view_count * sizeof(RendererRD::NextGeoDrawIndirectCommand), draw_zero.ptr());

	shadow_capture_timestamps = RSG::utilities != nullptr && (p_debug_stats || RSG::utilities->capturing_timestamps);
	if (shadow_capture_timestamps) {
		RSG::utilities->capture_timestamp("NextGeo Shadow Cull");
	}

	const RID residency_buf = RSG::virtual_mesh_storage->get_residency_buffer().is_valid() ? RSG::virtual_mesh_storage->get_residency_buffer() : dummy_page_buf;
	const RID attrs_buf = RSG::virtual_mesh_storage->get_instance_attributes_buffer().is_valid() ? RSG::virtual_mesh_storage->get_instance_attributes_buffer() : dummy_page_buf;
	const RID wind_params_buf = RSG::virtual_mesh_storage->get_wind_params_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_params_buffer() : dummy_page_buf;
	const RID wind_meshes_buf = RSG::virtual_mesh_storage->get_wind_meshes_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_meshes_buffer() : dummy_page_buf;
	{
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, RSG::virtual_mesh_storage->get_record_buffer()));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RSG::virtual_mesh_storage->get_mesh_record_buffer()));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, RSG::virtual_mesh_storage->get_dag_buffer()));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, shadow_params));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, residency_buf));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, active_counters));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, shadow_nodes));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ dummy_sampler, dummy_hiz })));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, dummy_page_buf));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, shadow_bvh_args[0]));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, attrs_buf));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, wind_params_buf));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, wind_meshes_buf));
		RID set = uniform_set_cache->get_cache_vec(instance_shader.version_get_shader(instance_version, 2), 0, uniforms);
		if (!set.is_valid()) {
			if (shadow_capture_timestamps) {
				RSG::utilities->capture_timestamp("<NextGeo Shadow Cull");
			}
			shadow_capture_timestamps = false;
			return false;
		}
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, instance_shadow_pipeline);
		rd->compute_list_bind_uniform_set(list, set, 0);
		rd->compute_list_dispatch(list, (record_count + 63u) / 64u, view_count, 1u);
		rd->compute_list_end();
	}

	const RID skin_palette_buf = RSG::virtual_mesh_storage->get_skin_palette_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_palette_buffer() : dummy_page_buf;
	const RID skin_inst_buf = RSG::virtual_mesh_storage->get_skin_instance_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_instance_buffer() : dummy_page_buf;
	const RID cluster_skin_buf = RSG::virtual_mesh_storage->get_cluster_skin_buffer().is_valid() ? RSG::virtual_mesh_storage->get_cluster_skin_buffer() : dummy_page_buf;
	RID input = shadow_nodes;
	for (uint32_t level = 0; level < max_levels; level++) {
		const RID output = (level % 2 == 0) ? shadow_level_a : shadow_level_b;
		const RID args_in = shadow_bvh_args[level & 1u];
		const RID args_out = shadow_bvh_args[(level + 1u) & 1u];
		LocalVector<RD::Uniform> uniforms;
		_set_shadow_uniforms(uniforms, active_counters, input, output, shadow_groups, shadow_clusters, args_out);
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, skin_inst_buf));
		RID set = _uniform_set(uniforms, 3);
		if (!set.is_valid()) {
			break;
		}
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, cluster_shadow_bvh);
		rd->compute_list_bind_uniform_set(list, set, 0);
		for (uint32_t v = 0; v < view_count; v++) {
			const uint32_t push[4] = { level, SHADOW_NODES_PER_VIEW, SHADOW_GROUPS_PER_VIEW, v };
			rd->compute_list_set_push_constant(list, push, sizeof(push));
			rd->compute_list_dispatch_indirect(list, args_in, (v * 9u + level) * 4u * sizeof(uint32_t));
		}
		rd->compute_list_end();
		input = output;
	}

	{
		LocalVector<RD::Uniform> uniforms;
		_set_shadow_uniforms(uniforms, active_counters, shadow_groups, shadow_level_a, dummy_page_buf, shadow_clusters, shadow_bvh_args[0]);
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, skin_palette_buf));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, skin_inst_buf));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, cluster_skin_buf));
		RID set = _uniform_set(uniforms, 4);
		if (set.is_valid()) {
			RD::ComputeListID list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, cluster_shadow_leaf);
			rd->compute_list_bind_uniform_set(list, set, 0);
			const uint32_t push[4] = { 0u, SHADOW_CLUSTERS_PER_VIEW, SHADOW_GROUPS_PER_VIEW, view_count };
			rd->compute_list_set_push_constant(list, push, sizeof(push));
			rd->compute_list_dispatch(list, (SHADOW_GROUPS_PER_VIEW + 63u) / 64u, view_count, 1u);
			rd->compute_list_end();
		}
	}

	if (capture_stats) {
		LocalVector<RD::Uniform> uniforms;
		_set_shadow_uniforms(uniforms, active_counters, shadow_level_b, shadow_level_a, shadow_groups, shadow_clusters, shadow_bvh_args[0], false);
		RID set = _uniform_set(uniforms, 5);
		if (set.is_valid()) {
			RD::ComputeListID list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, cluster_shadow_hash);
			rd->compute_list_bind_uniform_set(list, set, 0);
			const uint32_t push[4] = { SHADOW_CLUSTERS_PER_VIEW, view_count, 0u, 0u };
			rd->compute_list_set_push_constant(list, push, sizeof(push));
			rd->compute_list_dispatch(list, (SHADOW_CLUSTERS_PER_VIEW + 63u) / 64u, view_count, 1u);
			rd->compute_list_end();
		}
	}

	if (shadow_capture_timestamps) {
		RSG::utilities->capture_timestamp("<NextGeo Shadow Cull");
	}

	shadow_active_view_count = view_count;
	{
		MutexLock lock(s_stats_mutex);
		s_shadow_views_dispatched = view_count;
		s_shadow_raster_draws = 0;
		s_last_stats["shadow_views_dispatched"] = int64_t(s_shadow_views_dispatched);
		s_last_stats["shadow_raster_draws"] = int64_t(0);
	}

	if (capture_stats) {
		_request_shadow_stats(stats_slot, active_counters, view_count, record_count, max_levels, cuts.ptr(), p_views);
	}

	return true;
}

void NextGeoCut::begin_shadow_raster_timestamps() {
	if (shadow_capture_timestamps && RSG::utilities != nullptr) {
		RSG::utilities->capture_timestamp("NextGeo Shadow Raster");
	}
}

void NextGeoCut::end_shadow_raster_timestamps() {
	if (shadow_capture_timestamps && RSG::utilities != nullptr) {
		RSG::utilities->capture_timestamp("<NextGeo Shadow Raster");
	}
}

bool NextGeoCut::draw_shadow_view(uint32_t p_shadow_view, RID p_framebuffer, const Rect2i &p_rect, bool p_dual_paraboloid) {
	if (!initialized || p_shadow_view >= shadow_active_view_count || !p_framebuffer.is_valid()) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (rd == nullptr || uniform_set_cache == nullptr || RSG::virtual_mesh_storage == nullptr) {
		return false;
	}
	const int shadow_variant = p_dual_paraboloid ? 1 : 0;
	RID pipeline = shadow_raster_pipelines[shadow_variant].get_render_pipeline(RD::INVALID_ID, rd->framebuffer_get_format(p_framebuffer));
	if (!pipeline.is_valid()) {
		return false;
	}
	const RID page_buf = RSG::virtual_mesh_storage->get_page_buffer().is_valid() ? RSG::virtual_mesh_storage->get_page_buffer() : dummy_page_buf;
	const RID cluster_buf = RSG::virtual_mesh_storage->get_cluster_buffer().is_valid() ? RSG::virtual_mesh_storage->get_cluster_buffer() : dummy_page_buf;
	const RID skin_palette_buf = RSG::virtual_mesh_storage->get_skin_palette_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_palette_buffer() : dummy_page_buf;
	const RID skin_inst_buf = RSG::virtual_mesh_storage->get_skin_instance_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_instance_buffer() : dummy_page_buf;
	const RID cluster_skin_buf = RSG::virtual_mesh_storage->get_cluster_skin_buffer().is_valid() ? RSG::virtual_mesh_storage->get_cluster_skin_buffer() : dummy_page_buf;
	const RID wind_params_buf = RSG::virtual_mesh_storage->get_wind_params_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_params_buffer() : dummy_page_buf;
	const RID wind_meshes_buf = RSG::virtual_mesh_storage->get_wind_meshes_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_meshes_buffer() : dummy_page_buf;
	RID shader = raster_shader.version_get_shader(raster_version, 4 + shadow_variant);
	LocalVector<RD::Uniform> shadow_u;
	shadow_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, RSG::virtual_mesh_storage->get_record_buffer()));
	shadow_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RSG::virtual_mesh_storage->get_mesh_record_buffer()));
	shadow_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, page_buf));
	shadow_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, cluster_buf));
	shadow_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, shadow_clusters));
	shadow_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, shadow_raster_params));
	shadow_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, dummy_page_buf));
	shadow_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, RSG::virtual_mesh_storage->get_spline_buffer()));
	shadow_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, skin_palette_buf));
	shadow_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, skin_inst_buf));
	shadow_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, cluster_skin_buf));
	shadow_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, wind_params_buf));
	shadow_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, wind_meshes_buf));
	RID set = uniform_set_cache->get_cache_vec(shader, 0, shadow_u);
	if (!set.is_valid()) {
		return false;
	}

	RD::DrawListID dl = rd->draw_list_begin(p_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0.0f, 0, Rect2(p_rect));
	rd->draw_list_bind_render_pipeline(dl, pipeline);
	rd->draw_list_bind_uniform_set(dl, set, 0);
	const uint32_t push[4] = { p_shadow_view * SHADOW_CLUSTERS_PER_VIEW, p_shadow_view, 0u, 0u };
	rd->draw_list_set_push_constant(dl, push, sizeof(push));
	rd->draw_list_draw_indirect(dl, false, shadow_draw_args, p_shadow_view * sizeof(RendererRD::NextGeoDrawIndirectCommand), 1, 0);
	rd->draw_list_end();

	{
		MutexLock lock(s_stats_mutex);
		s_shadow_raster_draws++;
		s_last_stats["shadow_raster_draws"] = int64_t(s_shadow_raster_draws);
	}
	return true;
}

// GodotNext: vsm_nanite (#217, plan §21.3 contract S4, steps 1-7).
// Per-page multi-view cluster cull, hybrid HW/SW rasterization into the VSM physical page pool
// (1 indirect dispatch chain for all directional maps, 1 for all local maps), 1-level 8x8 per-page
// max-depth Hi-Z build, and programmable raster-bin integration shared with #155.
bool NextGeoCut::_ensure_vsm() {
	if (vsm_ready) {
		return true;
	}
	if (vsm_failed) {
		return false;
	}
	if (!_ensure()) {
		vsm_failed = true;
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || !rd->has_feature(RD::SUPPORTS_IMAGE_ATOMIC_32_BIT)) {
		vsm_failed = true;
		return false;
	}
#ifdef REAL_T_IS_DOUBLE
	const String base_defines = "\n#define REAL_T_IS_DOUBLE\n";
#else
	const String base_defines = "";
#endif
	const String vsm_defines = RendererVirtualShadowMapStorage::get_glsl_defines();
	const String vsm_base = base_defines + "\n#define VSM_VIEW\n" + vsm_defines;
	if (vsm_cluster_shader.get_variant_count() == 0) {
		vsm_cluster_shader.initialize(Vector<String>({
				vsm_base + "\n#define VSM_SETUP\n",
				vsm_base + "\n#define VSM_SEED\n",
				vsm_base,
				vsm_base + "\n#define CLUSTER_CULL\n",
		}));
	}
	if (vsm_hw_shader.get_variant_count() == 0) {
		vsm_hw_shader.initialize(Vector<String>({ base_defines + vsm_defines }));
	}
	if (vsm_sw_shader.get_variant_count() == 0) {
		vsm_sw_shader.initialize(Vector<String>({
				base_defines + vsm_defines,
				base_defines + "\n#define MODE_HIZ_BUILD\n" + vsm_defines,
				base_defines + "\n#define MODE_MARK_DYNAMIC_TEST\n" + vsm_defines,
		}));
	}
	vsm_cluster_version = vsm_cluster_shader.version_create();
	vsm_hw_version = vsm_hw_shader.version_create();
	vsm_sw_version = vsm_sw_shader.version_create();
	if (vsm_cluster_version.is_null() || vsm_hw_version.is_null() || vsm_sw_version.is_null()) {
		_free_vsm_resources();
		vsm_failed = true;
		return false;
	}
	const RID setup_sh = vsm_cluster_shader.version_get_shader(vsm_cluster_version, 0);
	const RID seed_sh = vsm_cluster_shader.version_get_shader(vsm_cluster_version, 1);
	const RID bvh_sh = vsm_cluster_shader.version_get_shader(vsm_cluster_version, 2);
	const RID leaf_sh = vsm_cluster_shader.version_get_shader(vsm_cluster_version, 3);
	const RID hw_sh = vsm_hw_shader.version_get_shader(vsm_hw_version, 0);
	const RID sw_sh = vsm_sw_shader.version_get_shader(vsm_sw_version, 0);
	const RID hiz_sh = vsm_sw_shader.version_get_shader(vsm_sw_version, 1);
	const RID dyn_sh = vsm_sw_shader.version_get_shader(vsm_sw_version, 2);
	if (setup_sh.is_null() || seed_sh.is_null() || bvh_sh.is_null() || leaf_sh.is_null() || hw_sh.is_null() || sw_sh.is_null() || hiz_sh.is_null() || dyn_sh.is_null()) {
		_free_vsm_resources();
		vsm_failed = true;
		return false;
	}
	vsm_setup_pipeline = rd->compute_pipeline_create(setup_sh);
	vsm_seed_pipeline = rd->compute_pipeline_create(seed_sh);
	vsm_bvh_pipeline = rd->compute_pipeline_create(bvh_sh);
	vsm_leaf_pipeline = rd->compute_pipeline_create(leaf_sh);
	vsm_sw_pipeline = rd->compute_pipeline_create(sw_sh);
	vsm_hiz_build_pipeline = rd->compute_pipeline_create(hiz_sh);
	vsm_mark_dynamic_pipeline = rd->compute_pipeline_create(dyn_sh);

	RD::PipelineRasterizationState hw_raster_state;
	hw_raster_state.cull_mode = RD::POLYGON_CULL_BACK;
	vsm_hw_pipeline.setup(hw_sh, RD::RENDER_PRIMITIVE_TRIANGLES, hw_raster_state, RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(0));

	vsm_empty_fb = rd->framebuffer_create_empty(Size2i(RendererVirtualShadowMapStorage::PAGE_SIZE, RendererVirtualShadowMapStorage::PAGE_SIZE));
	vsm_params_buffer = _storage(sizeof(RendererRD::NextGeoVSMParams));
	for (uint32_t chain = 0; chain < RendererRD::NEXTGEO_VSM_CHAIN_COUNT; chain++) {
		vsm_counters[chain] = _storage(sizeof(RendererRD::NextGeoVSMCounters));
		vsm_clusters[chain] = _storage(RendererRD::NEXTGEO_VSM_CLUSTER_CAPACITY * 4u * sizeof(uint32_t));
	}
	for (int ring = 0; ring < 3; ring++) {
		vsm_counters_ring[ring] = _storage(RendererRD::NEXTGEO_VSM_CHAIN_COUNT * sizeof(RendererRD::NextGeoVSMCounters));
		vsm_counters_ring_pending[ring] = false;
	}
	vsm_counters_ring_cursor = 0;
	vsm_seed_args = rd->storage_buffer_create(4u * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	vsm_bvh_args[0] = rd->storage_buffer_create(8u * 4u * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	vsm_bvh_args[1] = rd->storage_buffer_create(8u * 4u * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	vsm_group_args = rd->storage_buffer_create(4u * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	// Words 0..3: SW dispatch; words 4..7: HW VS draw; words 8..11: HW mesh-task dispatch (#154).
	vsm_raster_args = rd->storage_buffer_create(12u * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	vsm_raster_counts = rd->storage_buffer_create(4u * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	vsm_nodes_a = _storage(RendererRD::NEXTGEO_VSM_NODE_CAPACITY * 4u * sizeof(uint32_t));
	vsm_nodes_b = _storage(RendererRD::NEXTGEO_VSM_NODE_CAPACITY * 4u * sizeof(uint32_t));
	vsm_groups = _storage(RendererRD::NEXTGEO_VSM_GROUP_CAPACITY * 4u * sizeof(uint32_t));
	vsm_raster_lists = _storage(RendererRD::NEXTGEO_VSM_CLUSTER_CAPACITY * sizeof(uint32_t));

	const bool ok = vsm_setup_pipeline.is_valid() && vsm_seed_pipeline.is_valid() && vsm_bvh_pipeline.is_valid() && vsm_leaf_pipeline.is_valid() &&
			vsm_sw_pipeline.is_valid() && vsm_hiz_build_pipeline.is_valid() && vsm_mark_dynamic_pipeline.is_valid() &&
			vsm_empty_fb.is_valid() && vsm_params_buffer.is_valid() &&
			vsm_counters[0].is_valid() && vsm_counters[1].is_valid() &&
			vsm_clusters[0].is_valid() && vsm_clusters[1].is_valid() &&
			vsm_counters_ring[0].is_valid() && vsm_counters_ring[1].is_valid() && vsm_counters_ring[2].is_valid() &&
			vsm_seed_args.is_valid() && vsm_bvh_args[0].is_valid() && vsm_bvh_args[1].is_valid() &&
			vsm_group_args.is_valid() && vsm_raster_args.is_valid() && vsm_raster_counts.is_valid() &&
			vsm_nodes_a.is_valid() && vsm_nodes_b.is_valid() && vsm_groups.is_valid() && vsm_raster_lists.is_valid();
	if (!ok) {
		_free_vsm_resources();
		vsm_failed = true;
		return false;
	}
	// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9). Build VSM HW task/mesh variant
	// only on mesh-capable devices; non-mesh devices keep the VS VSM HW path without errors.
	vsm_hw_mesh_ready = false;
	const RendererRD::NextGeoHWRasterCapabilities mesh_caps = RendererRD::nextgeo_hw_raster_capabilities_from_device(rd);
	if (RendererRD::nextgeo_select_hw_raster_path(RendererRD::NEXTGEO_HW_RASTER_PATH_MESH, mesh_caps).use_mesh) {
		if (vsm_hw_mesh_shader.get_variant_count() == 0) {
			vsm_hw_mesh_shader.initialize(Vector<String>({ base_defines + vsm_defines }));
		}
		vsm_hw_mesh_version = vsm_hw_mesh_shader.version_create();
		vsm_hw_count_buf = _storage(16u);
		if (vsm_hw_mesh_version.is_valid() && vsm_hw_count_buf.is_valid()) {
			const RID hw_mesh_sh = vsm_hw_mesh_shader.version_get_shader(vsm_hw_mesh_version, 0);
			if (hw_mesh_sh.is_valid()) {
				RD::PipelineRasterizationState hw_mesh_raster_state;
				hw_mesh_raster_state.cull_mode = RD::POLYGON_CULL_BACK;
				vsm_hw_mesh_pipeline.setup(hw_mesh_sh, RD::RENDER_PRIMITIVE_TRIANGLES, hw_mesh_raster_state, RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(0));
				vsm_hw_mesh_ready = true;
			}
		}
	}
	vsm_ready = true;
	// GodotNext: vsm_max_storage_buffer_range (#306). Log device storage-buffer range limit and chosen NextGeo layout once at VSM init.
	{
		const uint64_t max_storage_range = rd->limit_get(RD::LIMIT_MAX_STORAGE_BUFFER_RANGE);
		if (RSG::virtual_mesh_storage != nullptr) {
			const RendererVirtualMeshStorage::PageArenaLayout layout = RSG::virtual_mesh_storage->get_page_arena_layout();
			print_line(vformat("VSM NextGeo: maxStorageBufferRange=%d MiB (%d B), page arena=%d MiB (%d slots x %d KiB, requested %d MiB%s).",
					max_storage_range >> 20, max_storage_range, uint64_t(layout.arena_bytes) >> 20, layout.slot_count,
					RSG::virtual_mesh_storage->get_page_slot_size_bytes() >> 10, layout.requested_bytes >> 20,
					layout.clamped_to_device_limit ? ", clamped to device limit" : ""));
		} else {
			print_line(vformat("VSM NextGeo: maxStorageBufferRange=%d MiB (%d B).", max_storage_range >> 20, max_storage_range));
		}
	}
	return true;
}

bool NextGeoCut::is_vsm_available() {
	return _ensure_vsm();
}

bool NextGeoCut::_ensure_vsm_hiz(uint32_t p_pool_pages, uint32_t p_grid_width, uint64_t p_pool_reallocations) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_pool_pages == 0u || p_grid_width == 0u) {
		return false;
	}
	const bool needs_create = vsm_hiz_texture.is_null() || vsm_hiz_owner.is_null() ||
			vsm_hiz_pool_pages != p_pool_pages || vsm_hiz_grid_width != p_grid_width;
	if (needs_create) {
		if (vsm_hiz_texture.is_valid()) {
			rd->free_rid(vsm_hiz_texture);
			vsm_hiz_texture = RID();
		}
		if (vsm_hiz_owner.is_valid()) {
			rd->free_rid(vsm_hiz_owner);
			vsm_hiz_owner = RID();
		}
		const uint32_t grid_height = (p_pool_pages + p_grid_width - 1u) / p_grid_width;
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R32_UINT;
		tf.texture_type = RD::TEXTURE_TYPE_2D;
		tf.width = p_grid_width * RendererRD::NEXTGEO_VSM_HIZ_PAGE_SIZE;
		tf.height = grid_height * RendererRD::NEXTGEO_VSM_HIZ_PAGE_SIZE;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		vsm_hiz_texture = rd->texture_create(tf, RD::TextureView());
		vsm_hiz_owner = _storage(p_pool_pages * sizeof(uint32_t));
		if (vsm_hiz_texture.is_null() || vsm_hiz_owner.is_null()) {
			if (vsm_hiz_texture.is_valid()) {
				rd->free_rid(vsm_hiz_texture);
				vsm_hiz_texture = RID();
			}
			if (vsm_hiz_owner.is_valid()) {
				rd->free_rid(vsm_hiz_owner);
				vsm_hiz_owner = RID();
			}
			vsm_hiz_pool_pages = 0;
			vsm_hiz_grid_width = 0;
			return false;
		}
		rd->set_resource_name(vsm_hiz_texture, "VSM Nanite Hi-Z");
		rd->set_resource_name(vsm_hiz_owner, "VSM Nanite Hi-Z Owner");
		rd->texture_clear(vsm_hiz_texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
		rd->buffer_clear(vsm_hiz_owner, 0, p_pool_pages * sizeof(uint32_t));
		vsm_hiz_pool_pages = p_pool_pages;
		vsm_hiz_grid_width = p_grid_width;
		vsm_hiz_pool_reallocations = p_pool_reallocations;
	} else if (vsm_hiz_pool_reallocations != p_pool_reallocations) {
		rd->texture_clear(vsm_hiz_texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
		rd->buffer_clear(vsm_hiz_owner, 0, p_pool_pages * sizeof(uint32_t));
		vsm_hiz_pool_reallocations = p_pool_reallocations;
	}
	return true;
}

void NextGeoCut::invalidate_vsm_hiz() {
	if (vsm_hiz_owner.is_null() || vsm_hiz_pool_pages == 0u) {
		return;
	}
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	rd->buffer_clear(vsm_hiz_owner, 0, vsm_hiz_pool_pages * sizeof(uint32_t));
}

void NextGeoCut::_dispatch_vsm_hiz_build(RID p_pool_texture, RID p_dirty_buffer, RID p_alloc_state_buffer, RID p_indirect_buffer, uint32_t p_table_maps, uint32_t p_pool_pages, uint32_t p_grid_width) {
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (rd == nullptr || uniform_set_cache == nullptr || vsm_hiz_build_pipeline.is_null() || vsm_hiz_texture.is_null() || vsm_hiz_owner.is_null()) {
		invalidate_vsm_hiz();
		return;
	}
	const RID hiz_sh = vsm_sw_shader.version_get_shader(vsm_sw_version, 1);
	LocalVector<RD::Uniform> u;
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_alloc_state_buffer));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_dirty_buffer));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, p_pool_texture));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, vsm_hiz_texture));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, vsm_hiz_owner));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, vsm_counters[0]));
	const RID set = uniform_set_cache->get_cache_vec(hiz_sh, 0, u);
	if (set.is_null()) {
		invalidate_vsm_hiz();
		return;
	}
	const uint32_t push[4] = { p_pool_pages, p_grid_width, p_table_maps, 0u };
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, vsm_hiz_build_pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	rd->compute_list_set_push_constant(list, push, sizeof(push));
	rd->compute_list_dispatch_indirect(list, p_indirect_buffer, RendererVirtualShadowMapStorage::INDIRECT_ALL * sizeof(uint32_t));
	rd->compute_list_end();
}

void NextGeoCut::update_vsm_hiz(RID p_pool_texture, RID p_dirty_buffer, RID p_alloc_state_buffer, RID p_indirect_buffer, uint32_t p_table_maps, uint32_t p_pool_pages, uint32_t p_grid_width, uint64_t p_pool_reallocations) {
	if (!vsm_ready || vsm_hiz_texture.is_null() || vsm_hiz_owner.is_null()) {
		return;
	}
	if (p_pool_texture.is_null() || p_dirty_buffer.is_null() || p_alloc_state_buffer.is_null() || p_indirect_buffer.is_null() ||
			p_table_maps == 0u || p_pool_pages == 0u || p_grid_width == 0u) {
		invalidate_vsm_hiz();
		return;
	}
	if (!_ensure_vsm_hiz(p_pool_pages, p_grid_width, p_pool_reallocations)) {
		invalidate_vsm_hiz();
		return;
	}
	_dispatch_vsm_hiz_build(p_pool_texture, p_dirty_buffer, p_alloc_state_buffer, p_indirect_buffer, p_table_maps, p_pool_pages, p_grid_width);
}

bool NextGeoCut::_ensure_vsm_bins(RenderingDevice *p_rd) {
	if (vsm_bins_ready) {
		return true;
	}
	if (!_ensure_raster_bins(p_rd)) {
		return false;
	}
	if (vsm_bin_state.is_null()) {
		vsm_bin_state = _storage(RendererRD::NEXTGEO_RASTER_BIN_STATE_WORDS * sizeof(uint32_t));
	}
	if (vsm_bin_args.is_null()) {
		vsm_bin_args = p_rd->storage_buffer_create(RendererRD::NEXTGEO_RASTER_BIN_ARGS_WORDS * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	if (vsm_bin_draw_count.is_null()) {
		const uint32_t one = 1u;
		Vector<uint8_t> one_bytes;
		one_bytes.resize(sizeof(uint32_t));
		memcpy(one_bytes.ptrw(), &one, sizeof(uint32_t));
		vsm_bin_draw_count = p_rd->storage_buffer_create(sizeof(uint32_t), one_bytes, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	if (vsm_bin_hw_counts.is_null()) {
		vsm_bin_hw_counts = p_rd->storage_buffer_create(RendererRD::NEXTGEO_RASTER_BIN_HW_COUNTS_WORDS * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	if (vsm_bin_append.is_null()) {
		vsm_bin_append = _storage(RendererRD::NEXTGEO_VSM_CLUSTER_CAPACITY * sizeof(uint32_t));
	}
	if (vsm_bin_lists.is_null()) {
		vsm_bin_lists = _storage(RendererRD::NEXTGEO_VSM_CLUSTER_CAPACITY * sizeof(uint32_t));
	}
	vsm_bins_ready = vsm_bin_state.is_valid() && vsm_bin_args.is_valid() && vsm_bin_draw_count.is_valid() && vsm_bin_hw_counts.is_valid() && vsm_bin_append.is_valid() && vsm_bin_lists.is_valid();
	return vsm_bins_ready;
}

RID NextGeoCut::_vsm_bin_render_pipeline(RID p_shader, RD::FramebufferFormatID p_format) {
	RenderingDevice *rd = RD::get_singleton();
	if (vsm_bin_render_format != p_format) {
		for (const KeyValue<RID, RID> &E : vsm_bin_render_pipelines) {
			if (rd->render_pipeline_is_valid(E.value)) {
				rd->free_rid(E.value);
			}
		}
		vsm_bin_render_pipelines.clear();
		vsm_bin_render_format = p_format;
	}
	RID *existing = vsm_bin_render_pipelines.getptr(p_shader);
	if (existing != nullptr && rd->render_pipeline_is_valid(*existing)) {
		return *existing;
	}
	RD::PipelineRasterizationState raster_state;
	raster_state.cull_mode = RD::POLYGON_CULL_BACK;
	const RID pipeline = rd->render_pipeline_create(p_shader, p_format, RD::INVALID_ID, RD::RENDER_PRIMITIVE_TRIANGLES, raster_state, RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(0));
	if (pipeline.is_valid()) {
		vsm_bin_render_pipelines[p_shader] = pipeline;
	}
	return pipeline;
}

void NextGeoCut::_compact_vsm_bins() {
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	const RID prefix = raster_bin_compact_shader.version_get_shader(raster_bin_compact_version, 0);
	const RID scatter = raster_bin_compact_shader.version_get_shader(raster_bin_compact_version, 1);
	RD::Uniform u_state(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, vsm_bin_state);
	RD::Uniform u_append(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, vsm_bin_append);
	RD::Uniform u_lists(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, vsm_bin_lists);
	RD::Uniform u_args(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, vsm_bin_args);
	RD::Uniform u_hw_counts(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, vsm_bin_hw_counts);
	const RID prefix_set = uniform_set_cache->get_cache(prefix, 0, u_state, u_append, u_lists, u_args, u_hw_counts);
	const RID scatter_set = uniform_set_cache->get_cache(scatter, 0, u_state, u_append, u_lists);
	if (prefix_set.is_null() || scatter_set.is_null()) {
		return;
	}
	const uint32_t push[4] = { 0u, 0u, RendererRD::NEXTGEO_VSM_CLUSTER_CAPACITY, RendererRD::NEXTGEO_RASTER_VERTEX_COUNT };
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, raster_bin_prefix_pipeline);
	rd->compute_list_bind_uniform_set(list, prefix_set, 0);
	rd->compute_list_set_push_constant(list, push, sizeof(push));
	rd->compute_list_dispatch(list, 1u, 1u, 1u);
	rd->compute_list_end();

	list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, raster_bin_scatter_pipeline);
	rd->compute_list_bind_uniform_set(list, scatter_set, 0);
	rd->compute_list_set_push_constant(list, push, sizeof(push));
	rd->compute_list_dispatch_indirect(list, vsm_bin_args, RendererRD::nextgeo_raster_bin_scatter_args_word(0u) * sizeof(uint32_t));
	rd->compute_list_end();
}

// GodotNext: nextgeo_tessellation (#163). Lazy VSM tessellation buffers (0 allocated when VSM tessellation is off).
bool NextGeoCut::_ensure_vsm_tessellation(RenderingDevice *p_rd) {
	if (vsm_tess_ready) {
		return true;
	}
	if (p_rd == nullptr) {
		return false;
	}
	if (vsm_tess_diced_patches.is_null()) {
		vsm_tess_diced_patches = _storage(RendererRD::NEXTGEO_TESS_VSM_PATCH_CAPACITY * sizeof(RendererRD::NextGeoDicedPatchRecord));
	}
	if (vsm_tess_hw_patches.is_null()) {
		vsm_tess_hw_patches = _storage(RendererRD::NEXTGEO_TESS_VSM_PATCH_CAPACITY * sizeof(uint32_t));
	}
	for (uint32_t chain = 0; chain < RendererRD::NEXTGEO_VSM_CHAIN_COUNT; chain++) {
		if (vsm_tess_state[chain].is_null()) {
			vsm_tess_state[chain] = _storage(sizeof(RendererRD::NextGeoTessState));
		}
	}
	if (vsm_tess_hw_args.is_null()) {
		vsm_tess_hw_args = p_rd->storage_buffer_create(RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX * 4u * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	for (int ring = 0; ring < 3; ring++) {
		if (vsm_tess_stats_ring[ring].is_null()) {
			vsm_tess_stats_ring[ring] = _storage(RendererRD::NEXTGEO_VSM_CHAIN_COUNT * RendererRD::NEXTGEO_TESS_STATS_WORDS * sizeof(uint32_t));
		}
		vsm_tess_stats_ring_pending[ring] = false;
	}
	vsm_tess_ready = vsm_tess_diced_patches.is_valid() && vsm_tess_hw_patches.is_valid() &&
			vsm_tess_state[0].is_valid() && vsm_tess_state[1].is_valid() && vsm_tess_hw_args.is_valid() &&
			vsm_tess_stats_ring[0].is_valid() && vsm_tess_stats_ring[1].is_valid() && vsm_tess_stats_ring[2].is_valid();
	if (!vsm_tess_ready) {
		_free_vsm_tessellation_resources();
	}
	return vsm_tess_ready;
}

void NextGeoCut::_free_vsm_tessellation_resources() {
	RenderingDevice *rd = RD::get_singleton();
	for (int ring = 0; ring < 3; ring++) {
		vsm_tess_stats_ring_pending[ring] = false;
	}
	if (rd != nullptr) {
		for (RID *buf : { &vsm_tess_diced_patches, &vsm_tess_hw_patches, &vsm_tess_state[0], &vsm_tess_state[1], &vsm_tess_hw_args, &vsm_tess_stats_ring[0], &vsm_tess_stats_ring[1], &vsm_tess_stats_ring[2] }) {
			if (buf->is_valid()) {
				rd->free_rid(*buf);
				*buf = RID();
			}
		}
	}
	vsm_tess_ready = false;
	vsm_tess_dispatches_last_frame = 0;
	vsm_tess_have_gpu_stats = false;
	memset(vsm_tess_last_gpu_stats, 0, sizeof(vsm_tess_last_gpu_stats));
}

// GodotNext: nextgeo_voxels (#165). Lazy VSM voxel buffers and depth-only SW splat pipeline (0 allocated when off).
bool NextGeoCut::_ensure_vsm_voxels(RenderingDevice *p_rd) {
	if (vsm_voxels_ready) {
		return true;
	}
	if (p_rd == nullptr || !p_rd->has_feature(RD::SUPPORTS_IMAGE_ATOMIC_32_BIT)) {
		return false;
	}
#ifdef REAL_T_IS_DOUBLE
	const String base_defines = "\n#define REAL_T_IS_DOUBLE\n";
#else
	const String base_defines = "";
#endif
	const String vsm_defines = RendererVirtualShadowMapStorage::get_glsl_defines();
	if (vsm_voxel_shader.get_variant_count() == 0) {
		vsm_voxel_shader.initialize(Vector<String>({ base_defines + vsm_defines }));
	}
	if (vsm_voxel_version.is_null()) {
		vsm_voxel_version = vsm_voxel_shader.version_create();
	}
	if (vsm_voxel_version.is_valid() && vsm_voxel_pipeline.is_null()) {
		const RID sh = vsm_voxel_shader.version_get_shader(vsm_voxel_version, 0);
		if (sh.is_valid()) {
			vsm_voxel_pipeline = p_rd->compute_pipeline_create(sh);
		}
	}
	if (vsm_voxel_bricks.is_null()) {
		vsm_voxel_bricks = _storage(RendererRD::NEXTGEO_VSM_VOXEL_BRICK_CAPACITY * 4u * sizeof(uint32_t));
	}
	if (vsm_voxel_args.is_null()) {
		vsm_voxel_args = p_rd->storage_buffer_create(4u * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	vsm_voxels_ready = vsm_voxel_pipeline.is_valid() && vsm_voxel_bricks.is_valid() && vsm_voxel_args.is_valid();
	if (!vsm_voxels_ready) {
		_free_vsm_voxel_resources();
	}
	return vsm_voxels_ready;
}

void NextGeoCut::_free_vsm_voxel_resources() {
	RenderingDevice *rd = RD::get_singleton();
	vsm_voxel_pipeline = RID();
	if (vsm_voxel_version.is_valid()) {
		vsm_voxel_shader.version_free(vsm_voxel_version);
		vsm_voxel_version = RID();
	}
	if (rd != nullptr) {
		for (RID *buf : { &vsm_voxel_bricks, &vsm_voxel_args }) {
			if (buf->is_valid()) {
				rd->free_rid(*buf);
				*buf = RID();
			}
		}
	}
	vsm_voxels_ready = false;
	vsm_voxel_dispatches_last_frame = 0;
}

void NextGeoCut::_readback_vsm_tess_stats_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot) {
	NextGeoCut *self = nullptr;
	{
		MutexLock lock(s_page_req_registry_mutex);
		NextGeoCut **found = s_page_req_registry.getptr(p_token);
		if (found != nullptr) {
			self = *found;
		}
		s_page_req_registry.erase(p_token);
	}
	if (self != nullptr) {
		self->pending_readback_tokens.erase(p_token);
		self->_publish_vsm_tess_stats(p_data, p_ring_slot);
	}
}

void NextGeoCut::_publish_vsm_tess_stats(const PackedByteArray &p_data, uint32_t p_ring_slot) {
	if (p_ring_slot < 3u) {
		vsm_tess_stats_ring_pending[p_ring_slot] = false;
	}
	const uint32_t expected = RendererRD::NEXTGEO_VSM_CHAIN_COUNT * RendererRD::NEXTGEO_TESS_STATS_WORDS * sizeof(uint32_t);
	if (uint32_t(p_data.size()) < expected) {
		return;
	}
	memcpy(vsm_tess_last_gpu_stats, p_data.ptr(), expected);
	for (uint32_t chain = 0; chain < RendererRD::NEXTGEO_VSM_CHAIN_COUNT; chain++) {
		if (vsm_tess_last_gpu_stats[chain][0] > RendererRD::NEXTGEO_TESS_VSM_PATCH_CAPACITY) {
			vsm_tess_last_gpu_stats[chain][0] = RendererRD::NEXTGEO_TESS_VSM_PATCH_CAPACITY;
		}
	}
	vsm_tess_have_gpu_stats = true;
}

void NextGeoCut::_request_vsm_tess_stats(uint32_t p_ring_slot) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_ring_slot >= 3u || vsm_tess_stats_ring_pending[p_ring_slot] || vsm_tess_stats_ring[p_ring_slot].is_null() || vsm_tess_state[0].is_null() || vsm_tess_state[1].is_null()) {
		return;
	}
	const uint32_t one = RendererRD::NEXTGEO_TESS_STATS_WORDS * sizeof(uint32_t);
	rd->buffer_copy(vsm_tess_state[0], vsm_tess_stats_ring[p_ring_slot], 0, 0, one);
	rd->buffer_copy(vsm_tess_state[1], vsm_tess_stats_ring[p_ring_slot], 0, one, one);
	uint64_t token = 0;
	{
		MutexLock lock(s_page_req_registry_mutex);
		token = s_page_req_next_token++;
		s_page_req_registry[token] = this;
	}
	pending_readback_tokens.push_back(token);
	vsm_tess_stats_ring_pending[p_ring_slot] = true;
	if (rd->buffer_get_data_async(vsm_tess_stats_ring[p_ring_slot], callable_mp_static(&NextGeoCut::_readback_vsm_tess_stats_static).bind(token, p_ring_slot), 0, one * RendererRD::NEXTGEO_VSM_CHAIN_COUNT) != OK) {
		vsm_tess_stats_ring_pending[p_ring_slot] = false;
		pending_readback_tokens.erase(token);
		MutexLock lock(s_page_req_registry_mutex);
		s_page_req_registry.erase(token);
	}
}

void NextGeoCut::_draw_vsm_bins(uint32_t p_chain, const NextGeoVSMInputs &p_inputs, bool p_chain_tess) {
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	const RID page_buf = RSG::virtual_mesh_storage->get_page_buffer().is_valid() ? RSG::virtual_mesh_storage->get_page_buffer() : dummy_page_buf;
	const RID cluster_buf = RSG::virtual_mesh_storage->get_cluster_buffer().is_valid() ? RSG::virtual_mesh_storage->get_cluster_buffer() : dummy_page_buf;
	const RID skin_palette_buf = RSG::virtual_mesh_storage->get_skin_palette_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_palette_buffer() : dummy_page_buf;
	const RID skin_inst_buf = RSG::virtual_mesh_storage->get_skin_instance_buffer().is_valid() ? RSG::virtual_mesh_storage->get_skin_instance_buffer() : dummy_page_buf;
	const RID cluster_skin_buf = RSG::virtual_mesh_storage->get_cluster_skin_buffer().is_valid() ? RSG::virtual_mesh_storage->get_cluster_skin_buffer() : dummy_page_buf;
	const RID attr_buf = RSG::virtual_mesh_storage->get_instance_attributes_buffer().is_valid() ? RSG::virtual_mesh_storage->get_instance_attributes_buffer() : dummy_page_buf;
	const RID wind_params_buf = RSG::virtual_mesh_storage->get_wind_params_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_params_buffer() : dummy_page_buf;
	const RID wind_meshes_buf = RSG::virtual_mesh_storage->get_wind_meshes_buffer().is_valid() ? RSG::virtual_mesh_storage->get_wind_meshes_buffer() : dummy_page_buf;
	LocalVector<RD::Uniform> geometry_uniforms;
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, RSG::virtual_mesh_storage->get_record_buffer()));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RSG::virtual_mesh_storage->get_mesh_record_buffer()));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, page_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, cluster_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, vsm_clusters[p_chain]));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_inputs.map_data_buffer));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_inputs.dirty_buffer));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, RSG::virtual_mesh_storage->get_spline_buffer()));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, skin_palette_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, skin_inst_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, cluster_skin_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, attr_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, wind_params_buf));
	geometry_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, wind_meshes_buf));

	LocalVector<RD::Uniform> bin_uniforms;
	bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, vsm_bin_lists));
	bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, vsm_bin_state));
	bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, p_inputs.pool_texture));
	bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, vsm_counters[p_chain]));

	struct BinPush {
		uint32_t list_base;
		uint32_t phase;
		uint32_t bin;
		uint32_t flags;
		int32_t size[2];
		uint32_t pad[2]; // pad[0] = cluster_capacity
	};
	static_assert(sizeof(BinPush) == 32, "BinPush must match raster_material_inc.glsl's RasterBinPush.");

	struct VsmTessPush {
		uint32_t list_base;
		uint32_t phase;
		uint32_t bin;
		uint32_t flags;
		int32_t size[2];
		uint32_t cluster_capacity;
		uint32_t wpo_max_bits;
		float dicing_rate;
		float px_scale;
		float sw_max_edge;
		float z_near;
		float disp_magnitude;
		float disp_center;
		uint32_t hw_bin_base;
		uint32_t hw_bin_capacity;
		uint32_t patch_capacity;
		uint32_t tess_pad[3];
	};
	static_assert(sizeof(VsmTessPush) == 80, "VsmTessPush must match raster_material_inc.glsl's NEXTGEO_TESSELLATION RasterBinPush.");

	auto bind_sets = [&](RID p_shader, const NextGeoVSMBinProgram &p_program, RID r_sets[4]) -> bool {
		r_sets[0] = uniform_set_cache->get_cache_vec(p_shader, 0, geometry_uniforms);
		r_sets[1] = uniform_set_cache->get_cache_vec(p_shader, 1, p_inputs.environment_uniforms);
		r_sets[2] = uniform_set_cache->get_cache_vec(p_shader, 2, bin_uniforms);
		r_sets[3] = p_program.material_uniforms.is_empty() ? RID() : uniform_set_cache->get_cache_vec(p_shader, 3, p_program.material_uniforms);
		return r_sets[0].is_valid() && r_sets[1].is_valid() && r_sets[2].is_valid() && (p_program.material_uniforms.is_empty() || r_sets[3].is_valid());
	};

	auto is_vsm_tess_program = [&](const NextGeoVSMBinProgram &p_program) -> bool {
		return p_chain_tess && vsm_tess_ready &&
				(p_program.features & RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) != 0u &&
				p_program.tess_dice_shader.is_valid() &&
				p_program.tess_hw_shader.is_valid();
	};

	uint32_t active_tess_bins = 0u;
	if (p_chain_tess && vsm_tess_ready) {
		for (const NextGeoVSMBinProgram &program : p_inputs.raster_bin_programs) {
			if (is_vsm_tess_program(program)) {
				active_tess_bins++;
			}
		}
		if (active_tess_bins > 0u) {
			uint32_t hw_args_init[RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX * 4u] = {};
			for (uint32_t b = 0; b < RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX; b++) {
				hw_args_init[b * 4u + 0u] = RendererRD::NEXTGEO_RASTER_VERTEX_COUNT;
			}
			rd->buffer_update(vsm_tess_hw_args, 0, sizeof(hw_args_init), hw_args_init);
		}
	}

	LocalVector<RD::Uniform> tess_dice_bin_uniforms;
	LocalVector<RD::Uniform> tess_hw_bin_uniforms;
	if (active_tess_bins > 0u) {
		tess_dice_bin_uniforms = bin_uniforms;
		tess_dice_bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, vsm_tess_diced_patches));
		tess_dice_bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, vsm_tess_hw_patches));
		tess_dice_bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, vsm_tess_state[p_chain]));
		tess_dice_bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, vsm_tess_hw_args));

		tess_hw_bin_uniforms = bin_uniforms;
		tess_hw_bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, vsm_tess_diced_patches));
		tess_hw_bin_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, vsm_tess_hw_patches));
	}

	auto bind_tess_sets = [&](RID p_shader, const NextGeoVSMBinProgram &p_program, const LocalVector<RD::Uniform> &p_set2, RID r_sets[4]) -> bool {
		r_sets[0] = uniform_set_cache->get_cache_vec(p_shader, 0, geometry_uniforms);
		r_sets[1] = uniform_set_cache->get_cache_vec(p_shader, 1, p_inputs.environment_uniforms);
		r_sets[2] = uniform_set_cache->get_cache_vec(p_shader, 2, p_set2);
		r_sets[3] = p_program.material_uniforms.is_empty() ? RID() : uniform_set_cache->get_cache_vec(p_shader, 3, p_program.material_uniforms);
		return r_sets[0].is_valid() && r_sets[1].is_valid() && r_sets[2].is_valid() && (p_program.material_uniforms.is_empty() || r_sets[3].is_valid());
	};

	const int32_t ps = int32_t(RendererVirtualShadowMapStorage::PAGE_SIZE);
	auto make_vsm_bin_push = [&](const NextGeoVSMBinProgram &p_program) -> BinPush {
		const bool has_wpo = (p_program.features & RendererVirtualMeshStorage::RASTER_FEATURE_VERTEX_OFFSET) != 0u;
		uint32_t flags = 0u;
		uint32_t wpo_bits = 0u;
		if (has_wpo) {
			flags |= (1u << 1u) | (1u << 2u); // NEXTGEO_RASTER_BIN_FLAG_VERTEX_OFFSET | COUNT_CLAMPED
			const float limit = MAX(p_program.vertex_offset_max, 0.0f);
			memcpy(&wpo_bits, &limit, sizeof(uint32_t));
		}
		return BinPush{ 0u, 0u, p_program.bin, flags, { ps, ps }, { RendererRD::NEXTGEO_VSM_CLUSTER_CAPACITY, wpo_bits } };
	};
	const uint32_t vsm_patch_cap = p_inputs.tessellation_debug_max_patches > 0u
			? MIN(p_inputs.tessellation_debug_max_patches, RendererRD::NEXTGEO_TESS_VSM_PATCH_CAPACITY)
			: RendererRD::NEXTGEO_TESS_VSM_PATCH_CAPACITY;
	const float vsm_dicing_rate = RendererRD::nextgeo_tessellation_clamp_dicing_rate(p_inputs.tessellation_dicing_rate);
	const float vsm_sw_max_edge = RendererRD::nextgeo_vsm_sw_switch_texels(p_inputs.sw_max_edge_texels);
	const uint32_t hw_per_bin = active_tess_bins > 0u ? (RendererRD::NEXTGEO_TESS_VSM_PATCH_CAPACITY / active_tess_bins) : 0u;
	auto fill_vsm_tess_push = [&](const NextGeoVSMBinProgram &p_program, uint32_t p_tess_bin_idx) -> VsmTessPush {
		const uint32_t bslot = (p_program.bin - 1u) & 0x3Fu;
		const bool has_wpo = (p_program.features & RendererVirtualMeshStorage::RASTER_FEATURE_VERTEX_OFFSET) != 0u;
		uint32_t flags = bslot << 16u;
		uint32_t wpo_bits = 0u;
		if (has_wpo) {
			flags |= (1u << 1u) | (1u << 2u);
			const float limit = MAX(p_program.vertex_offset_max, 0.0f);
			memcpy(&wpo_bits, &limit, sizeof(uint32_t));
		}
		VsmTessPush push = {};
		push.list_base = 0u;
		push.phase = 0u;
		push.bin = p_program.bin;
		push.flags = flags;
		push.size[0] = ps;
		push.size[1] = ps;
		push.cluster_capacity = RendererRD::NEXTGEO_VSM_CLUSTER_CAPACITY;
		push.wpo_max_bits = wpo_bits;
		push.dicing_rate = vsm_dicing_rate;
		push.px_scale = 0.0f;
		push.sw_max_edge = vsm_sw_max_edge;
		push.z_near = 0.0f;
		push.disp_magnitude = p_program.disp_magnitude;
		push.disp_center = p_program.disp_center;
		push.hw_bin_base = p_tess_bin_idx * hw_per_bin;
		push.hw_bin_capacity = hw_per_bin;
		push.patch_capacity = vsm_patch_cap;
		return push;
	};
	{
		RD::ComputeListID list = rd->compute_list_begin();
		uint32_t tess_bin_idx = 0u;
		for (const NextGeoVSMBinProgram &program : p_inputs.raster_bin_programs) {
			if (is_vsm_tess_program(program)) {
				const RID pipeline = _raster_bin_compute_pipeline(program.tess_dice_shader);
				RID sets[4];
				if (pipeline.is_null() || !bind_tess_sets(program.tess_dice_shader, program, tess_dice_bin_uniforms, sets)) {
					tess_bin_idx++;
					continue;
				}
				const VsmTessPush push = fill_vsm_tess_push(program, tess_bin_idx);
				rd->compute_list_bind_compute_pipeline(list, pipeline);
				for (uint32_t set = 0; set < 4; set++) {
					if (sets[set].is_valid()) {
						rd->compute_list_bind_uniform_set(list, sets[set], set);
					}
				}
				rd->compute_list_set_push_constant(list, &push, sizeof(push));
				rd->compute_list_dispatch_indirect(list, vsm_bin_args, RendererRD::nextgeo_raster_bin_args_word(0u, program.bin, RendererRD::NEXTGEO_RASTER_BIN_KIND_SW) * sizeof(uint32_t));
				vsm_tess_dispatches_last_frame++;
				tess_bin_idx++;
				continue;
			}
			if (program.sw_shader.is_null()) {
				continue;
			}
			const RID pipeline = _raster_bin_compute_pipeline(program.sw_shader);
			RID sets[4];
			if (pipeline.is_null() || !bind_sets(program.sw_shader, program, sets)) {
				continue;
			}
			const BinPush push = make_vsm_bin_push(program);
			rd->compute_list_bind_compute_pipeline(list, pipeline);
			for (uint32_t set = 0; set < 4; set++) {
				if (sets[set].is_valid()) {
					rd->compute_list_bind_uniform_set(list, sets[set], set);
				}
			}
			rd->compute_list_set_push_constant(list, &push, sizeof(push));
			rd->compute_list_dispatch_indirect(list, vsm_bin_args, RendererRD::nextgeo_raster_bin_args_word(0u, program.bin, RendererRD::NEXTGEO_RASTER_BIN_KIND_SW) * sizeof(uint32_t));
		}
		rd->compute_list_end();
	}

	{
		const RD::FramebufferFormatID format = rd->framebuffer_get_format(vsm_empty_fb);
		RD::DrawListID dl = rd->draw_list_begin(vsm_empty_fb, RD::DRAW_DEFAULT_ALL);
		uint32_t tess_bin_idx = 0u;
		for (const NextGeoVSMBinProgram &program : p_inputs.raster_bin_programs) {
			if (is_vsm_tess_program(program)) {
				const RID pipeline = _vsm_bin_render_pipeline(program.tess_hw_shader, format);
				RID sets[4];
				if (pipeline.is_null() || !bind_tess_sets(program.tess_hw_shader, program, tess_hw_bin_uniforms, sets)) {
					tess_bin_idx++;
					continue;
				}
				const uint32_t bslot = (program.bin - 1u) & 0x3Fu;
				const VsmTessPush push = fill_vsm_tess_push(program, tess_bin_idx);
				rd->draw_list_bind_render_pipeline(dl, pipeline);
				for (uint32_t set = 0; set < 4; set++) {
					if (sets[set].is_valid()) {
						rd->draw_list_bind_uniform_set(dl, sets[set], set);
					}
				}
				rd->draw_list_set_push_constant(dl, &push, sizeof(push));
				if (rd->has_feature(RD::SUPPORTS_DRAW_INDIRECT_COUNT) && vsm_bin_draw_count.is_valid()) {
					rd->draw_list_draw_indirect_count(dl, false, vsm_tess_hw_args, bslot * 16u, vsm_bin_draw_count, 0u, 1u, 0u);
				} else {
					rd->draw_list_draw_indirect(dl, false, vsm_tess_hw_args, bslot * 16u, 1u, 0u);
				}
				tess_bin_idx++;
				continue;
			}
			const bool use_mesh = p_inputs.use_mesh_hw_raster && program.hw_mesh_shader.is_valid();
			const RID shader = use_mesh ? program.hw_mesh_shader : program.hw_shader;
			if (shader.is_null()) {
				continue;
			}
			const RID pipeline = _vsm_bin_render_pipeline(shader, format);
			RID sets[4];
			if (pipeline.is_null() || !bind_sets(shader, program, sets)) {
				continue;
			}
			const BinPush push = make_vsm_bin_push(program);
			rd->draw_list_bind_render_pipeline(dl, pipeline);
			for (uint32_t set = 0; set < 4; set++) {
				if (sets[set].is_valid()) {
					rd->draw_list_bind_uniform_set(dl, sets[set], set);
				}
			}
			rd->draw_list_set_push_constant(dl, &push, sizeof(push));
			if (use_mesh) {
				rd->draw_list_draw_mesh_tasks_indirect_count(dl, vsm_bin_args, RendererRD::nextgeo_raster_bin_hw_mesh_args_word(0u, program.bin) * sizeof(uint32_t), vsm_bin_hw_counts, RendererRD::nextgeo_raster_bin_hw_count_word(0u, program.bin) * sizeof(uint32_t), 1u, 0u);
			} else {
				// firstInstance = 0 in vsm_bin_args (written by raster_bin_compact PREFIX).
				// GodotNext: vsm_max_storage_buffer_range (#306). Use indirect-count draw when supported
				// (matching vsm_hw_pipeline) so software drivers (llvmpipe) execute the HW bin draw reliably.
				const uint32_t args_offset = RendererRD::nextgeo_raster_bin_args_word(0u, program.bin, RendererRD::NEXTGEO_RASTER_BIN_KIND_HW) * sizeof(uint32_t);
				if (rd->has_feature(RD::SUPPORTS_DRAW_INDIRECT_COUNT) && vsm_bin_draw_count.is_valid()) {
					rd->draw_list_draw_indirect_count(dl, false, vsm_bin_args, args_offset, vsm_bin_draw_count, 0u, 1u, 0u);
				} else {
					rd->draw_list_draw_indirect(dl, false, vsm_bin_args, args_offset, 1u, 0u);
				}
			}
		}
		rd->draw_list_end();
	}
}

void NextGeoCut::_readback_vsm_stats_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot) {
	NextGeoCut *self = nullptr;
	{
		MutexLock lock(s_page_req_registry_mutex);
		NextGeoCut **found = s_page_req_registry.getptr(p_token);
		if (found != nullptr) {
			self = *found;
		}
		s_page_req_registry.erase(p_token);
	}
	if (self != nullptr) {
		self->pending_readback_tokens.erase(p_token);
		self->_publish_vsm_stats(p_data, p_ring_slot);
	}
}

void NextGeoCut::_publish_vsm_stats(const PackedByteArray &p_data, uint32_t p_ring_slot) {
	if (p_ring_slot < 3u) {
		vsm_counters_ring_pending[p_ring_slot] = false;
	}
	const uint32_t expected = RendererRD::NEXTGEO_VSM_CHAIN_COUNT * sizeof(RendererRD::NextGeoVSMCounters);
	if (uint32_t(p_data.size()) < expected) {
		return;
	}
	memcpy(vsm_last_gpu_counters, p_data.ptr(), expected);
	vsm_have_gpu_counters = true;
}

void NextGeoCut::_request_vsm_stats(uint32_t p_ring_slot) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_ring_slot >= 3u || vsm_counters_ring_pending[p_ring_slot] || vsm_counters_ring[p_ring_slot].is_null()) {
		return;
	}
	const uint32_t one = uint32_t(sizeof(RendererRD::NextGeoVSMCounters));
	rd->buffer_copy(vsm_counters[0], vsm_counters_ring[p_ring_slot], 0, 0, one);
	rd->buffer_copy(vsm_counters[1], vsm_counters_ring[p_ring_slot], 0, one, one);
	uint64_t token = 0;
	{
		MutexLock lock(s_page_req_registry_mutex);
		token = s_page_req_next_token++;
		s_page_req_registry[token] = this;
	}
	pending_readback_tokens.push_back(token);
	vsm_counters_ring_pending[p_ring_slot] = true;
	const Error err = rd->buffer_get_data_async(vsm_counters_ring[p_ring_slot], callable_mp_static(&NextGeoCut::_readback_vsm_stats_static).bind(token, p_ring_slot), 0, one * RendererRD::NEXTGEO_VSM_CHAIN_COUNT);
	if (err != OK) {
		vsm_counters_ring_pending[p_ring_slot] = false;
		pending_readback_tokens.erase(token);
		MutexLock lock(s_page_req_registry_mutex);
		s_page_req_registry.erase(token);
	}
}

void NextGeoCut::mark_vsm_dynamic_test(RID p_page_table, uint32_t p_table_maps, uint32_t p_pool_pages) {
	if (!vsm_ready || vsm_hiz_owner.is_null() || p_page_table.is_null() || p_table_maps == 0u || p_pool_pages == 0u) {
		return;
	}
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (rd == nullptr || uniform_set_cache == nullptr) {
		return;
	}
	const uint32_t pages_to_mark = MIN(p_pool_pages, vsm_hiz_pool_pages);
	if (pages_to_mark == 0u) {
		return;
	}
	const RID sh = vsm_sw_shader.version_get_shader(vsm_sw_version, 2);
	RD::Uniform u_table(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_page_table);
	RD::Uniform u_owner(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, vsm_hiz_owner);
	RD::Uniform u_cnt(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, vsm_counters[0]);
	const RID set = uniform_set_cache->get_cache(sh, 0, u_table, u_owner, u_cnt);
	if (set.is_null()) {
		return;
	}
	const uint32_t push[4] = { pages_to_mark, p_table_maps, 0u, 0u };
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, vsm_mark_dynamic_pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	rd->compute_list_set_push_constant(list, push, sizeof(push));
	rd->compute_list_dispatch_threads(list, pages_to_mark, 1u, 1u);
	rd->compute_list_end();
}

bool NextGeoCut::dispatch_vsm(RendererVirtualMeshStorage *p_storage, const NextGeoVSMInputs &p_inputs, Dictionary *r_stats) {
	if (p_storage == nullptr || p_inputs.pool_texture.is_null() || p_inputs.page_table_buffer.is_null() ||
			p_inputs.map_data_buffer.is_null() || p_inputs.dirty_buffer.is_null() ||
			p_inputs.alloc_state_buffer.is_null() || p_inputs.indirect_buffer.is_null() ||
			p_inputs.table_maps == 0u || p_inputs.pool_pages == 0u || p_inputs.grid_width == 0u) {
		return false;
	}
	if (!_ensure_vsm() || !_ensure_vsm_hiz(p_inputs.pool_pages, p_inputs.grid_width, p_inputs.pool_reallocations)) {
		return false;
	}
	const uint32_t record_count = p_storage->get_active_record_count();
	const uint32_t max_levels = MIN(p_storage->get_max_bvh_levels(), 8u);
	if (record_count == 0u || max_levels == 0u || p_storage->get_record_buffer().is_null() || p_storage->get_mesh_record_buffer().is_null() || p_storage->get_dag_buffer().is_null()) {
		return false;
	}

	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (rd == nullptr || uniform_set_cache == nullptr) {
		return false;
	}

	const uint32_t meta_count = MAX(p_inputs.table_maps, 1u);
	if (vsm_map_meta_buffer.is_null() || vsm_map_meta_capacity < meta_count) {
		if (vsm_map_meta_buffer.is_valid()) {
			rd->free_rid(vsm_map_meta_buffer);
		}
		vsm_map_meta_capacity = MAX(Math::next_power_of_2(meta_count), 128u);
		vsm_map_meta_buffer = _storage(vsm_map_meta_capacity * sizeof(RendererRD::NextGeoVSMMapMeta));
		if (vsm_map_meta_buffer.is_null()) {
			vsm_map_meta_capacity = 0;
			return false;
		}
	}
	if (p_inputs.map_meta.size() >= p_inputs.table_maps) {
		rd->buffer_update(vsm_map_meta_buffer, 0, p_inputs.table_maps * sizeof(RendererRD::NextGeoVSMMapMeta), p_inputs.map_meta.ptr());
	} else {
		LocalVector<RendererRD::NextGeoVSMMapMeta> padded;
		padded.resize(p_inputs.table_maps);
		for (uint32_t i = 0; i < p_inputs.map_meta.size() && i < p_inputs.table_maps; i++) {
			padded[i] = p_inputs.map_meta[i];
		}
		rd->buffer_update(vsm_map_meta_buffer, 0, p_inputs.table_maps * sizeof(RendererRD::NextGeoVSMMapMeta), padded.ptr());
	}

	const bool use_bins = !p_inputs.raster_bin_programs.is_empty() && p_inputs.raster_bin_table != nullptr && p_inputs.raster_bin_table_words > 0u && !p_inputs.environment_uniforms.is_empty() && _ensure_vsm_bins(rd);
	if (use_bins) {
		if (vsm_bin_table_buffer.is_null() || vsm_bin_table_capacity < p_inputs.raster_bin_table_words) {
			if (vsm_bin_table_buffer.is_valid()) {
				rd->free_rid(vsm_bin_table_buffer);
			}
			vsm_bin_table_capacity = MAX(Math::next_power_of_2(p_inputs.raster_bin_table_words), 256u);
			vsm_bin_table_buffer = _storage(vsm_bin_table_capacity * sizeof(uint32_t));
		}
		if (vsm_bin_table_buffer.is_valid()) {
			rd->buffer_update(vsm_bin_table_buffer, 0, p_inputs.raster_bin_table_words * sizeof(uint32_t), p_inputs.raster_bin_table);
		}
	}

	const RID records_buf = p_storage->get_record_buffer();
	const RID meshes_buf = p_storage->get_mesh_record_buffer();
	const RID dag_buf = p_storage->get_dag_buffer();
	const RID residency_buf = p_storage->get_residency_buffer().is_valid() ? p_storage->get_residency_buffer() : dummy_page_buf;
	const RID page_buf = p_storage->get_page_buffer().is_valid() ? p_storage->get_page_buffer() : dummy_page_buf;
	const RID cluster_buf = p_storage->get_cluster_buffer().is_valid() ? p_storage->get_cluster_buffer() : dummy_page_buf;
	const RID spline_buf = p_storage->get_spline_buffer();
	const RID skin_palette_buf = p_storage->get_skin_palette_buffer().is_valid() ? p_storage->get_skin_palette_buffer() : dummy_page_buf;
	const RID skin_inst_buf = p_storage->get_skin_instance_buffer().is_valid() ? p_storage->get_skin_instance_buffer() : dummy_page_buf;
	const RID cluster_skin_buf = p_storage->get_cluster_skin_buffer().is_valid() ? p_storage->get_cluster_skin_buffer() : dummy_page_buf;
	const RID wind_params_buf = p_storage->get_wind_params_buffer().is_valid() ? p_storage->get_wind_params_buffer() : dummy_page_buf;
	const RID wind_meshes_buf = p_storage->get_wind_meshes_buffer().is_valid() ? p_storage->get_wind_meshes_buffer() : dummy_page_buf;
	// GodotNext: vsm_max_storage_buffer_range (#306). Refuse to bind a page arena larger than the device limit.
	const uint64_t max_storage_range = rd->limit_get(RD::LIMIT_MAX_STORAGE_BUFFER_RANGE);
	ERR_FAIL_COND_V_MSG(max_storage_range > 0 && p_storage->get_page_buffer().is_valid() && p_storage->get_page_arena_allocation_bytes() > max_storage_range, false,
			vformat("VSM NextGeo: page arena (%d B) exceeds device maxStorageBufferRange (%d B).", p_storage->get_page_arena_allocation_bytes(), max_storage_range));
	const RID bin_table_buf = (use_bins && vsm_bin_table_buffer.is_valid()) ? vsm_bin_table_buffer : dummy_page_buf;
	const RID bin_append_buf = (use_bins && vsm_bin_append.is_valid()) ? vsm_bin_append : dummy_page_buf;
	const RID bin_state_buf = (use_bins && vsm_bin_state.is_valid()) ? vsm_bin_state : dummy_page_buf;
	const bool use_indirect_count = rd->has_feature(RD::SUPPORTS_DRAW_INDIRECT_COUNT);
	const float sw_threshold = RendererRD::nextgeo_vsm_sw_switch_texels(p_inputs.sw_max_edge_texels);

	uint32_t cull_flags = 0u;
	if (p_inputs.hiz_enabled) {
		cull_flags |= RendererRD::NEXTGEO_VSM_CULL_FLAG_HIZ;
	}
	if (use_bins) {
		cull_flags |= RendererRD::NEXTGEO_VSM_CULL_FLAG_PROGRAMMABLE_BINS;
	}
	if (p_inputs.dynamic_layer_hook) {
		cull_flags |= RendererRD::NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK;
	}
	if (p_inputs.count_sw_writes) {
		cull_flags |= RendererRD::NEXTGEO_VSM_CULL_FLAG_COUNT_SW_WRITES;
	}

	const uint32_t seed_init[4] = { 0u, 0u, 1u, 0u };
	uint32_t bvh_init[8 * 4] = {};
	for (uint32_t lvl = 0; lvl < 8; lvl++) {
		bvh_init[lvl * 4 + 0] = 0u;
		bvh_init[lvl * 4 + 1] = 1u;
		bvh_init[lvl * 4 + 2] = 1u;
		bvh_init[lvl * 4 + 3] = 0u;
	}
	const uint32_t group_init[4] = { 0u, 1u, 1u, 0u };
	// SW dispatch: (0, 0, 1, 0); HW VS draw: (384, 0, 0, 0) with firstInstance = 0; HW mesh tasks: (0, 1, 1, 0).
	const uint32_t raster_args_init[12] = { 0u, 0u, 1u, 0u, RendererRD::NEXTGEO_RASTER_VERTEX_COUNT, 0u, 0u, 0u, 0u, 1u, 1u, 0u };
	const uint32_t raster_counts_init[4] = { 0u, 0u, 0u, 0u };
	const bool use_mesh_hw = p_inputs.use_mesh_hw_raster && vsm_hw_mesh_ready;

	const RID setup_sh = vsm_cluster_shader.version_get_shader(vsm_cluster_version, 0);
	const RID seed_sh = vsm_cluster_shader.version_get_shader(vsm_cluster_version, 1);
	const RID bvh_sh = vsm_cluster_shader.version_get_shader(vsm_cluster_version, 2);
	const RID leaf_sh = vsm_cluster_shader.version_get_shader(vsm_cluster_version, 3);
	const RID hw_sh = use_mesh_hw ? vsm_hw_mesh_shader.version_get_shader(vsm_hw_mesh_version, 0) : vsm_hw_shader.version_get_shader(vsm_hw_version, 0);
	const RID sw_sh = vsm_sw_shader.version_get_shader(vsm_sw_version, 0);

	auto push_common_vsm_uniforms = [&](LocalVector<RD::Uniform> &r_u, uint32_t p_chain, RID p_nodes_in) {
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, records_buf));
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, meshes_buf));
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, dag_buf));
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, vsm_params_buffer));
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, residency_buf));
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, vsm_counters[p_chain]));
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_inputs.map_data_buffer));
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, vsm_map_meta_buffer));
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, p_inputs.dirty_buffer));
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, p_inputs.alloc_state_buffer));
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, p_inputs.page_table_buffer));
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, vsm_hiz_texture));
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, vsm_hiz_owner));
		r_u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, p_nodes_in));
	};

	// GodotNext: nextgeo_tessellation (#163). Allocate VSM tessellation buffers only when VSM tessellation is active.
	const bool vsm_tess_requested = use_bins && (p_inputs.tessellation_directional || p_inputs.tessellation_local);
	bool vsm_has_tess_bin = false;
	if (vsm_tess_requested) {
		for (const NextGeoVSMBinProgram &prog : p_inputs.raster_bin_programs) {
			if ((prog.features & RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) != 0u &&
					prog.tess_dice_shader.is_valid() && prog.tess_hw_shader.is_valid()) {
				vsm_has_tess_bin = true;
				break;
			}
		}
	}
	if (!vsm_tess_requested && vsm_tess_ready) {
		_free_vsm_tessellation_resources();
	}
	const bool vsm_tess_active = vsm_has_tess_bin && _ensure_vsm_tessellation(rd);
	vsm_tess_dispatches_last_frame = 0;
	// GodotNext: nextgeo_voxels (#165). Allocate VSM voxel buffers only when experimental/voxels is enabled.
	const bool vsm_voxels_requested = p_inputs.voxels_enabled;
	if (!vsm_voxels_requested && vsm_voxels_ready) {
		_free_vsm_voxel_resources();
	}
	const bool vsm_voxels_active = vsm_voxels_requested && _ensure_vsm_voxels(rd);
	vsm_voxel_dispatches_last_frame = 0;

	rd->draw_command_begin_label("VSM NextGeo Cull and Raster");

	// GodotNext: vsm_local_lights (#216). Count indirect raster dispatches issued for local maps (chain 1).
	uint32_t local_raster_indirect_dispatches = 0u;
	// Chain 0: all directional maps; Chain 1: all local maps (1 indirect dispatch chain each).
	for (uint32_t chain = 0; chain < RendererRD::NEXTGEO_VSM_CHAIN_COUNT; chain++) {
		const bool chain_tess = vsm_tess_active && (chain == 0u ? p_inputs.tessellation_directional : p_inputs.tessellation_local);
		RendererRD::NextGeoVSMParams vp = {};
		vp.table_maps = p_inputs.table_maps;
		vp.pool_pages = p_inputs.pool_pages;
		vp.grid_width = p_inputs.grid_width;
		vp.record_count = record_count;
		vp.node_capacity = RendererRD::NEXTGEO_VSM_NODE_CAPACITY;
		vp.group_capacity = RendererRD::NEXTGEO_VSM_GROUP_CAPACITY;
		vp.cluster_capacity = RendererRD::NEXTGEO_VSM_CLUSTER_CAPACITY;
		vp.cull_flags = cull_flags | (chain_tess ? RendererRD::NEXTGEO_VSM_CULL_FLAG_TESSELLATION : 0u) | (vsm_voxels_active ? RendererRD::NEXTGEO_VSM_CULL_FLAG_VOXELS : 0u);
		vp.chain_index = chain;
		vp.bucket_table_words = use_bins ? p_inputs.raster_bin_table_words : 0u;
		vp.max_texel_error = p_inputs.max_texel_error;
		vp.lod_bias = p_inputs.lod_bias;
		vp.sw_max_edge_texels = sw_threshold;
		vp.dicing_rate = RendererRD::nextgeo_tessellation_clamp_dicing_rate(p_inputs.tessellation_dicing_rate);
		vp.camera_position[0] = float(p_inputs.camera_position.x);
		vp.camera_position[1] = float(p_inputs.camera_position.y);
		vp.camera_position[2] = float(p_inputs.camera_position.z);
		vp.camera_position[3] = 1.0f;
		if (use_bins) {
			for (const NextGeoVSMBinProgram &prog : p_inputs.raster_bin_programs) {
				if (prog.bin < RendererVirtualMeshStorage::RASTER_BIN_LIMIT) {
					vp.bin_features[prog.bin] = prog.features;
				}
			}
		}
		if (vsm_voxels_active) {
			const float vthresh = RendererRD::NEXTGEO_VOXEL_PROVISIONAL_SCREEN_THRESHOLD_PX;
			memcpy(&vp.bin_features[65], &vthresh, sizeof(uint32_t));
			vp.bin_features[66] = RendererRD::NEXTGEO_VSM_VOXEL_BRICK_CAPACITY;
		}
		rd->buffer_update(vsm_params_buffer, 0, sizeof(vp), &vp);
		rd->buffer_clear(vsm_counters[chain], 0, sizeof(RendererRD::NextGeoVSMCounters));
		if (vsm_tess_ready && vsm_tess_state[chain].is_valid()) {
			rd->buffer_clear(vsm_tess_state[chain], 0, sizeof(RendererRD::NextGeoTessState));
		}
		rd->buffer_update(vsm_seed_args, 0, sizeof(seed_init), seed_init);
		rd->buffer_update(vsm_bvh_args[0], 0, sizeof(bvh_init), bvh_init);
		rd->buffer_update(vsm_bvh_args[1], 0, sizeof(bvh_init), bvh_init);
		rd->buffer_update(vsm_group_args, 0, sizeof(group_init), group_init);
		rd->buffer_update(vsm_raster_args, 0, sizeof(raster_args_init), raster_args_init);
		rd->buffer_update(vsm_raster_counts, 0, sizeof(raster_counts_init), raster_counts_init);
		if (vsm_voxels_active && vsm_voxel_args.is_valid()) {
			const uint32_t vsm_voxel_args_init[4] = { 0u, 0u, 1u, 0u };
			rd->buffer_update(vsm_voxel_args, 0, sizeof(vsm_voxel_args_init), vsm_voxel_args_init);
		}
		if (use_bins) {
			rd->buffer_clear(vsm_bin_state, 0, RendererRD::NEXTGEO_RASTER_BIN_STATE_WORDS * sizeof(uint32_t));
			rd->buffer_clear(vsm_bin_args, 0, RendererRD::NEXTGEO_RASTER_BIN_ARGS_WORDS * sizeof(uint32_t));
			rd->buffer_clear(vsm_bin_hw_counts, 0, RendererRD::NEXTGEO_RASTER_BIN_HW_COUNTS_WORDS * sizeof(uint32_t));
		}

		// 1. VSM_SETUP: compute indirect dispatch grid for VSM_SEED from vsm_alloc_state.
		{
			LocalVector<RD::Uniform> u;
			push_common_vsm_uniforms(u, chain, vsm_nodes_b);
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, vsm_nodes_a));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, vsm_groups));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, vsm_seed_args));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, vsm_group_args));
			const RID set = uniform_set_cache->get_cache_vec(setup_sh, 0, u);
			if (set.is_null()) {
				continue;
			}
			const uint32_t push[4] = { 0u, 0u, 0u, 0u };
			RD::ComputeListID list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, vsm_setup_pipeline);
			rd->compute_list_bind_uniform_set(list, set, 0);
			rd->compute_list_set_push_constant(list, push, sizeof(push));
			rd->compute_list_dispatch(list, 1u, 1u, 1u);
			rd->compute_list_end();
		}

		// 2. VSM_SEED: cull (instance, dirty_page) pairs -> vsm_nodes_a + vsm_bvh_args[0].
		{
			LocalVector<RD::Uniform> u;
			push_common_vsm_uniforms(u, chain, vsm_nodes_b);
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, vsm_nodes_a));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, vsm_groups));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, vsm_bvh_args[0]));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, vsm_group_args));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, wind_params_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, wind_meshes_buf));
			const RID set = uniform_set_cache->get_cache_vec(seed_sh, 0, u);
			if (set.is_null()) {
				continue;
			}
			const uint32_t push[4] = { 0u, 0u, 0u, 0u };
			RD::ComputeListID list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, vsm_seed_pipeline);
			rd->compute_list_bind_uniform_set(list, set, 0);
			rd->compute_list_set_push_constant(list, push, sizeof(push));
			rd->compute_list_dispatch_indirect(list, vsm_seed_args, 0u);
			rd->compute_list_end();
		}

		// 3. VSM_BVH: level-synchronous indirect descent ping-ponging nodes & indirect args.
		for (uint32_t level = 0; level < max_levels; level++) {
			const RID nodes_in = (level & 1u) == 0u ? vsm_nodes_a : vsm_nodes_b;
			const RID nodes_out = (level & 1u) == 0u ? vsm_nodes_b : vsm_nodes_a;
			const RID args_in = vsm_bvh_args[level & 1u];
			const RID args_out = vsm_bvh_args[(level + 1u) & 1u];
			LocalVector<RD::Uniform> u;
			push_common_vsm_uniforms(u, chain, nodes_in);
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, nodes_out));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, vsm_groups));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, args_out));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, vsm_group_args));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, skin_inst_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 26, spline_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, wind_params_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, wind_meshes_buf));
			const RID set = uniform_set_cache->get_cache_vec(bvh_sh, 0, u);
			if (set.is_null()) {
				break;
			}
			const uint32_t push[4] = { level, 0u, 0u, 0u };
			RD::ComputeListID list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, vsm_bvh_pipeline);
			rd->compute_list_bind_uniform_set(list, set, 0);
			rd->compute_list_set_push_constant(list, push, sizeof(push));
			rd->compute_list_dispatch_indirect(list, args_in, level * 4u * sizeof(uint32_t));
			rd->compute_list_end();
		}

		// 4. VSM_CLUSTER_CULL: leaf groups -> (cluster, page) pairs routed to HW/SW + programmable bins.
		{
			LocalVector<RD::Uniform> u;
			push_common_vsm_uniforms(u, chain, vsm_groups);
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, page_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, cluster_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, vsm_clusters[chain]));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, vsm_raster_args));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, vsm_raster_counts));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, vsm_raster_lists));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, bin_table_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, bin_append_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, bin_state_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, skin_palette_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, skin_inst_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, cluster_skin_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 26, spline_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, wind_params_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, wind_meshes_buf));
			// GodotNext: nextgeo_voxels (#165). Bindings 29..30 (skipping 27..28 reserved for #152).
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, (vsm_voxels_active && vsm_voxel_bricks.is_valid()) ? vsm_voxel_bricks : dummy_page_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 30, (vsm_voxels_active && vsm_voxel_args.is_valid()) ? vsm_voxel_args : dummy_page_buf));
			const RID set = uniform_set_cache->get_cache_vec(leaf_sh, 0, u);
			if (set.is_null()) {
				continue;
			}
			const uint32_t push[4] = { 0u, 0u, 0u, 0u };
			RD::ComputeListID list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, vsm_leaf_pipeline);
			rd->compute_list_bind_uniform_set(list, set, 0);
			rd->compute_list_set_push_constant(list, push, sizeof(push));
			rd->compute_list_dispatch_indirect(list, vsm_group_args, 0u);
			rd->compute_list_end();
		}

		// 5. Fixed-function SW raster (compute indirect, 1 workgroup per cluster-page pair).
		{
			LocalVector<RD::Uniform> u;
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, records_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, meshes_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, page_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, cluster_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, vsm_clusters[chain]));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, vsm_raster_lists));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_inputs.map_data_buffer));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_inputs.dirty_buffer));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, p_inputs.pool_texture));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, vsm_counters[chain]));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, spline_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, skin_palette_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, skin_inst_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, cluster_skin_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, wind_params_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, wind_meshes_buf));
			const RID set = uniform_set_cache->get_cache_vec(sw_sh, 0, u);
			if (set.is_valid()) {
				const uint32_t push[4] = { RendererRD::NEXTGEO_VSM_CLUSTER_CAPACITY, p_inputs.count_sw_writes ? 1u : 0u, 0u, 0u };
				RD::ComputeListID list = rd->compute_list_begin();
				rd->compute_list_bind_compute_pipeline(list, vsm_sw_pipeline);
				rd->compute_list_bind_uniform_set(list, set, 0);
				rd->compute_list_set_push_constant(list, push, sizeof(push));
				rd->compute_list_dispatch_indirect(list, vsm_raster_args, 0u);
				rd->compute_list_end();
				if (chain == 1u) {
					local_raster_indirect_dispatches++;
				}
			}
		}

		// 6. Fixed-function HW raster (empty 64x64 FB, indirect-count draw #177 / mesh tasks #154).
		{
			if (use_mesh_hw) {
				// Copy hw_clusters (word 13) to a dedicated readonly buffer so vsm_counters stays
				// fragment-only writable across Vulkan and D3D12.
				rd->buffer_copy(vsm_counters[chain], vsm_hw_count_buf, 13u * sizeof(uint32_t), 0u, sizeof(uint32_t));
			}
			const RID pipeline = (use_mesh_hw ? vsm_hw_mesh_pipeline : vsm_hw_pipeline).get_render_pipeline(RD::INVALID_ID, rd->framebuffer_get_format(vsm_empty_fb));
			LocalVector<RD::Uniform> u;
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, records_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, meshes_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, page_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, cluster_buf));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, vsm_clusters[chain]));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, vsm_raster_lists));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_inputs.map_data_buffer));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_inputs.dirty_buffer));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, p_inputs.pool_texture));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, vsm_counters[chain]));
			if (use_mesh_hw) {
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, vsm_hw_count_buf));
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, spline_buf));
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, skin_palette_buf));
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, skin_inst_buf));
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, cluster_skin_buf));
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, wind_params_buf));
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, wind_meshes_buf));
			} else {
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, spline_buf));
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, skin_palette_buf));
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, skin_inst_buf));
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, cluster_skin_buf));
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, wind_params_buf));
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, wind_meshes_buf));
			}
			const RID set = uniform_set_cache->get_cache_vec(hw_sh, 0, u);
			if (pipeline.is_valid() && set.is_valid()) {
				const uint32_t push[4] = { RendererRD::NEXTGEO_VSM_CLUSTER_CAPACITY, 0u, 0u, 0u };
				RD::DrawListID dl = rd->draw_list_begin(vsm_empty_fb, RD::DRAW_DEFAULT_ALL);
				rd->draw_list_bind_render_pipeline(dl, pipeline);
				rd->draw_list_bind_uniform_set(dl, set, 0);
				rd->draw_list_set_push_constant(dl, push, sizeof(push));
				if (use_mesh_hw) {
					rd->draw_list_draw_mesh_tasks_indirect_count(dl, vsm_raster_args, 8u * sizeof(uint32_t), vsm_raster_counts, 0u, 1u, 0u);
				} else if (use_indirect_count) {
					rd->draw_list_draw_indirect_count(dl, false, vsm_raster_args, 4u * sizeof(uint32_t), vsm_raster_counts, 0u, 1u, 0u);
				} else {
					rd->draw_list_draw_indirect(dl, false, vsm_raster_args, 4u * sizeof(uint32_t), 1u, 0u);
				}
				rd->draw_list_end();
				if (chain == 1u) {
					local_raster_indirect_dispatches++;
				}
			}
		}

		// 7. Programmable raster bins (Step 7, shared table with #155; tessellation #163 per chain).
		if (use_bins) {
			_compact_vsm_bins();
			_draw_vsm_bins(chain, p_inputs, chain_tess);
			if (chain == 1u) {
				local_raster_indirect_dispatches += 2u * p_inputs.raster_bin_programs.size();
			}
		}

		// 8. GodotNext: nextgeo_voxels (#165). Depth-only SW voxel brick splat into dirty VSM pages.
		if (vsm_voxels_active && vsm_voxel_pipeline.is_valid() && vsm_voxel_bricks.is_valid() && vsm_voxel_args.is_valid()) {
			const RID vox_sh = vsm_voxel_shader.version_get_shader(vsm_voxel_version, 0);
			LocalVector<RD::Uniform> vu;
			vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, records_buf));
			vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, page_buf));
			vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, vsm_voxel_bricks));
			vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, p_inputs.map_data_buffer));
			vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, p_inputs.dirty_buffer));
			vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, p_inputs.pool_texture));
			vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, vsm_counters[chain]));
			const RID vset = uniform_set_cache->get_cache_vec(vox_sh, 0, vu);
			if (vset.is_valid()) {
				const uint32_t vpush[4] = { RendererRD::NEXTGEO_VSM_VOXEL_BRICK_CAPACITY, p_inputs.count_sw_writes ? 1u : 0u, 0u, 0u };
				RD::ComputeListID vlist = rd->compute_list_begin();
				rd->compute_list_bind_compute_pipeline(vlist, vsm_voxel_pipeline);
				rd->compute_list_bind_uniform_set(vlist, vset, 0);
				rd->compute_list_set_push_constant(vlist, vpush, sizeof(vpush));
				rd->compute_list_dispatch_indirect(vlist, vsm_voxel_args, 0u);
				rd->compute_list_end();
				vsm_voxel_dispatches_last_frame++;
				if (chain == 1u) {
					local_raster_indirect_dispatches++;
				}
			}
		}
	}

	// Build 1-level 8x8 max-depth Hi-Z per cached dirty page after both non-Nanite and Nanite raster.
	_dispatch_vsm_hiz_build(p_inputs.pool_texture, p_inputs.dirty_buffer, p_inputs.alloc_state_buffer, p_inputs.indirect_buffer, p_inputs.table_maps, p_inputs.pool_pages, p_inputs.grid_width);

	rd->draw_command_end_label();

	vsm_dispatched_views++;
	_request_vsm_stats(vsm_counters_ring_cursor++ % 3u);
	if (vsm_tess_active) {
		_request_vsm_tess_stats(vsm_tess_stats_ring_cursor++ % 3u);
	}

	if (r_stats != nullptr) {
		Dictionary stats;
		stats["active"] = true;
		stats["views"] = int64_t(vsm_dispatched_views);
		stats["directional_chains"] = int64_t(1);
		stats["local_chains"] = int64_t(1);
		// GodotNext: vsm_local_lights (#216). Indirect raster dispatches issued for all local dirty pages combined.
		stats["local_raster_indirect_dispatches"] = int64_t(local_raster_indirect_dispatches);
		stats["used_indirect_count"] = use_indirect_count;
		stats["hw_raster_path"] = RendererRD::nextgeo_hw_raster_path_name(use_mesh_hw);
		const RendererRD::NextGeoHWRasterReason vsm_hw_reason = use_mesh_hw
				? RendererRD::NEXTGEO_HW_RASTER_REASON_NONE
				: (p_inputs.use_mesh_hw_raster ? RendererRD::NEXTGEO_HW_RASTER_REASON_SHADER_UNAVAILABLE : p_inputs.hw_raster_reason);
		stats["hw_raster_reason"] = RendererRD::nextgeo_hw_raster_reason_name(vsm_hw_reason);
		stats["sw_threshold_texels"] = double(sw_threshold);
		stats["hiz_enabled"] = p_inputs.hiz_enabled;
		stats["dynamic_layer_hook"] = p_inputs.dynamic_layer_hook;
		stats["programmable_bins"] = int64_t(use_bins ? p_inputs.raster_bin_programs.size() : 0u);
		// GodotNext: nextgeo_tessellation (#163).
		stats["tessellation_directional"] = p_inputs.tessellation_directional;
		stats["tessellation_local"] = p_inputs.tessellation_local;
		stats["tessellation_buffers_allocated"] = vsm_tess_ready;
		stats["tessellation_dicing_dispatches"] = int64_t(vsm_tess_dispatches_last_frame);
		stats["tessellation_directional_diced_clusters"] = int64_t(vsm_tess_have_gpu_stats ? vsm_tess_last_gpu_stats[0][6] : 0u);
		stats["tessellation_local_diced_clusters"] = int64_t(vsm_tess_have_gpu_stats ? vsm_tess_last_gpu_stats[1][6] : 0u);
		stats["tessellation_directional_sw_micro_tris"] = int64_t(vsm_tess_have_gpu_stats ? vsm_tess_last_gpu_stats[0][2] : 0u);
		stats["tessellation_local_sw_micro_tris"] = int64_t(vsm_tess_have_gpu_stats ? vsm_tess_last_gpu_stats[1][2] : 0u);
		stats["tessellation_directional_hw_patches"] = int64_t(vsm_tess_have_gpu_stats ? vsm_tess_last_gpu_stats[0][3] : 0u);
		stats["tessellation_local_hw_patches"] = int64_t(vsm_tess_have_gpu_stats ? vsm_tess_last_gpu_stats[1][3] : 0u);
		stats["tessellation_dicing_overflow"] = int64_t(vsm_tess_have_gpu_stats ? (vsm_tess_last_gpu_stats[0][1] + vsm_tess_last_gpu_stats[1][1]) : 0u);
		// GodotNext: nextgeo_voxels (#165).
		stats["voxels_enabled"] = p_inputs.voxels_enabled;
		stats["voxels_buffers_allocated"] = vsm_voxels_ready;
		stats["vsm_voxel_dispatches"] = int64_t(vsm_voxel_dispatches_last_frame);
		stats["gpu_counters_valid"] = vsm_have_gpu_counters;
		if (vsm_have_gpu_counters) {
			const RendererRD::NextGeoVSMCounters &dc = vsm_last_gpu_counters[0];
			const RendererRD::NextGeoVSMCounters &lc = vsm_last_gpu_counters[1];
			stats["directional_dirty_pages"] = int64_t(dc.dirty_pages);
			stats["local_dirty_pages"] = int64_t(lc.dirty_pages);
			stats["directional_clusters"] = int64_t(dc.cluster_count);
			stats["local_clusters"] = int64_t(lc.cluster_count);
			stats["sw_clusters"] = int64_t(dc.sw_clusters + lc.sw_clusters);
			stats["hw_clusters"] = int64_t(dc.hw_clusters + lc.hw_clusters);
			stats["prog_sw_clusters"] = int64_t(dc.prog_sw_clusters + lc.prog_sw_clusters);
			stats["prog_hw_clusters"] = int64_t(dc.prog_hw_clusters + lc.prog_hw_clusters);
			stats["hiz_culled_nodes"] = int64_t(dc.hiz_culled_nodes + lc.hiz_culled_nodes);
			stats["hiz_culled_clusters"] = int64_t(dc.hiz_culled_clusters + lc.hiz_culled_clusters);
			stats["hiz_pages_built"] = int64_t(dc.hiz_pages_built);
			stats["sw_writes"] = int64_t(dc.sw_writes + lc.sw_writes);
			stats["overflow"] = int64_t(dc.overflow + lc.overflow);
			stats["redraw_marks"] = int64_t(dc.redraw_marks + lc.redraw_marks);
			stats["sink"] = int64_t(dc.sink | lc.sink);
			// GodotNext: vsm_caching (#212).
			stats["static_clusters"] = int64_t(dc.static_clusters + lc.static_clusters);
			stats["dynamic_clusters"] = int64_t(dc.dynamic_clusters + lc.dynamic_clusters);
			stats["always_layer0_clusters"] = int64_t(dc.always_layer0_clusters + lc.always_layer0_clusters);
			// GodotNext: nextgeo_masked_two_sided (#153), nextgeo_vertex_offset (#164).
			stats["distance_fixed_clusters"] = int64_t(dc.distance_fixed_clusters + lc.distance_fixed_clusters);
			stats["vertex_offset_distance_fixed_clusters"] = int64_t(dc.vertex_offset_distance_fixed + lc.vertex_offset_distance_fixed);
			stats["vertex_offset_clamped"] = int64_t(dc.vertex_offset_clamped + lc.vertex_offset_clamped);
			// GodotNext: nextgeo_voxels (#165).
			stats["vsm_voxel_bricks"] = int64_t(dc.voxel_bricks + lc.voxel_bricks);
			stats["vsm_directional_voxel_bricks"] = int64_t(dc.voxel_bricks);
			stats["vsm_local_voxel_bricks"] = int64_t(lc.voxel_bricks);
			stats["vsm_voxel_writes"] = int64_t(dc.voxel_writes + lc.voxel_writes);
		}
		*r_stats = stats;
	}
	return true;
}

void NextGeoCut::_free_vsm_resources() {
	_free_vsm_tessellation_resources();
	_free_vsm_voxel_resources();
	RenderingDevice *rd = RD::get_singleton();
	for (int ring = 0; ring < 3; ring++) {
		vsm_counters_ring_pending[ring] = false;
	}
	vsm_have_gpu_counters = false;
	vsm_ready = false;
	if (rd == nullptr) {
		return;
	}
	vsm_hw_pipeline.clear();
	vsm_hw_mesh_pipeline.clear();
	for (const KeyValue<RID, RID> &E : vsm_bin_render_pipelines) {
		if (rd->render_pipeline_is_valid(E.value)) {
			rd->free_rid(E.value);
		}
	}
	vsm_bin_render_pipelines.clear();
	vsm_bin_render_format = RD::INVALID_ID;
	if (vsm_cluster_version.is_valid()) {
		vsm_cluster_shader.version_free(vsm_cluster_version);
		vsm_cluster_version = RID();
	}
	if (vsm_hw_version.is_valid()) {
		vsm_hw_shader.version_free(vsm_hw_version);
		vsm_hw_version = RID();
	}
	if (vsm_hw_mesh_version.is_valid()) {
		vsm_hw_mesh_shader.version_free(vsm_hw_mesh_version);
		vsm_hw_mesh_version = RID();
	}
	vsm_hw_mesh_ready = false;
	if (vsm_sw_version.is_valid()) {
		vsm_sw_shader.version_free(vsm_sw_version);
		vsm_sw_version = RID();
	}
	vsm_setup_pipeline = RID();
	vsm_seed_pipeline = RID();
	vsm_bvh_pipeline = RID();
	vsm_leaf_pipeline = RID();
	vsm_sw_pipeline = RID();
	vsm_hiz_build_pipeline = RID();
	vsm_mark_dynamic_pipeline = RID();
	if (vsm_empty_fb.is_valid() && rd->framebuffer_is_valid(vsm_empty_fb)) {
		rd->free_rid(vsm_empty_fb);
	}
	vsm_empty_fb = RID();
	if (vsm_hiz_texture.is_valid()) {
		rd->free_rid(vsm_hiz_texture);
		vsm_hiz_texture = RID();
	}
	for (RID *buf : { &vsm_params_buffer, &vsm_map_meta_buffer, &vsm_counters[0], &vsm_counters[1], &vsm_counters_ring[0], &vsm_counters_ring[1], &vsm_counters_ring[2], &vsm_seed_args, &vsm_bvh_args[0], &vsm_bvh_args[1], &vsm_group_args, &vsm_raster_args, &vsm_raster_counts, &vsm_hw_count_buf, &vsm_nodes_a, &vsm_nodes_b, &vsm_groups, &vsm_clusters[0], &vsm_clusters[1], &vsm_raster_lists, &vsm_hiz_owner, &vsm_bin_state, &vsm_bin_args, &vsm_bin_draw_count, &vsm_bin_hw_counts, &vsm_bin_append, &vsm_bin_lists, &vsm_bin_table_buffer }) {
		if (buf->is_valid()) {
			rd->free_rid(*buf);
			*buf = RID();
		}
	}
	vsm_map_meta_capacity = 0;
	vsm_hiz_pool_pages = 0;
	vsm_hiz_grid_width = 0;
	vsm_hiz_pool_reallocations = UINT64_MAX;
	vsm_bin_table_capacity = 0;
	vsm_bins_ready = false;
}

void NextGeoCut::free_resources() {
	{
		MutexLock lock(s_page_req_registry_mutex);
		for (const uint64_t &token : pending_readback_tokens) {
			s_page_req_registry.erase(token);
		}
	}
	pending_readback_tokens.clear();
	for (int ring = 0; ring < 3; ring++) {
		counters_ring_pending[ring] = false;
		stats_ring_cpu_hash[ring] = 0;
		stats_ring_info[ring].clear();
		shadow_counters_ring_pending[ring] = false;
		shadow_stats_ring_meta[ring].view_count = 0;
		request_ring_pending[ring] = false;
	}
	shadow_active_view_count = 0;
	shadow_capture_timestamps = false;
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	_free_vsm_resources();
	_free_tessellation_resources();
	_free_voxel_resources();
	_release_hiz();
	for (int variant = 0; variant < 4; variant++) {
		raster_pipelines[variant].clear();
	}
	for (int variant = 0; variant < 2; variant++) {
		shadow_raster_pipelines[variant].clear();
	}
	// GodotNext: nextgeo_sw_raster (#162), nextgeo_mesh_raster (#154).
	for (int variant = 0; variant < 4; variant++) {
		export_pipelines[variant].clear();
		vis64_raster_pipelines[variant].clear();
		vis64_mesh_pipelines[variant].clear();
	}
	for (int storage = 0; storage < 2; storage++) {
		sw_pipelines[storage] = RID();
		vis64_clear_pipelines[storage] = RID();
	}
	if (sw_version.is_valid()) {
		sw_shader.version_free(sw_version);
		sw_version = RID();
	}
	if (export_version.is_valid()) {
		export_shader.version_free(export_version);
		export_version = RID();
	}
	if (vis64_raster_version.is_valid()) {
		vis64_raster_shader.version_free(vis64_raster_version);
		vis64_raster_version = RID();
	}
	if (vis64_mesh_version.is_valid()) {
		vis64_mesh_shader.version_free(vis64_mesh_version);
		vis64_mesh_version = RID();
	}
	vis64_mesh_image_ready = false;
	vis64_mesh_buffer_ready = false;
	if (vis64_framebuffer.is_valid() && rd->framebuffer_is_valid(vis64_framebuffer)) {
		rd->free_rid(vis64_framebuffer);
	}
	vis64_framebuffer = RID();
	vis64_framebuffer_size = Size2i();
	// GodotNext: nextgeo_raster_bins (#155). Pipelines go with their shaders; free only our own.
	for (const KeyValue<RID, RID> &E : raster_bin_compute_pipelines) {
		if (rd->compute_pipeline_is_valid(E.value)) {
			rd->free_rid(E.value);
		}
	}
	raster_bin_compute_pipelines.clear();
	for (const KeyValue<RID, RID> &E : raster_bin_render_pipelines) {
		if (rd->render_pipeline_is_valid(E.value)) {
			rd->free_rid(E.value);
		}
	}
	raster_bin_render_pipelines.clear();
	raster_bin_render_format = RD::INVALID_ID;
	raster_bin_prefix_pipeline = RID();
	raster_bin_scatter_pipeline = RID();
	if (raster_bin_compact_version.is_valid()) {
		raster_bin_compact_shader.version_free(raster_bin_compact_version);
		raster_bin_compact_version = RID();
	}
	raster_bins_ready = false;
	raster_bins_active = false;
	raster_bin_table_capacity = 0;
	hybrid_ready = false;
	vis64_image_ready = false;
	vis64_buffer_ready = false;
	if (raster_version.is_valid()) {
		raster_shader.version_free(raster_version);
		raster_version = RID();
	}
	if (instance_version.is_valid()) {
		instance_shader.version_free(instance_version);
		instance_version = RID();
	}
	if (multimesh_expand_version.is_valid()) {
		multimesh_expand_shader.version_free(multimesh_expand_version);
		multimesh_expand_version = RID();
	}
	if (cluster_version.is_valid()) {
		cluster_shader.version_free(cluster_version);
		cluster_version = RID();
	}
	if (hiz_version.is_valid()) {
		hiz_shader.version_free(hiz_version);
		hiz_version = RID();
	}
	instance_pipeline = RID();
	instance_phase2 = RID();
	instance_shadow_pipeline = RID();
	instance_assembly_expand_pipeline = RID();
	multimesh_expand_pipeline = RID();
	last_assembly_expand_frame = UINT64_MAX;
	cluster_bvh = RID();
	cluster_leaf = RID();
	cluster_hash = RID();
	cluster_shadow_bvh = RID();
	cluster_shadow_leaf = RID();
	cluster_shadow_hash = RID();
	hiz_pipeline = RID();
	clear_vis_pipeline = RID();
	for (RID *buffer : { &params, &raster_params, &nodes, &phase2_nodes, &groups, &clusters, &level_a, &level_b, &bvh_args[0], &bvh_args[1], &draw_args, &counters, &aux_counters, &counters_ring[0], &counters_ring[1], &counters_ring[2], &shadow_params, &shadow_raster_params, &shadow_nodes, &shadow_level_a, &shadow_level_b, &shadow_groups, &shadow_clusters, &shadow_bvh_args[0], &shadow_bvh_args[1], &shadow_draw_args, &shadow_counters, &shadow_counters_ring[0], &shadow_counters_ring[1], &shadow_counters_ring[2], &request_ring[0], &request_ring[1], &request_ring[2], &dummy_page_buf, &multimesh_expand_params, &assembly_counter_buf, &raster_args, &raster_counts, &raster_lists, &raster_bin_state, &raster_bin_args, &raster_bin_hw_counts, &raster_bin_append, &raster_bin_lists, &raster_bin_table_buffer }) {
		if (buffer->is_valid()) {
			rd->free_rid(*buffer);
			*buffer = RID();
		}
	}
	if (dummy_hiz.is_valid()) {
		rd->free_rid(dummy_hiz);
		dummy_hiz = RID();
	}
	if (dummy_sampler.is_valid()) {
		rd->free_rid(dummy_sampler);
		dummy_sampler = RID();
	}
	_free_dynamic_wind_resources();
	dynamic_wind_floor_logger.reset();
	dynamic_wind_total_dispatches = 0u;
	{
		MutexLock lock(s_stats_mutex);
		s_last_stats.clear();
		s_resolve_active_buckets = 0;
		s_resolve_stage_a_draws = 0;
		s_resolve_stage_b_draws = 0;
		s_resolve_stage_b_wrote = false;
		s_shading_stats.clear(); // GodotNext: nextgeo_shading_bins (#158)
		s_last_shadow_views.clear();
		s_last_shadow_tick = 0;
		s_last_shadow_render_frame = 0;
		s_shadow_views_dispatched = 0;
		s_shadow_raster_draws = 0;
		s_shadow_cull_gpu_ms = 0.0;
		s_shadow_raster_gpu_ms = 0.0;
		s_tessellation_enabled = false;
		s_tessellation_buffers_allocated = false;
		s_tessellation_dicing_dispatches = 0;
		s_tessellation_dicing_rate = RendererRD::NEXTGEO_TESS_DEFAULT_DICING_RATE;
		s_tess_have_gpu_stats = false;
		memset(s_tess_gpu_stats, 0, sizeof(s_tess_gpu_stats));
		s_voxels_enabled = false;
		s_voxels_buffers_allocated = false;
		s_voxel_raster_dispatches = 0;
		s_voxel_bricks_phase1 = 0;
		s_voxel_bricks_phase2 = 0;
		s_dynamic_wind_buffers_allocated = false;
		s_dynamic_wind_dispatches = 0u;
		s_dynamic_wind_total_dispatches = 0u;
		s_dynamic_wind_floor_reason = "DISABLED";
	}
	initialized = false;
}

void NextGeoCut::set_resolve_stats(uint32_t p_active_buckets, uint32_t p_stage_a_draws, uint32_t p_stage_b_draws, bool p_stage_b_wrote) {
	MutexLock lock(s_stats_mutex);
	s_resolve_active_buckets = p_active_buckets;
	s_resolve_stage_a_draws = p_stage_a_draws;
	s_resolve_stage_b_draws = p_stage_b_draws;
	s_resolve_stage_b_wrote = p_stage_b_wrote;
	s_last_stats["resolve_active_buckets"] = int64_t(p_active_buckets);
	s_last_stats["resolve_stage_a_draws"] = int64_t(p_stage_a_draws);
	s_last_stats["resolve_stage_b_draws"] = int64_t(p_stage_b_draws);
	s_last_stats["resolve_stage_b_wrote"] = p_stage_b_wrote;
}

// GodotNext: nextgeo_shading_bins (#158)
void NextGeoCut::set_shading_stats(const Dictionary &p_stats) {
	MutexLock lock(s_stats_mutex);
	s_shading_stats = p_stats.duplicate();
	for (const KeyValue<Variant, Variant> &E : s_shading_stats) {
		s_last_stats[E.key] = E.value;
	}
}

Dictionary NextGeoCut::get_last_stats() {
	MutexLock lock(s_stats_mutex);
	Dictionary stats = s_last_stats.duplicate();
	// GodotNext: nextgeo_sw_raster (#162). The raster path of the last main view, with or without
	// debug_stats: "hybrid" or "hardware", and why the device is below the Nanite tier (N1).
	stats["raster_mode"] = s_raster_mode;
	stats["raster_reason"] = s_raster_reason;
	stats["vis64_storage"] = s_vis64_storage;
	stats["sw_raster_max_edge_px"] = s_sw_raster_max_edge_px;
	stats["programmable_bins"] = int64_t(s_programmable_bins);
	stats["hw_raster_path"] = s_hw_raster_path;
	stats["hw_raster_reason"] = s_hw_raster_reason;
	// GodotNext: nextgeo_tessellation (#163).
	stats["tessellation_enabled"] = s_tessellation_enabled;
	stats["tessellation_buffers_allocated"] = s_tessellation_buffers_allocated;
	stats["tessellation_dicing_dispatches"] = int64_t(s_tessellation_dicing_dispatches);
	stats["tessellation_dicing_rate"] = double(s_tessellation_dicing_rate);
	stats["tessellation_patches"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[0] : 0u);
	stats["dicing_overflow"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[1] : 0u);
	stats["tessellation_sw_micro_tris"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[2] : 0u);
	stats["tessellation_hw_patches"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[3] : 0u);
	stats["tessellation_hw_stream_overflow"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[4] : 0u);
	stats["tessellation_coarsened_tris"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[5] : 0u);
	stats["tessellation_diced_clusters"] = int64_t(s_tess_have_gpu_stats ? s_tess_gpu_stats[6] : 0u);
	// GodotNext: nextgeo_voxels (#165).
	stats["voxels_enabled"] = s_voxels_enabled;
	stats["voxels_buffers_allocated"] = s_voxels_buffers_allocated;
	stats["voxel_raster_dispatches"] = int64_t(s_voxel_raster_dispatches);
	stats["voxel_bricks_phase1"] = int64_t(s_voxel_bricks_phase1);
	stats["voxel_bricks_phase2"] = int64_t(s_voxel_bricks_phase2);
	stats["voxel_bricks"] = int64_t(s_voxel_bricks_phase1) + int64_t(s_voxel_bricks_phase2);
	// GodotNext: nextgeo_dynamic_wind (#296).
	stats["dynamic_wind_buffers_allocated"] = s_dynamic_wind_buffers_allocated;
	stats["dynamic_wind_dispatches"] = int64_t(s_dynamic_wind_dispatches);
	stats["dynamic_wind_total_dispatches"] = int64_t(s_dynamic_wind_total_dispatches);
	stats["dynamic_wind_floor_reason"] = s_dynamic_wind_floor_reason;
	return stats;
}
