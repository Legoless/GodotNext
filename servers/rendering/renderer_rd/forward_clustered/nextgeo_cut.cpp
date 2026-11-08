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
#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/variant/array.h"
#include "servers/rendering/renderer_rd/environment/nextgeo_cull.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_visibility.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"

namespace {
struct CutParams {
	float view_proj[16];
	float prev_view_proj[16];
	float frustum[6][4];
	float camera[4];
	float view[4];
	uint32_t cull[4];
};
static_assert(sizeof(CutParams) == 272, "CutParams must stay 272 bytes.");

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
};
static_assert(sizeof(CutCounters) == 124, "CutCounters must stay 124 bytes.");

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
};
static_assert(sizeof(RasterParams) == 288, "RasterParams must stay 288 bytes.");

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
Array s_last_shadow_views;
int64_t s_last_shadow_tick = 0;
int64_t s_last_shadow_render_frame = 0;
uint32_t s_shadow_views_dispatched = 0;
uint32_t s_shadow_raster_draws = 0;
double s_shadow_cull_gpu_ms = 0.0;
double s_shadow_raster_gpu_ms = 0.0;

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
	for (uint32_t record_index = 0; record_index < p_record_count; record_index++) {
		const RendererVirtualMeshStorage::Record &record = p_storage->get_record(record_index);
		const bool cast_ok = is_shadow_view
				? ((record.flags & RendererVirtualMeshStorage::RECORD_CAST_SHADOW_MASK) != RendererVirtualMeshStorage::RECORD_CAST_SHADOW_OFF)
				: ((record.flags & RendererVirtualMeshStorage::RECORD_CAST_SHADOW_MASK) != RendererVirtualMeshStorage::RECORD_CAST_SHADOW_SHADOWS_ONLY);
		if ((record.flags & (RendererVirtualMeshStorage::RECORD_VALID | RendererVirtualMeshStorage::RECORD_VISIBLE)) != (RendererVirtualMeshStorage::RECORD_VALID | RendererVirtualMeshStorage::RECORD_VISIBLE) ||
				!cast_ok ||
				(record.flags & (RendererVirtualMeshStorage::RECORD_HAS_SKELETON | RendererVirtualMeshStorage::RECORD_HAS_INSTANCE_UNIFORMS)) != 0 ||
				(record.gi_flags & RendererVirtualMeshStorage::GI_LIGHTMAPPED) != 0 ||
				record.surface_mask == 0 ||
				(record.layer_mask & cull_mask) == 0) {
			continue;
		}
		const Vector3 lo(record.world_aabb_min[0], record.world_aabb_min[1], record.world_aabb_min[2]);
		const Vector3 hi(record.world_aabb_max[0], record.world_aabb_max[1], record.world_aabb_max[2]);
		bool visible = true;
		for (int plane = 0; plane < 6 && visible; plane++) {
			const Vector3 &n = *reinterpret_cast<const Vector3 *>(p_cut.frustum[plane]);
			Vector3 farthest = lo;
			if (n.x >= 0.0f) {
				farthest.x = hi.x;
			}
			if (n.y >= 0.0f) {
				farthest.y = hi.y;
			}
			if (n.z >= 0.0f) {
				farthest.z = hi.z;
			}
			visible = plane_distance(p_cut.frustum[plane], farthest) >= 0.0f;
		}
		if (!visible) {
			continue;
		}
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
		auto projected = [&](const float p_sphere[4], float p_error) {
			if (p_error >= 3.4e38f) {
				return tau + 1.0f;
			}
			if ((cut_flags & RendererRD::NEXTGEO_CUT_FLAG_ORTHOGRAPHIC) != 0) {
				return p_error * scale * p_cut.view[0] * lod_bias;
			}
			const Vector3 center(
					record.transform[0][0] * p_sphere[0] + record.transform[0][1] * p_sphere[1] + record.transform[0][2] * p_sphere[2] + record.transform[0][3],
					record.transform[1][0] * p_sphere[0] + record.transform[1][1] * p_sphere[1] + record.transform[1][2] * p_sphere[2] + record.transform[1][3],
					record.transform[2][0] * p_sphere[0] + record.transform[2][1] * p_sphere[1] + record.transform[2][2] * p_sphere[2] + record.transform[2][3]);
			const float radius = p_sphere[3] * scale;
			const float distance = MAX(center.distance_to(eye) - radius, znear);
			if ((cut_flags & RendererRD::NEXTGEO_CUT_FLAG_DUAL_PARABOLOID) != 0) {
				return p_error * scale * 0.5f * height / distance * lod_bias;
			}
			return p_error * scale * 0.5f * height * p11 / distance * lod_bias;
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
			if (projected(sphere, error) <= tau) {
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
			if (projected(sphere, error) <= tau) {
				continue;
			}
			const uint32_t cluster_offset = read_u32(dag, base);
			const uint32_t cluster_count = read_u32(dag, base + 4);
			const uint32_t group_page_id = read_u32(dag, base + 8);
			const bool page_has_slot = group_page_id < mesh->page_to_slot.size() && mesh->page_to_slot[group_page_id] != RendererVirtualMeshStorage::INVALID_PAGE_SLOT && slot_page_size >= 96u;
			for (uint32_t cluster = 0; cluster < cluster_count; cluster++) {
				const uint32_t global_cluster = cluster_offset + cluster;
				float cluster_cut_err = 0.0f;
				if (page_has_slot && global_cluster >= mesh->record.cluster_offset) {
					const uint32_t local_cluster = global_cluster - mesh->record.cluster_offset;
					if (local_cluster < mesh->cluster_refine.size() && mesh->cluster_refine[local_cluster].valid) {
						const RendererVirtualMeshStorage::ClusterRefineInfo &ref = mesh->cluster_refine[local_cluster];
						if (ref.refined_group >= 0 && uint32_t(ref.refined_group) < group_count) {
							const float child_err = projected(ref.group_lod, ref.group_error);
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
	}));
	instance_version = instance_shader.version_create();
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
	if (!instance_version.is_valid() || !cluster_version.is_valid() || !hiz_version.is_valid() || !raster_version.is_valid()) {
		failed = true;
		return false;
	}
	instance_pipeline = rd->compute_pipeline_create(instance_shader.version_get_shader(instance_version, 0));
	instance_phase2 = rd->compute_pipeline_create(instance_shader.version_get_shader(instance_version, 1));
	instance_shadow_pipeline = rd->compute_pipeline_create(instance_shader.version_get_shader(instance_version, 2));
	cluster_bvh = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 0));
	cluster_leaf = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 1));
	cluster_hash = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 2));
	cluster_shadow_bvh = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 3));
	cluster_shadow_leaf = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 4));
	cluster_shadow_hash = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 5));
	hiz_pipeline = rd->compute_pipeline_create(hiz_shader.version_get_shader(hiz_version, 0));
	clear_vis_pipeline = rd->compute_pipeline_create(hiz_shader.version_get_shader(hiz_version, 1));
	if (!instance_pipeline.is_valid() || !instance_phase2.is_valid() || !instance_shadow_pipeline.is_valid() || !cluster_bvh.is_valid() || !cluster_leaf.is_valid() || !cluster_hash.is_valid() || !cluster_shadow_bvh.is_valid() || !cluster_shadow_leaf.is_valid() || !cluster_shadow_hash.is_valid() || !hiz_pipeline.is_valid() || !clear_vis_pipeline.is_valid()) {
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
	dummy_zero.resize(16);
	dummy_zero.fill(0);
	dummy_page_buf = rd->storage_buffer_create(16, dummy_zero);

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
	initialized = params.is_valid() && raster_params.is_valid() && counters.is_valid() && aux_counters.is_valid() && shadow_params.is_valid() && shadow_raster_params.is_valid() && shadow_counters.is_valid() && counters_ring[0].is_valid() && counters_ring[1].is_valid() && counters_ring[2].is_valid() && shadow_counters_ring[0].is_valid() && shadow_counters_ring[1].is_valid() && shadow_counters_ring[2].is_valid() && request_ring[0].is_valid() && request_ring[1].is_valid() && request_ring[2].is_valid() && nodes.is_valid() && phase2_nodes.is_valid() && groups.is_valid() && clusters.is_valid() && level_a.is_valid() && level_b.is_valid() && shadow_nodes.is_valid() && shadow_level_a.is_valid() && shadow_level_b.is_valid() && shadow_groups.is_valid() && shadow_clusters.is_valid() && bvh_args[0].is_valid() && bvh_args[1].is_valid() && draw_args.is_valid() && shadow_bvh_args[0].is_valid() && shadow_bvh_args[1].is_valid() && shadow_draw_args.is_valid() && dummy_page_buf.is_valid() && dummy_hiz.is_valid() && dummy_sampler.is_valid();
	if (!initialized) {
		failed = true;
	}
	return initialized;
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
	RID shader = raster_shader.version_get_shader(raster_version, variant);
	RD::Uniform u0(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, RSG::virtual_mesh_storage->get_record_buffer());
	RD::Uniform u1(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RSG::virtual_mesh_storage->get_mesh_record_buffer());
	RD::Uniform u2(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, page_buf);
	RD::Uniform u3(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, cluster_buf);
	RD::Uniform u4(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, clusters);
	RD::Uniform u5(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, raster_params);
	RD::Uniform u6(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, bucket_buf);
	RID set = uniform_set_cache->get_cache(shader, 0, u0, u1, u2, u3, u4, u5, u6);
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

void NextGeoCut::_set_common_uniforms(LocalVector<RD::Uniform> &r_uniforms, RID p_counters, RID p_hiz_tex, RID p_input, RID p_output, RID p_groups, RID p_clusters, RID p_bvh_args_out, RID p_request_buf) {
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
}

void NextGeoCut::_set_shadow_uniforms(LocalVector<RD::Uniform> &r_uniforms, RID p_counters, RID p_input, RID p_output, RID p_groups, RID p_clusters, RID p_bvh_args_out) {
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
		info["shadow_views"] = s_last_shadow_views.duplicate(true);
		info["shadow_tick"] = s_last_shadow_tick;
		info["shadow_render_frame"] = s_last_shadow_render_frame;
		info["shadow_views_dispatched"] = int64_t(s_shadow_views_dispatched);
		info["shadow_raster_draws"] = int64_t(s_shadow_raster_draws);
		info["shadow_cull_gpu_ms"] = s_shadow_cull_gpu_ms;
		info["shadow_raster_gpu_ms"] = s_shadow_raster_gpu_ms;
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
	const uint32_t record_count = RSG::virtual_mesh_storage->get_record_count();
	if (record_count == 0 || !RSG::virtual_mesh_storage->get_record_buffer().is_valid() || !RSG::virtual_mesh_storage->get_mesh_record_buffer().is_valid() || !RSG::virtual_mesh_storage->get_dag_buffer().is_valid()) {
		return false;
	}
	uint32_t max_levels = RSG::virtual_mesh_storage->get_max_bvh_levels();
	if (max_levels == 0) {
		return false;
	}

	const bool can_raster = p_raster != nullptr && p_raster->visibility.is_valid() && p_raster->instance_id.is_valid() && p_raster->depth.is_valid();
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
		rd->buffer_update(raster_params, 0, sizeof(RasterParams), &rp);
		_clear_visibility_targets(p_raster->visibility, p_raster->instance_id, p_internal_size);
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
	RENDER_TIMESTAMP("NextGeo BVH");
	RID input = nodes;
	for (uint32_t level = 0; level < max_levels; level++) {
		const RID output = (level % 2 == 0) ? level_a : level_b;
		const RID args_in = bvh_args[level & 1u];
		const RID args_out = bvh_args[(level + 1u) & 1u];
		LocalVector<RD::Uniform> uniforms;
		_set_common_uniforms(uniforms, active_counters, prev_hiz_tex, input, output, groups, clusters, args_out, request_buf);
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
		if (_draw_raster_phase(0, 0u, *p_raster)) {
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
		_set_common_uniforms(leaf_uniforms, active_counters, prev_hiz_tex, groups, level_a, dummy_page_buf, clusters, bvh_args[0], request_buf);
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
		if (_draw_raster_phase(1, PHASE2_CLUSTER_BASE, *p_raster)) {
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
		if (capture_stats) {
			LocalVector<RD::Uniform> uniforms;
			_set_common_uniforms(uniforms, active_counters, prev_hiz_tex, level_b, level_a, groups, clusters, bvh_args[0], dummy_page_buf);
			RID set = _uniform_set(uniforms, 2);
			if (set.is_valid()) {
				list = rd->compute_list_begin();
				rd->compute_list_bind_compute_pipeline(list, cluster_hash);
				rd->compute_list_bind_uniform_set(list, set, 0);
				rd->compute_list_dispatch(list, (PHASE2_CLUSTER_BASE + 63) / 64, 1, 1);
				rd->compute_list_end();
			}
			_request_stats(stats_slot, active_counters, record_count, max_levels, &cut, use_occlusion, have_prev, raster_draws);
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
	const uint32_t record_count = RSG::virtual_mesh_storage->get_record_count();
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

	RID input = shadow_nodes;
	for (uint32_t level = 0; level < max_levels; level++) {
		const RID output = (level % 2 == 0) ? shadow_level_a : shadow_level_b;
		const RID args_in = shadow_bvh_args[level & 1u];
		const RID args_out = shadow_bvh_args[(level + 1u) & 1u];
		LocalVector<RD::Uniform> uniforms;
		_set_shadow_uniforms(uniforms, active_counters, input, output, shadow_groups, shadow_clusters, args_out);
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
		_set_shadow_uniforms(uniforms, active_counters, shadow_level_b, shadow_level_a, shadow_groups, shadow_clusters, shadow_bvh_args[0]);
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
	RID shader = raster_shader.version_get_shader(raster_version, 4 + shadow_variant);
	RD::Uniform u0(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, RSG::virtual_mesh_storage->get_record_buffer());
	RD::Uniform u1(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RSG::virtual_mesh_storage->get_mesh_record_buffer());
	RD::Uniform u2(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, page_buf);
	RD::Uniform u3(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, cluster_buf);
	RD::Uniform u4(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, shadow_clusters);
	RD::Uniform u5(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, shadow_raster_params);
	RD::Uniform u6(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, dummy_page_buf);
	RID set = uniform_set_cache->get_cache(shader, 0, u0, u1, u2, u3, u4, u5, u6);
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
	_release_hiz();
	for (int variant = 0; variant < 4; variant++) {
		raster_pipelines[variant].clear();
	}
	for (int variant = 0; variant < 2; variant++) {
		shadow_raster_pipelines[variant].clear();
	}
	if (raster_version.is_valid()) {
		raster_shader.version_free(raster_version);
		raster_version = RID();
	}
	if (instance_version.is_valid()) {
		instance_shader.version_free(instance_version);
		instance_version = RID();
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
	cluster_bvh = RID();
	cluster_leaf = RID();
	cluster_hash = RID();
	cluster_shadow_bvh = RID();
	cluster_shadow_leaf = RID();
	cluster_shadow_hash = RID();
	hiz_pipeline = RID();
	clear_vis_pipeline = RID();
	for (RID *buffer : { &params, &raster_params, &nodes, &phase2_nodes, &groups, &clusters, &level_a, &level_b, &bvh_args[0], &bvh_args[1], &draw_args, &counters, &aux_counters, &counters_ring[0], &counters_ring[1], &counters_ring[2], &shadow_params, &shadow_raster_params, &shadow_nodes, &shadow_level_a, &shadow_level_b, &shadow_groups, &shadow_clusters, &shadow_bvh_args[0], &shadow_bvh_args[1], &shadow_draw_args, &shadow_counters, &shadow_counters_ring[0], &shadow_counters_ring[1], &shadow_counters_ring[2], &request_ring[0], &request_ring[1], &request_ring[2], &dummy_page_buf }) {
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
	{
		MutexLock lock(s_stats_mutex);
		s_last_stats.clear();
		s_resolve_active_buckets = 0;
		s_resolve_stage_a_draws = 0;
		s_resolve_stage_b_draws = 0;
		s_resolve_stage_b_wrote = false;
		s_last_shadow_views.clear();
		s_last_shadow_tick = 0;
		s_last_shadow_render_frame = 0;
		s_shadow_views_dispatched = 0;
		s_shadow_raster_draws = 0;
		s_shadow_cull_gpu_ms = 0.0;
		s_shadow_raster_gpu_ms = 0.0;
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

Dictionary NextGeoCut::get_last_stats() {
	MutexLock lock(s_stats_mutex);
	return s_last_stats.duplicate();
}
