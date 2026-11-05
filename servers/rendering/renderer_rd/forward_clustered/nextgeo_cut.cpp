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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,         */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "nextgeo_cut.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/environment/nextgeo_cull.h"
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
	uint32_t pad[2];
};
static_assert(sizeof(CutCounters) == 68, "CutCounters must stay 68 bytes.");

Dictionary s_last_stats;

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
static uint64_t CPU_cut_hash(const RendererVirtualMeshStorage *p_storage, const CutParams &p_cut, uint32_t p_record_count, uint32_t p_max_levels, uint32_t &r_instances, uint32_t &r_groups, uint32_t &r_clusters) {
	LocalVector<uint64_t> pairs;
	const uint32_t cull_mask = p_cut.cull[0];
	const float tau = p_cut.camera[3];
	const float height = p_cut.view[1];
	const float znear = p_cut.view[2];
	const float lod_bias = p_cut.view[3];
	const float p11 = p_cut.view_proj[5]; // column-major [1][1]
	const Vector3 eye(p_cut.camera[0], p_cut.camera[1], p_cut.camera[2]);
	auto plane_distance = [&](const float p_plane[4], const Vector3 &p_point) {
		return p_plane[0] * p_point.x + p_plane[1] * p_point.y + p_plane[2] * p_point.z + p_plane[3];
	};
	for (uint32_t record_index = 0; record_index < p_record_count; record_index++) {
		const RendererVirtualMeshStorage::Record &record = p_storage->get_record(record_index);
		if ((record.flags & (RendererVirtualMeshStorage::RECORD_VALID | RendererVirtualMeshStorage::RECORD_VISIBLE)) != (RendererVirtualMeshStorage::RECORD_VALID | RendererVirtualMeshStorage::RECORD_VISIBLE) ||
				(record.flags & (RendererVirtualMeshStorage::RECORD_HAS_SKELETON | RendererVirtualMeshStorage::RECORD_HAS_INSTANCE_UNIFORMS)) != 0 ||
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
			const Vector3 center(
					record.transform[0][0] * p_sphere[0] + record.transform[0][1] * p_sphere[1] + record.transform[0][2] * p_sphere[2] + record.transform[0][3],
					record.transform[1][0] * p_sphere[0] + record.transform[1][1] * p_sphere[1] + record.transform[1][2] * p_sphere[2] + record.transform[1][3],
					record.transform[2][0] * p_sphere[0] + record.transform[2][1] * p_sphere[1] + record.transform[2][2] * p_sphere[2] + record.transform[2][3]);
			const float radius = p_sphere[3] * scale;
			const float distance = MAX(center.distance_to(eye) - radius, znear);
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
		for (const uint32_t &group_index : groups) {
			if (group_index >= group_count) {
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
			for (uint32_t cluster = 0; cluster < cluster_count; cluster++) {
				pairs.push_back((uint64_t(record_index) << 32) | uint64_t(cluster_offset + cluster));
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
	instance_shader.initialize(Vector<String>({ "", "\n#define PHASE2\n" }));
	instance_version = instance_shader.version_create();
	cluster_shader.initialize(Vector<String>({ "", "\n#define CLUSTER_CULL\n" }));
	cluster_version = cluster_shader.version_create();
	hiz_shader.initialize(Vector<String>({ "" }));
	hiz_version = hiz_shader.version_create();
	if (!instance_version.is_valid() || !cluster_version.is_valid() || !hiz_version.is_valid()) {
		failed = true;
		return false;
	}
	instance_pipeline = rd->compute_pipeline_create(instance_shader.version_get_shader(instance_version, 0));
	instance_phase2 = rd->compute_pipeline_create(instance_shader.version_get_shader(instance_version, 1));
	cluster_bvh = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 0));
	cluster_leaf = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 1));
	hiz_pipeline = rd->compute_pipeline_create(hiz_shader.version_get_shader(hiz_version, 0));
	if (!instance_pipeline.is_valid() || !instance_phase2.is_valid() || !cluster_bvh.is_valid() || !cluster_leaf.is_valid() || !hiz_pipeline.is_valid()) {
		failed = true;
		return false;
	}

	params = _storage(sizeof(CutParams));
	counters = _storage(sizeof(CutCounters));
	nodes = _storage(NODE_CAPACITY * 8);
	phase2_nodes = _storage(NODE_CAPACITY * 8);
	groups = _storage(GROUP_CAPACITY * 8);
	clusters = _storage(CLUSTER_CAPACITY * 8);
	level_a = _storage(NODE_CAPACITY * 8);
	level_b = _storage(NODE_CAPACITY * 8);

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
	initialized = params.is_valid() && counters.is_valid() && nodes.is_valid() && phase2_nodes.is_valid() && groups.is_valid() && clusters.is_valid() && level_a.is_valid() && level_b.is_valid() && dummy_hiz.is_valid() && dummy_sampler.is_valid();
	if (!initialized) {
		failed = true;
	}
	return initialized;
}

RID NextGeoCut::_uniform_set(const Vector<RD::Uniform> &p_uniforms, int p_variant) {
	return RD::get_singleton()->uniform_set_create(p_uniforms, cluster_shader.version_get_shader(cluster_version, p_variant), 0);
}

void NextGeoCut::_set_common_uniforms(Vector<RD::Uniform> &r_uniforms, RID p_input, RID p_output, RID p_groups, RID p_clusters) {
	RID records = RSG::virtual_mesh_storage->get_record_buffer();
	RID mesh_records = RSG::virtual_mesh_storage->get_mesh_record_buffer();
	RID dag = RSG::virtual_mesh_storage->get_dag_buffer();
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, records));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, mesh_records));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, dag));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, params));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, nodes));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, counters));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, nodes));
	RD::Uniform hiz(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ dummy_sampler, dummy_hiz }));
	r_uniforms.push_back(hiz);
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, p_input));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, p_output));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, p_groups));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, p_clusters));
}

