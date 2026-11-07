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

#include "core/config/project_settings.h"
#include "core/os/mutex.h"
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

Mutex s_stats_mutex;
Dictionary s_last_stats;
uint32_t s_resolve_active_buckets = 0;
uint32_t s_resolve_stage_a_draws = 0;
uint32_t s_resolve_stage_b_draws = 0;
bool s_resolve_stage_b_wrote = false;

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
	const float p11 = Math::abs(p_cut.view_proj[5]); // column-major [1][1]; the depth correction flips y
	const Vector3 eye(p_cut.camera[0], p_cut.camera[1], p_cut.camera[2]);
	auto plane_distance = [&](const float p_plane[4], const Vector3 &p_point) {
		return p_plane[0] * p_point.x + p_plane[1] * p_point.y + p_plane[2] * p_point.z + p_plane[3];
	};
	for (uint32_t record_index = 0; record_index < p_record_count; record_index++) {
		const RendererVirtualMeshStorage::Record &record = p_storage->get_record(record_index);
		if ((record.flags & (RendererVirtualMeshStorage::RECORD_VALID | RendererVirtualMeshStorage::RECORD_VISIBLE)) != (RendererVirtualMeshStorage::RECORD_VALID | RendererVirtualMeshStorage::RECORD_VISIBLE) ||
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
#ifdef REAL_T_IS_DOUBLE
	const String base_defines = "\n#define REAL_T_IS_DOUBLE\n";
#else
	const String base_defines = "";
#endif
	instance_shader.initialize(Vector<String>({ base_defines, base_defines + "\n#define PHASE2\n" }));
	instance_version = instance_shader.version_create();
	cluster_shader.initialize(Vector<String>({ base_defines, base_defines + "\n#define CLUSTER_CULL\n", base_defines + "\n#define CUT_HASH\n" }));
	cluster_version = cluster_shader.version_create();
	hiz_shader.initialize(Vector<String>({ "", "\n#define CLEAR_VISIBILITY\n" }));
	hiz_version = hiz_shader.version_create();
	raster_shader.initialize(Vector<String>({
			base_defines,
			base_defines + "\n#define WITH_VELOCITY\n",
			base_defines + "\n#define DEBUG_BACKFACE\n",
			base_defines + "\n#define WITH_VELOCITY\n#define DEBUG_BACKFACE\n",
	}));
	raster_version = raster_shader.version_create();
	if (!instance_version.is_valid() || !cluster_version.is_valid() || !hiz_version.is_valid() || !raster_version.is_valid()) {
		failed = true;
		return false;
	}
	instance_pipeline = rd->compute_pipeline_create(instance_shader.version_get_shader(instance_version, 0));
	instance_phase2 = rd->compute_pipeline_create(instance_shader.version_get_shader(instance_version, 1));
	cluster_bvh = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 0));
	cluster_leaf = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 1));
	cluster_hash = rd->compute_pipeline_create(cluster_shader.version_get_shader(cluster_version, 2));
	hiz_pipeline = rd->compute_pipeline_create(hiz_shader.version_get_shader(hiz_version, 0));
	clear_vis_pipeline = rd->compute_pipeline_create(hiz_shader.version_get_shader(hiz_version, 1));
	if (!instance_pipeline.is_valid() || !instance_phase2.is_valid() || !cluster_bvh.is_valid() || !cluster_leaf.is_valid() || !cluster_hash.is_valid() || !hiz_pipeline.is_valid() || !clear_vis_pipeline.is_valid()) {
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

	params = _storage(sizeof(CutParams));
	raster_params = _storage(sizeof(RasterParams));
	counters = _storage(sizeof(CutCounters));
	nodes = _storage(NODE_CAPACITY * 8);
	phase2_nodes = _storage(NODE_CAPACITY * 8);
	groups = _storage(GROUP_CAPACITY * 8);
	clusters = _storage(CLUSTER_CAPACITY * 8);
	level_a = _storage(NODE_CAPACITY * 8);
	level_b = _storage(NODE_CAPACITY * 8);
	bvh_args[0] = rd->storage_buffer_create(18 * 4 * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	bvh_args[1] = rd->storage_buffer_create(18 * 4 * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	draw_args = rd->storage_buffer_create(2 * sizeof(RendererRD::NextGeoDrawIndirectCommand), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
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
	initialized = params.is_valid() && raster_params.is_valid() && counters.is_valid() && nodes.is_valid() && phase2_nodes.is_valid() && groups.is_valid() && clusters.is_valid() && level_a.is_valid() && level_b.is_valid() && bvh_args[0].is_valid() && bvh_args[1].is_valid() && draw_args.is_valid() && dummy_page_buf.is_valid() && dummy_hiz.is_valid() && dummy_sampler.is_valid();
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

void NextGeoCut::_set_common_uniforms(LocalVector<RD::Uniform> &r_uniforms, RID p_input, RID p_output, RID p_groups, RID p_clusters, RID p_bvh_args_out) {
	RID records = RSG::virtual_mesh_storage->get_record_buffer();
	RID mesh_records = RSG::virtual_mesh_storage->get_mesh_record_buffer();
	RID dag = RSG::virtual_mesh_storage->get_dag_buffer();
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, records));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, mesh_records));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, dag));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, params));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, dummy_page_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, counters));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, dummy_page_buf));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ dummy_sampler, hiz_texture[0].is_valid() ? hiz_texture[0] : dummy_hiz })));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, p_input));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, p_output));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, p_groups));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, p_clusters));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, p_bvh_args_out));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, draw_args));
}

