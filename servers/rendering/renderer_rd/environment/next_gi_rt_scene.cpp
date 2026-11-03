/**************************************************************************/
/*  next_gi_rt_scene.cpp                                                  */
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

#include "next_gi_rt_scene.h"

#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"

namespace RendererRD {

int nextgi_rt_select_lod(const float *p_edge_lengths, uint32_t p_lod_count, float p_eps_rt) {
	int selected = -1;
	for (uint32_t i = 0; i < p_lod_count; i++) {
		if (p_edge_lengths[i] <= p_eps_rt) {
			selected = int(i);
		}
	}
	return selected;
}

bool nextgi_rt_decode_positions(uint64_t p_format, const Vector<uint8_t> &p_vertex_data, uint32_t p_vertex_count, const AABB &p_aabb, Vector<float> &r_positions) {
	r_positions.clear();
	ERR_FAIL_COND_V(!(p_format & RSE::ARRAY_FORMAT_VERTEX), false);
	ERR_FAIL_COND_V(p_format & RSE::ARRAY_FLAG_USE_2D_VERTICES, false);

	// Cross-check the position stride against the canonical offsets helper; positions sit at offset 0.
	// Without a RenderingServer (a doctest), derive the same stride directly.
	uint32_t vertex_element_size = 0;
	if (RenderingServer::get_singleton() != nullptr) {
		uint32_t offsets[RSE::ARRAY_MAX];
		uint32_t normal_element_size = 0;
		uint32_t attrib_element_size = 0;
		uint32_t skin_element_size = 0;
		// A dummy index length keeps the helper from rejecting indexed formats; only the vertex stride is read.
		RenderingServer::get_singleton()->mesh_surface_make_offsets_from_format(p_format, int(p_vertex_count), 3, offsets, vertex_element_size, normal_element_size, attrib_element_size, skin_element_size);
		ERR_FAIL_COND_V(offsets[RSE::ARRAY_VERTEX] != 0, false);
	} else {
		vertex_element_size = (p_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) ? sizeof(uint16_t) * 4 : sizeof(float) * 3;
	}
	ERR_FAIL_COND_V(uint64_t(vertex_element_size) * p_vertex_count > uint64_t(p_vertex_data.size()), false);

	r_positions.resize(int64_t(p_vertex_count) * 3);
	float *out = r_positions.ptrw();
	const uint8_t *src = p_vertex_data.ptr();
	if (p_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) {
		ERR_FAIL_COND_V(vertex_element_size != sizeof(uint16_t) * 4, false);
		for (uint32_t i = 0; i < p_vertex_count; i++) {
			const uint16_t *packed = reinterpret_cast<const uint16_t *>(src + uint64_t(i) * vertex_element_size);
			out[i * 3 + 0] = float(packed[0]) / 65535.0f * p_aabb.size.x + p_aabb.position.x;
			out[i * 3 + 1] = float(packed[1]) / 65535.0f * p_aabb.size.y + p_aabb.position.y;
			out[i * 3 + 2] = float(packed[2]) / 65535.0f * p_aabb.size.z + p_aabb.position.z;
		}
	} else {
		ERR_FAIL_COND_V(vertex_element_size != sizeof(float) * 3, false);
		memcpy(out, src, uint64_t(p_vertex_count) * sizeof(float) * 3);
	}
	return true;
}

void nextgi_rt_widen_indices(const Vector<uint8_t> &p_index_data, uint32_t p_index_count, bool p_is_16_bit, uint32_t p_vertex_count, Vector<uint32_t> &r_indices) {
	r_indices.clear();
	if (p_index_count == 0) {
		r_indices.resize(p_vertex_count);
		for (uint32_t i = 0; i < p_vertex_count; i++) {
			r_indices.write[i] = i;
		}
		return;
	}
	r_indices.resize(p_index_count);
	uint32_t *out = r_indices.ptrw();
	if (p_is_16_bit) {
		const uint16_t *src = reinterpret_cast<const uint16_t *>(p_index_data.ptr());
		for (uint32_t i = 0; i < p_index_count; i++) {
			out[i] = src[i];
		}
	} else {
		memcpy(out, p_index_data.ptr(), uint64_t(p_index_count) * sizeof(uint32_t));
	}
}

NextGIArenaAllocator::Allocation NextGIArenaAllocator::allocate(uint32_t p_size) {
	Allocation allocation;
	ERR_FAIL_COND_V(p_size == 0, allocation);
	const uint32_t aligned_size = (p_size + NEXTGI_RT_ALIGNMENT - 1) & ~uint32_t(NEXTGI_RT_ALIGNMENT - 1);
	ERR_FAIL_COND_V_MSG(aligned_size > chunk_size, allocation, "NextGI RT arena allocation is larger than one chunk.");

	for (uint32_t i = 0; i < free_ranges.size(); i++) {
		if (free_ranges[i].size >= aligned_size) {
			allocation.chunk = free_ranges[i].chunk;
			allocation.offset = free_ranges[i].offset;
			allocation.size = aligned_size;
			free_ranges[i].offset += aligned_size;
			free_ranges[i].size -= aligned_size;
			if (free_ranges[i].size == 0) {
				free_ranges.remove_at(i);
			}
			return allocation;
		}
	}

	allocation.chunk = chunk_count++;
	allocation.offset = 0;
	allocation.size = aligned_size;
	allocation.new_chunk = true;
	if (aligned_size < chunk_size) {
		FreeRange rest;
		rest.chunk = allocation.chunk;
		rest.offset = aligned_size;
		rest.size = chunk_size - aligned_size;
		free_ranges.push_back(rest);
	}
	return allocation;
}

void NextGIArenaAllocator::free_range(const Allocation &p_allocation) {
	if (!p_allocation.is_valid() || p_allocation.size == 0) {
		return;
	}
	FreeRange freed;
	freed.chunk = p_allocation.chunk;
	freed.offset = p_allocation.offset;
	freed.size = p_allocation.size;

	// Coalesce with directly adjacent ranges of the same chunk.
	for (uint32_t i = 0; i < free_ranges.size();) {
		const FreeRange &range = free_ranges[i];
		if (range.chunk == freed.chunk && range.offset + range.size == freed.offset) {
			freed.offset = range.offset;
			freed.size += range.size;
			free_ranges.remove_at(i);
		} else if (range.chunk == freed.chunk && freed.offset + freed.size == range.offset) {
			freed.size += range.size;
			free_ranges.remove_at(i);
		} else {
			i++;
		}
	}
	free_ranges.push_back(freed);
}

void NextGIArenaAllocator::reset() {
	chunk_count = 0;
	free_ranges.clear();
}

void nextgi_rt_split_parts(const uint32_t *p_triangle_counts, const uint32_t *p_chunks, uint32_t p_surface_count, uint32_t p_max_triangles_per_part, LocalVector<NextGIRTPartPlan> &r_parts) {
	r_parts.clear();
	ERR_FAIL_COND(p_max_triangles_per_part == 0);

	NextGIRTPartPlan current;
	auto flush = [&]() {
		if (!current.geometries.is_empty()) {
			r_parts.push_back(current);
			current = NextGIRTPartPlan();
		}
	};

	for (uint32_t s = 0; s < p_surface_count; s++) {
		uint32_t remaining = p_triangle_counts[s];
		if (remaining == 0) {
			continue;
		}
		uint32_t first_triangle = 0;
		while (remaining > 0) {
			if (!current.geometries.is_empty() && (current.chunk != p_chunks[s] || current.triangle_count >= p_max_triangles_per_part)) {
				flush();
			}
			// Whole surfaces first: a surface that fits a fresh part whole is never split to fill one.
			if (!current.geometries.is_empty() && first_triangle == 0 && remaining <= p_max_triangles_per_part && current.triangle_count + remaining > p_max_triangles_per_part) {
				flush();
			}
			if (current.geometries.is_empty()) {
				current.chunk = p_chunks[s];
				current.first_surface = s;
				current.surface_count = 0;
			}
			const uint32_t take = MIN(remaining, p_max_triangles_per_part - current.triangle_count);
			NextGIRTPartGeometry geometry;
			geometry.surface = s;
			geometry.first_triangle = first_triangle;
			geometry.triangle_count = take;
			current.geometries.push_back(geometry);
			current.triangle_count += take;
			current.surface_count = s - current.first_surface + 1;
			first_triangle += take;
			remaining -= take;
			if (current.triangle_count >= p_max_triangles_per_part) {
				flush();
			}
		}
	}
	flush();
}

// D17: alpha surfaces stay out of the RT scene in v0. The check uses the mesh surface's own
// material, matching FLAG_PASS_ALPHA / uses_alpha_clip of the forward shader data.
static bool _nextgi_rt_material_is_alpha(RID p_material) {
	if (p_material.is_null()) {
		return false;
	}
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	if (material_storage == nullptr || !material_storage->owns_material(p_material)) {
		return false;
	}
	RendererSceneRenderImplementation::SceneShaderForwardClustered::MaterialData *material_data = static_cast<RendererSceneRenderImplementation::SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(p_material, MaterialStorage::SHADER_TYPE_3D));
	if (material_data == nullptr || material_data->shader_data == nullptr) {
		return false;
	}
	return material_data->shader_data->uses_alpha_pass() || material_data->shader_data->uses_alpha_clip;
}