void NextGeoCut::dispatch(RendererVirtualMeshStorage *p_storage, RID p_depth, const Projection &p_view_projection, const Projection &p_prev_view_projection, const Vector3 &p_camera, const Size2i &p_internal_size, uint32_t p_cull_mask, float p_max_pixel_error, float p_lod_bias, bool p_occlusion, bool p_debug_stats) {
	(void)p_storage;
	if (!_ensure()) {
		return;
	}
	RenderingDevice *rd = RD::get_singleton();
	const uint32_t record_count = RSG::virtual_mesh_storage->get_record_count();
	if (record_count == 0 || !RSG::virtual_mesh_storage->get_record_buffer().is_valid() || !RSG::virtual_mesh_storage->get_mesh_record_buffer().is_valid() || !RSG::virtual_mesh_storage->get_dag_buffer().is_valid()) {
		return;
	}
	uint32_t max_levels = RSG::virtual_mesh_storage->get_max_bvh_levels();
	if (max_levels == 0) {
		return;
	}

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
	cut.cull[3] = p_occlusion ? 1 : 0;
	rd->buffer_update(params, 0, sizeof(CutParams), &cut);
	CutCounters zero;
	memset(&zero, 0, sizeof(CutCounters));
	rd->buffer_update(counters, 0, sizeof(CutCounters), &zero);

	// Persistent previous-frame depth for occlusion. Query the real depth
	// texture; a first frame has no previous data (cleared to far).
	const Size2i hiz_target_size(MAX(p_internal_size.x / 2, 1), MAX(p_internal_size.y / 2, 1));
	bool have_prev = false;
	if (p_occlusion && p_depth.is_valid()) {
		if (!hiz_prev.is_valid() || hiz_size != hiz_target_size) {
			if (hiz_prev.is_valid()) {
				rd->free_rid(hiz_prev);
			}
			RD::TextureFormat hiz_format;
			hiz_format.format = RD::DATA_FORMAT_R32_SFLOAT;
			hiz_format.width = hiz_target_size.x;
			hiz_format.height = hiz_target_size.y;
			hiz_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
			hiz_prev = rd->texture_create(hiz_format, RD::TextureView());
			hiz_size = hiz_target_size;
			if (hiz_prev.is_valid()) {
				Vector<uint8_t> clear;
				clear.resize(hiz_target_size.x * hiz_target_size.y * 4);
				clear.fill(0); // 0 = far in reverse-Z, so the first frame culls nothing.
				rd->texture_update(hiz_prev, 0, clear);
			}
		}
		have_prev = hiz_prev.is_valid();
	}

	// Instance cull: level 0 seed.
	RENDER_TIMESTAMP("NextGeo Instance Cull");
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, instance_pipeline);
	{
		Vector<RD::Uniform> uniforms = {
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, RSG::virtual_mesh_storage->get_record_buffer()),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RSG::virtual_mesh_storage->get_mesh_record_buffer()),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, RSG::virtual_mesh_storage->get_dag_buffer()),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, params),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, nodes),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, counters),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, nodes),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ dummy_sampler, have_prev ? hiz_prev : dummy_hiz })),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, phase2_nodes),
		};
		RID set = rd->uniform_set_create(uniforms, instance_shader.version_get_shader(instance_version, 0), 0);
		if (!set.is_valid()) {
			rd->compute_list_end();
			return;
		}
		rd->compute_list_bind_uniform_set(list, set, 0);
		rd->compute_list_dispatch(list, (record_count + 63) / 64, 1, 1);
	}
	rd->compute_list_end();

	// Phase 2: retest the occluded instances against the current depth.
	RENDER_TIMESTAMP("NextGeo Phase2 Cull");
	if (p_occlusion && p_depth.is_valid()) {
		Vector<RD::Uniform> uniforms = {
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, RSG::virtual_mesh_storage->get_record_buffer()),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RSG::virtual_mesh_storage->get_mesh_record_buffer()),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, RSG::virtual_mesh_storage->get_dag_buffer()),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, params),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, nodes),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, counters),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, nodes),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ dummy_sampler, p_depth })),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, phase2_nodes),
		};
		RID set = rd->uniform_set_create(uniforms, instance_shader.version_get_shader(instance_version, 1), 0);
		if (set.is_valid()) {
			list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, instance_phase2);
			rd->compute_list_bind_uniform_set(list, set, 0);
			rd->compute_list_dispatch(list, (NODE_CAPACITY + 63) / 64, 1, 1);
			rd->compute_list_end();
		}
	}

	// BVH levels: one dispatch per level, ping-ponging the node lists.
	RENDER_TIMESTAMP("NextGeo BVH");
	RID input = nodes;
	for (uint32_t level = 0; level < max_levels; level++) {
		const RID output = (level % 2 == 0) ? level_a : level_b;
		Vector<RD::Uniform> uniforms;
		_set_common_uniforms(uniforms, input, output, groups, clusters);
		RID set = _uniform_set(uniforms, 0);
		if (!set.is_valid()) {
			break;
		}
		list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, cluster_bvh);
		rd->compute_list_bind_uniform_set(list, set, 0);
		const uint32_t push[4] = { level, NODE_CAPACITY, 0, 0 };
		rd->compute_list_set_push_constant(list, push, sizeof(push));
		rd->compute_list_dispatch(list, (NODE_CAPACITY + 63) / 64, 1, 1);
		rd->compute_list_end();
		input = output;
	}

	// Leaf groups to compacted clusters.
	RENDER_TIMESTAMP("NextGeo Cluster Cull");
	{
		Vector<RD::Uniform> uniforms;
		_set_common_uniforms(uniforms, groups, level_a, groups, clusters);
		RID set = _uniform_set(uniforms, 1);
		if (set.is_valid()) {
			list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, cluster_leaf);
			rd->compute_list_bind_uniform_set(list, set, 0);
			const uint32_t push[4] = { 0, GROUP_CAPACITY, 0, 0 };
			rd->compute_list_set_push_constant(list, push, sizeof(push));
			rd->compute_list_dispatch(list, (GROUP_CAPACITY + 63) / 64, 1, 1);
			rd->compute_list_end();
		}
	}

	const uint32_t stats_interval = MAX(uint32_t(GLOBAL_GET("rendering/nextgeo/debug_stats_interval")), 1u);
	const bool read_stats = p_debug_stats && (stats_counter++ % stats_interval == 0);
	if (read_stats) {
		stats_tick++;
		const Vector<uint8_t> raw = rd->buffer_get_data(counters, 0, sizeof(CutCounters));
		if (raw.size() < int(sizeof(CutCounters))) {
			return;
		}
		const CutCounters *stats = reinterpret_cast<const CutCounters *>(raw.ptr());
		Dictionary info;
		info["tick"] = int64_t(stats_tick);
		info["record_count"] = int64_t(record_count);
		info["used_records"] = int64_t(RSG::virtual_mesh_storage->get_record_count());
		{
			const RendererVirtualMeshStorage::Record &first = RSG::virtual_mesh_storage->get_record(0);
			info["first_virtual_mesh"] = int64_t(first.virtual_mesh);
			const RendererVirtualMeshStorage::MeshInfo *mesh = RSG::virtual_mesh_storage->get_mesh_info(first.virtual_mesh);
			if (mesh != nullptr) {
				info["first_group_offset"] = int64_t(mesh->record.group_offset);
				info["first_bvh_offset"] = int64_t(mesh->record.bvh_node_offset);
				info["first_levels"] = int64_t(mesh->record.bvh_level_count);
				info["first_dag_size"] = int64_t(mesh->dag_size);
				info["first_dag_offset"] = int64_t(mesh->dag_offset);
			}
		}
		info["hiz_tested"] = int64_t(stats->pad[0]);
		info["hiz_hit"] = int64_t(stats->pad[1]);
		info["hiz_behind"] = int64_t(stats->cut_lo);
		info["hiz_oob"] = int64_t(stats->cut_hi);
		info["phase1_instances"] = int64_t(stats->phase1);
		info["phase2_instances"] = int64_t(stats->phase2);
		info["bvh_levels"] = int64_t(max_levels);
		info["groups"] = int64_t(stats->group_count);
		info["clusters"] = int64_t(stats->cluster_count);
		info["overflow"] = int64_t(stats->overflow);
		info["occlusion"] = p_occlusion;
		info["hiz"] = have_prev;
		info["cluster_capacity"] = int64_t(CLUSTER_CAPACITY);
		if (stats->cluster_count > 0 && stats->cluster_count <= CLUSTER_CAPACITY) {
			const Vector<uint8_t> raw_clusters = rd->buffer_get_data(clusters, 0, stats->cluster_count * 8);
			LocalVector<uint64_t> pairs;
			pairs.resize(stats->cluster_count);
			for (uint32_t index = 0; index < stats->cluster_count; index++) {
				uint32_t record = 0;
				uint32_t cluster = 0;
				memcpy(&record, raw_clusters.ptr() + index * 8, sizeof(uint32_t));
				memcpy(&cluster, raw_clusters.ptr() + index * 8 + 4, sizeof(uint32_t));
				pairs[index] = (uint64_t(record) << 32) | uint64_t(cluster);
			}
			info["cut_hash"] = int64_t(RendererRD::nextgeo_cut_hash(pairs));
		} else {
			info["cut_hash"] = int64_t(0);
		}
		uint32_t cpu_instances = 0;
		uint32_t cpu_groups = 0;
		uint32_t cpu_clusters = 0;
		const uint64_t cpu_hash = CPU_cut_hash(RSG::virtual_mesh_storage, cut, record_count, max_levels, cpu_instances, cpu_groups, cpu_clusters);
		info["cpu_instances"] = int64_t(cpu_instances);
		info["cpu_groups"] = int64_t(cpu_groups);
		info["cpu_clusters"] = int64_t(cpu_clusters);
		info["cpu_cut_hash"] = int64_t(cpu_hash);
		info["cut_match"] = cpu_hash == uint64_t(info["cut_hash"]);
		s_last_stats = info;
	}

	// Build this frame's half-resolution Hi-Z from the current depth for the
	// next frame's occlusion test.
	if (p_occlusion && p_depth.is_valid() && hiz_prev.is_valid() && hiz_size == hiz_target_size) {
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ dummy_sampler, p_depth })));
		RD::Uniform target(RD::UNIFORM_TYPE_IMAGE, 1, hiz_prev);
		uniforms.push_back(target);
		RID set = rd->uniform_set_create(uniforms, hiz_shader.version_get_shader(hiz_version, 0), 0);
		if (set.is_valid()) {
			const uint32_t push[4] = { uint32_t(MAX(p_internal_size.x, 1)), uint32_t(MAX(p_internal_size.y, 1)), uint32_t(hiz_target_size.x), uint32_t(hiz_target_size.y) };
			list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, hiz_pipeline);
			rd->compute_list_bind_uniform_set(list, set, 0);
			rd->compute_list_set_push_constant(list, push, sizeof(push));
			rd->compute_list_dispatch(list, (hiz_target_size.x + 7) / 8, (hiz_target_size.y + 7) / 8, 1);
			rd->compute_list_end();
		}
	}
}

void NextGeoCut::free_resources() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
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
	hiz_pipeline = RID();
	cluster_bvh = RID();
	cluster_leaf = RID();
	for (RID *buffer : { &params, &nodes, &phase2_nodes, &groups, &clusters, &level_a, &level_b, &counters }) {
		if (buffer->is_valid()) {
			rd->free_rid(*buffer);
			*buffer = RID();
		}
	}
	if (dummy_hiz.is_valid()) {
		rd->free_rid(dummy_hiz);
		dummy_hiz = RID();
	}
	if (hiz_prev.is_valid()) {
		rd->free_rid(hiz_prev);
		hiz_prev = RID();
	}
	if (dummy_sampler.is_valid()) {
		rd->free_rid(dummy_sampler);
		dummy_sampler = RID();
	}
	initialized = false;
}

Dictionary NextGeoCut::get_last_stats() {
	return s_last_stats;
}