void NextGeoCut::_request_stats(uint32_t p_record_count, uint32_t p_max_levels, const void *p_cut, bool p_occlusion, bool p_have_prev, uint32_t p_raster_draws) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	stats_info.clear();
	stats_info["record_count"] = int64_t(p_record_count);
	stats_info["bvh_levels"] = int64_t(p_max_levels);
	stats_info["occlusion"] = p_occlusion;
	stats_info["hiz"] = p_have_prev;
	stats_info["hiz_mips"] = int64_t(hiz_mips);
	stats_info["cluster_capacity"] = int64_t(CLUSTER_CAPACITY);
	stats_info["raster_indirect_draws"] = int64_t(p_raster_draws);
	stats_info["nextgeo_double_dispatch"] = int64_t(RSG::virtual_mesh_storage->nextgeo_get_double_dispatch());
	stats_info["nextgeo_missing"] = int64_t(RSG::virtual_mesh_storage->nextgeo_get_missing());

	uint32_t cpu_instances = 0;
	uint32_t cpu_groups = 0;
	uint32_t cpu_clusters = 0;
	stats_cpu_hash = CPU_cut_hash(RSG::virtual_mesh_storage, *static_cast<const CutParams *>(p_cut), p_record_count, p_max_levels, cpu_instances, cpu_groups, cpu_clusters);
	stats_info["cpu_instances"] = int64_t(cpu_instances);
	stats_info["cpu_groups"] = int64_t(cpu_groups);
	stats_info["cpu_clusters"] = int64_t(cpu_clusters);
	if (bool(GLOBAL_GET("rendering/nextgeo/hiz_debug")) && hiz_texture[0].is_valid() && hiz_texture[1].is_valid()) {
		for (int texture = 0; texture < 2; texture++) {
			const Vector<uint8_t> raw = rd->texture_get_data(hiz_texture[texture], 0);
			float center = -1.0f;
			if (raw.size() >= int(hiz_size.x * hiz_size.y * 4)) {
				const int index = ((hiz_size.y / 2) * hiz_size.x + hiz_size.x / 2) * 4;
				memcpy(&center, raw.ptr() + index, sizeof(float));
			}
			stats_info[texture == 0 ? "hiz_prev_center" : "hiz_cur_center"] = center;
		}
	}

	// The readback is taken at the start of the next frame, before the
	// counters are reset: the GPU cut and the CPU reference then describe the
	// same frame without a second buffer or an async request per frame.
	stats_pending_read = true;
}