void NextGIRTScene::init() {
	ERR_FAIL_COND(tlas.is_valid());
	ERR_FAIL_NULL(RD::get_singleton());
	ERR_FAIL_COND(!RD::get_singleton()->has_feature(RD::SUPPORTS_RAY_QUERY));
	tlas = RD::get_singleton()->tlas_create(NEXTGI_RT_TLAS_CAPACITY, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
}

void NextGIRTScene::free() {
	if (RD::get_singleton() == nullptr) {
		return;
	}
	if (tlas.is_valid()) {
		RD::get_singleton()->free_rid(tlas);
		tlas = RID();
	}
	if (instance_record_buffer.is_valid()) {
		RD::get_singleton()->free_rid(instance_record_buffer);
		instance_record_buffer = RID();
	}
	if (surface_record_buffer.is_valid()) {
		RD::get_singleton()->free_rid(surface_record_buffer);
		surface_record_buffer = RID();
	}
	instance_record_capacity = 0;
	surface_record_capacity = 0;
}

void NextGIRTScene::_ensure_record_buffers(uint32_t p_instance_count, uint32_t p_surface_count) {
	if (p_instance_count > instance_record_capacity) {
		if (instance_record_buffer.is_valid()) {
			RD::get_singleton()->free_rid(instance_record_buffer);
		}
		instance_record_capacity = MAX(p_instance_count, instance_record_capacity * 2);
		instance_record_buffer = RD::get_singleton()->storage_buffer_create(instance_record_capacity * sizeof(NextGIInstanceRecord));
	}
	if (p_surface_count > surface_record_capacity) {
		if (surface_record_buffer.is_valid()) {
			RD::get_singleton()->free_rid(surface_record_buffer);
		}
		surface_record_capacity = MAX(p_surface_count, surface_record_capacity * 2);
		surface_record_buffer = RD::get_singleton()->storage_buffer_create(surface_record_capacity * sizeof(NextGISurfaceRecord));
	}
}

void NextGIRTScene::_build_dirty_mesh_parts(RID p_mesh) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	mesh_storage->mesh_nextgi_rt_clear_parts(p_mesh);

	const uint32_t surface_count = uint32_t(mesh_storage->mesh_get_surface_count(p_mesh));
	LocalVector<uint32_t> triangle_counts;
	LocalVector<uint32_t> chunks;
	LocalVector<MeshStorage::NextGIRTSurfaceInfo> surface_infos;
	triangle_counts.resize(surface_count);
	chunks.resize(surface_count);
	surface_infos.resize(surface_count);
	for (uint32_t i = 0; i < surface_count; i++) {
		triangle_counts[i] = 0;
		chunks[i] = UINT32_MAX;
		if (!mesh_storage->mesh_nextgi_rt_get_surface_info(p_mesh, i, surface_infos[i]) || !surface_infos[i].eligible) {
			continue;
		}
		if (_nextgi_rt_material_is_alpha(surface_infos[i].material)) {
			continue;
		}
		triangle_counts[i] = surface_infos[i].index_count / 3;
		chunks[i] = surface_infos[i].chunk;
	}

	LocalVector<NextGIRTPartPlan> plans;
	nextgi_rt_split_parts(triangle_counts.ptr(), chunks.ptr(), surface_count, NEXTGI_RT_MAX_TRIANGLES_PER_BLAS, plans);

	LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
	ERR_FAIL_NULL(parts);
	for (const NextGIRTPartPlan &plan : plans) {
		LocalVector<RD::AccelerationStructureGeometry> geometries;
		const RID chunk_buffer = mesh_storage->mesh_nextgi_rt_get_chunk_buffer(plan.chunk);
		ERR_CONTINUE(chunk_buffer.is_null());
		for (const NextGIRTPartGeometry &part_geometry : plan.geometries) {
			const MeshStorage::NextGIRTSurfaceInfo &info = surface_infos[part_geometry.surface];
			RD::AccelerationStructureGeometry geometry;
			geometry.flags = RD::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT;
			geometry.vertex_buffer = chunk_buffer;
			geometry.vertex_offset = info.vertex_offset;
			geometry.vertex_stride = sizeof(float) * 3;
			geometry.vertex_count = info.vertex_count;
			geometry.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
			geometry.index_buffer = chunk_buffer;
			geometry.index_offset = info.index_offset + part_geometry.first_triangle * 3 * sizeof(uint32_t);
			geometry.index_count = part_geometry.triangle_count * 3;
			geometry.index_format = RD::INDEX_BUFFER_FORMAT_UINT32;
			geometries.push_back(geometry);
		}

		MeshStorage::NextGIRTPart part;
		part.first_surface = plan.first_surface;
		part.surface_count = plan.surface_count;
		part.geometries = plan.geometries;
		part.blas = RD::get_singleton()->blas_create(Span<RD::AccelerationStructureGeometry>(geometries.ptr(), geometries.size()), RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
		if (part.blas.is_valid()) {
			RD::get_singleton()->blas_build(part.blas);
			stats.blas_build_count++;
		}
		parts->push_back(part);
	}
	mesh_storage->mesh_nextgi_rt_set_blas_dirty(p_mesh, false);
}

void NextGIRTScene::update(const ::RenderDataRD *p_render_data) {
	stats.fed_instance_count = 0;
	stats.instance_count = 0;
	stats.blas_build_count = 0;
	ERR_FAIL_NULL(p_render_data);
	if (tlas.is_null()) {
		return;
	}
	const RendererSceneRender::RenderSDFGIUpdateData *update_data = p_render_data->sdfgi_update_data;
	if (update_data == nullptr || update_data->nextgi_rt_instances == nullptr || update_data->nextgi_rt_instances->is_empty()) {
		return;
	}
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	ERR_FAIL_NULL(mesh_storage);
	if (!mesh_storage->mesh_nextgi_rt_is_enabled()) {
		return;
	}
	stats.fed_instance_count = update_data->nextgi_rt_instances->size();

	LocalVector<RD::AccelerationStructureInstance> tlas_instances;
	LocalVector<NextGIInstanceRecord> instance_records;
	LocalVector<NextGISurfaceRecord> surface_records;

	RD::get_singleton()->draw_command_begin_label("NextGI: AS builds");
	for (const RendererSceneRender::NextGIRTInstance &fed : *update_data->nextgi_rt_instances) {
		RenderGeometryInstanceBase *geometry_instance = static_cast<RenderGeometryInstanceBase *>(fed.geometry_instance);
		if (geometry_instance == nullptr || geometry_instance->data == nullptr) {
			continue;
		}
		geometry_instance->nextgi_record_index = UINT32_MAX;
		const RID mesh = geometry_instance->data->base;
		if (!mesh_storage->owns_mesh(mesh)) {
			continue;
		}
		if (mesh_storage->mesh_nextgi_rt_is_blas_dirty(mesh)) {
			_build_dirty_mesh_parts(mesh);
		}
		const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(mesh);
		if (parts == nullptr) {
			continue;
		}

		const Transform3D &transform = geometry_instance->transform;
		const Vector3 scale = transform.basis.get_scale_abs();
		const float max_scale = MAX(scale.x, MAX(scale.y, scale.z));
		for (const MeshStorage::NextGIRTPart &part : *parts) {
			if (part.blas.is_null() || part.geometries.is_empty()) {
				continue;
			}
			if (instance_records.size() >= NEXTGI_RT_TLAS_CAPACITY || instance_records.size() > NEXTGI_RT_RECORD_INDEX_MAX) {
				break;
			}
			const uint32_t record_index = instance_records.size();

			NextGIInstanceRecord record;
			for (int i = 0; i < 3; i++) {
				record.object_to_world[i * 4 + 0] = transform.basis.rows[i][0];
				record.object_to_world[i * 4 + 1] = transform.basis.rows[i][1];
				record.object_to_world[i * 4 + 2] = transform.basis.rows[i][2];
			}
			record.object_to_world[3] = transform.origin.x;
			record.object_to_world[7] = transform.origin.y;
			record.object_to_world[11] = transform.origin.z;
			record.surface_base = surface_records.size();
			record.surface_count = part.geometries.size();
			record.rt_flags = NEXTGI_RT_FLAG_STATIC | (fed.lightmapped ? uint32_t(NEXTGI_RT_FLAG_LIGHTMAPPED) : 0);
			record.tlas_index = record_index;

			float eps_rt = 0.0f;
			for (const NextGIRTPartGeometry &part_geometry : part.geometries) {
				MeshStorage::NextGIRTSurfaceInfo info;
				if (!mesh_storage->mesh_nextgi_rt_get_surface_info(mesh, part_geometry.surface, info)) {
					continue;
				}
				record.arena_chunk = info.chunk;
				eps_rt = MAX(eps_rt, info.eps);
				NextGISurfaceRecord surface_record;
				surface_record.vertex_offset = info.vertex_offset;
				surface_record.index_offset = info.index_offset + part_geometry.first_triangle * 3 * sizeof(uint32_t);
				surface_record.triangle_count = part_geometry.triangle_count;
				surface_record.flags = 0;
				surface_records.push_back(surface_record);
			}
			record.origin_offset = nextgi_rt_origin_offset(eps_rt, max_scale);
			instance_records.push_back(record);

			RD::AccelerationStructureInstance tlas_instance;
			tlas_instance.transform = transform;
			tlas_instance.id = record_index;
			tlas_instance.mask = 0xFF;
			tlas_instance.hit_sbt_range = 0;
			tlas_instance.flags = RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
			tlas_instance.blas = part.blas;
			tlas_instances.push_back(tlas_instance);

			if (geometry_instance->nextgi_record_index == UINT32_MAX) {
				geometry_instance->nextgi_record_index = record_index;
			}
		}
	}
	RD::get_singleton()->draw_command_end_label();

	stats.instance_count = instance_records.size();
	if (instance_records.is_empty()) {
		return;
	}

	_ensure_record_buffers(instance_records.size(), surface_records.size());
	if (instance_record_buffer.is_valid()) {
		RD::get_singleton()->buffer_update(instance_record_buffer, 0, instance_records.size() * sizeof(NextGIInstanceRecord), instance_records.ptr());
	}
	if (surface_record_buffer.is_valid() && !surface_records.is_empty()) {
		RD::get_singleton()->buffer_update(surface_record_buffer, 0, surface_records.size() * sizeof(NextGISurfaceRecord), surface_records.ptr());
	}

	RD::get_singleton()->draw_command_begin_label("NextGI: AS updates");
	if (RD::get_singleton()->tlas_build(tlas, Span<RD::AccelerationStructureInstance>(tlas_instances.ptr(), tlas_instances.size())) == OK) {
		stats.tlas_build_count++;
	}
	RD::get_singleton()->draw_command_end_label();
}

} // namespace RendererRD