void NextGeoCut::_publish_stats(const PackedByteArray &p_data) {
	if (p_data.size() < int(sizeof(CutCounters))) {
		return;
	}
	const CutCounters *stats = reinterpret_cast<const CutCounters *>(p_data.ptr());
	Dictionary info = stats_info;
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
	info["cpu_cut_hash"] = int64_t(stats_cpu_hash);
	info["cut_match"] = gpu_hash == stats_cpu_hash;
	stats_tick++;
	info["tick"] = int64_t(stats_tick);
	{
		MutexLock lock(s_stats_mutex);
		info["resolve_active_buckets"] = int64_t(s_resolve_active_buckets);
		info["resolve_stage_a_draws"] = int64_t(s_resolve_stage_a_draws);
		info["resolve_stage_b_draws"] = int64_t(s_resolve_stage_b_draws);
		info["resolve_stage_b_wrote"] = s_resolve_stage_b_wrote;
		s_last_stats = info.duplicate();
	}
	stats_info.clear();
}

bool NextGeoCut::dispatch(RendererVirtualMeshStorage *p_storage, RID p_depth, const Projection &p_view_projection, const Projection &p_prev_view_projection, const Vector3 &p_camera, const Size2i &p_internal_size, uint32_t p_cull_mask, float p_max_pixel_error, float p_lod_bias, bool p_occlusion, bool p_debug_stats, const NextGeoRasterInputs *p_raster) {
	(void)p_storage;
	if (!_ensure()) {
		return false;
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

	// Publish the previous frame's cut: the read is taken before this frame's
	// reset, so the counters still hold the frame the CPU reference modeled.
	if (stats_pending_read) {
		const Vector<uint8_t> raw = rd->buffer_get_data(counters, 0, sizeof(CutCounters));
		PackedByteArray data;
		data.resize(raw.size());
		if (!raw.is_empty()) {
			memcpy(data.ptrw(), raw.ptr(), raw.size());
		}
		_publish_stats(data);
	}
	CutCounters zero;
	memset(&zero, 0, sizeof(CutCounters));
	rd->buffer_update(counters, 0, sizeof(CutCounters), &zero);
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

	const bool build_hiz = p_occlusion && p_depth.is_valid();
	if (build_hiz) {
		_ensure_hiz(p_internal_size);
	}
	const bool have_prev = hiz_texture[0].is_valid() && hiz_primed;

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
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, dummy_page_buf));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, counters));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, nodes));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ dummy_sampler, have_prev ? hiz_texture[0] : dummy_hiz })));
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
		_set_common_uniforms(uniforms, input, output, groups, clusters, args_out);
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
		_set_common_uniforms(uniforms, groups, level_a, dummy_page_buf, clusters, bvh_args[0]);
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
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, dummy_page_buf));
		inst_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, counters));
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
			_set_common_uniforms(uniforms, phase2_input, output, groups, clusters, args_out);
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
		_set_common_uniforms(leaf_uniforms, groups, level_a, dummy_page_buf, clusters, bvh_args[0]);
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
	{
		LocalVector<RD::Uniform> uniforms;
		_set_common_uniforms(uniforms, level_b, level_a, groups, clusters, bvh_args[0]);
		RID set = _uniform_set(uniforms, 2);
		if (set.is_valid()) {
			list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, cluster_hash);
			rd->compute_list_bind_uniform_set(list, set, 0);
			rd->compute_list_dispatch(list, (PHASE2_CLUSTER_BASE + 63) / 64, 1, 1);
			rd->compute_list_end();
		}
	}

	const uint32_t stats_interval = MAX(uint32_t(GLOBAL_GET("rendering/nextgeo/debug_stats_interval")), 1u);
	if (p_debug_stats && (stats_counter++ % stats_interval == 0)) {
		_request_stats(record_count, max_levels, &cut, p_occlusion, have_prev, raster_draws);
	}

	if (build_hiz && hiz_texture[1].is_valid()) {
		_swap_hiz();
		hiz_primed = true;
	}
	return raster_draws > 0;
}

void NextGeoCut::free_resources() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	_release_hiz();
	for (int variant = 0; variant < 4; variant++) {
		raster_pipelines[variant].clear();
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
	cluster_bvh = RID();
	cluster_leaf = RID();
	cluster_hash = RID();
	hiz_pipeline = RID();
	clear_vis_pipeline = RID();
	for (RID *buffer : { &params, &raster_params, &nodes, &phase2_nodes, &groups, &clusters, &level_a, &level_b, &bvh_args[0], &bvh_args[1], &draw_args, &counters, &dummy_page_buf }) {
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
