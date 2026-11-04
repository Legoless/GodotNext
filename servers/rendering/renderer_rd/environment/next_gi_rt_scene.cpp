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

#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_server_globals.h"

#include <algorithm>

// GodotNext: nextgi_src_cards
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_rt_instances.glsl.gen.h"
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

float nextgi_rt_angular_radius(float p_distance, float p_bounding_radius) {
	if (p_bounding_radius <= 0.0f) {
		return 0.0f;
	}
	if (p_distance <= 0.0f) {
		return Math::PI * 0.5f;
	}
	return Math::asin(MIN(1.0f, p_bounding_radius / p_distance));
}

void nextgi_rt_note_multimesh_change(NextGIMultiMeshChange &r_state, uint64_t p_frame) {
	if (r_state.last_change_frame == p_frame) {
		return;
	}
	if (p_frame == r_state.last_change_frame + 1 && r_state.change_streak > 0) {
		r_state.change_streak = MIN(uint8_t(r_state.change_streak + 1), uint8_t(255));
	} else {
		r_state.change_streak = 1;
	}
	r_state.last_change_frame = p_frame;
}

Transform3D nextgi_rt_element_transform(const float *p_rows_3x4) {
	Transform3D transform;
	if (p_rows_3x4 == nullptr) {
		return transform;
	}
	transform.basis.rows[0] = Vector3(p_rows_3x4[0], p_rows_3x4[1], p_rows_3x4[2]);
	transform.basis.rows[1] = Vector3(p_rows_3x4[4], p_rows_3x4[5], p_rows_3x4[6]);
	transform.basis.rows[2] = Vector3(p_rows_3x4[8], p_rows_3x4[9], p_rows_3x4[10]);
	transform.origin = Vector3(p_rows_3x4[3], p_rows_3x4[7], p_rows_3x4[11]);
	return transform;
}

Transform3D nextgi_rt_element_world(const Transform3D &p_instance, const float *p_rows_3x4) {
	return p_instance * nextgi_rt_element_transform(p_rows_3x4);
}

int64_t nextgi_rt_coverage_percent(const NextGIRTCoverage &p_coverage) {
	if (p_coverage.denominator == 0) {
		return 0;
	}
	return int64_t(p_coverage.numerator) * 100 / int64_t(p_coverage.denominator);
}

NextGIRTExclusion nextgi_rt_cull_instance(float p_distance, float p_bounding_radius, float p_scene_radius) {
	if (p_distance > p_scene_radius) {
		return NEXTGI_RT_NOT_IN_SCENE;
	}
	if (p_distance > NEXTGI_RT_ANGULAR_CULL_DISTANCE && nextgi_rt_angular_radius(p_distance, p_bounding_radius) < Math::deg_to_rad(0.5f)) {
		return NEXTGI_RT_NOT_IN_SCENE;
	}
	return NEXTGI_RT_INCLUDED;
}

void nextgi_rt_apply_instance_cap(float *p_angular, NextGIRTExclusion *p_reasons, uint32_t p_count, uint32_t p_cap) {
	LocalVector<uint32_t> included;
	for (uint32_t i = 0; i < p_count; i++) {
		if (p_reasons[i] == NEXTGI_RT_INCLUDED) {
			included.push_back(i);
		}
	}
	if (included.size() <= p_cap) {
		return;
	}
	struct Greater {
		const float *angular = nullptr;
		bool operator()(uint32_t p_a, uint32_t p_b) const { return angular[p_a] > angular[p_b]; }
	} greater;
	greater.angular = p_angular;
	std::nth_element(included.ptr(), included.ptr() + p_cap, included.ptr() + included.size(), greater);
	for (uint32_t i = p_cap; i < included.size(); i++) {
		p_reasons[included[i]] = NEXTGI_RT_INSTANCE_CAP;
	}
}

NextGIDirtyRegion nextgi_rt_dirty_region(NextGIDirtyKind p_kind) {
	NextGIDirtyRegion region;
	region.has_old = true;
	region.has_new = p_kind == NEXTGI_DIRTY_MOVED;
	return region;
}

bool nextgi_rt_surface_is_dynamic(uint32_t p_frame, const uint32_t p_update_frames[4]) {
	for (uint32_t i = 0; i < 4; i++) {
		if (p_update_frames[i] != p_frame - i) {
			return false;
		}
	}
	return true;
}

NextGIRTExclusion nextgi_rt_refit_or_exclude(uint32_t p_refit_triangles_so_far, uint32_t p_triangles, uint32_t p_refit_cap) {
	if (p_refit_triangles_so_far + p_triangles > p_refit_cap) {
		return NEXTGI_RT_DYNAMIC_EXCLUDED;
	}
	return NEXTGI_RT_INCLUDED;
}

uint32_t nextgi_rt_schedule_builds(const NextGIBuildItem *p_items, uint32_t p_count, uint32_t p_triangle_cap, uint32_t *r_accepted, uint32_t p_accepted_capacity, uint32_t &r_triangles) {
	LocalVector<uint32_t> order;
	order.resize(p_count);
	for (uint32_t i = 0; i < p_count; i++) {
		order[i] = i;
	}
	struct Less {
		const NextGIBuildItem *items = nullptr;
		bool operator()(uint32_t p_a, uint32_t p_b) const {
			const NextGIBuildItem &a = items[p_a];
			const NextGIBuildItem &b = items[p_b];
			if (a.is_new != b.is_new) {
				return a.is_new;
			}
			return a.angular_radius > b.angular_radius;
		}
	} less;
	less.items = p_items;
	if (p_count > 1) {
		std::sort(order.ptr(), order.ptr() + p_count, less);
	}
	uint32_t accepted = 0;
	r_triangles = 0;
	for (uint32_t i = 0; i < p_count; i++) {
		const NextGIBuildItem &item = p_items[order[i]];
		if (r_triangles + item.triangles > p_triangle_cap) {
			break;
		}
		if (accepted < p_accepted_capacity && r_accepted != nullptr) {
			r_accepted[accepted] = item.id;
		}
		accepted++;
		r_triangles += item.triangles;
	}
	return accepted;
}

bool nextgi_rt_evict_as_memory(const NextGIASMesh *p_meshes, uint32_t p_count, uint64_t p_budget, uint32_t p_absent_frames, uint32_t *r_evict, uint32_t &r_evict_count) {
	r_evict_count = 0;
	uint64_t total = 0;
	for (uint32_t i = 0; i < p_count; i++) {
		total += p_meshes[i].bytes;
	}
	LocalVector<uint32_t> absent;
	for (uint32_t i = 0; i < p_count; i++) {
		if (p_meshes[i].frames_absent >= p_absent_frames) {
			absent.push_back(i);
		}
	}
	struct Older {
		const NextGIASMesh *meshes = nullptr;
		bool operator()(uint32_t p_a, uint32_t p_b) const { return meshes[p_a].frames_absent > meshes[p_b].frames_absent; }
	} older;
	older.meshes = p_meshes;
	if (absent.size() > 1) {
		std::sort(absent.ptr(), absent.ptr() + absent.size(), older);
	}
	for (uint32_t index : absent) {
		if (total <= p_budget) {
			break;
		}
		r_evict[r_evict_count++] = p_meshes[index].id;
		total -= p_meshes[index].bytes;
	}
	return total > p_budget;
}

void nextgi_rt_patch_positions(Vector<float> &r_positions, uint32_t p_byte_offset, const uint8_t *p_data, uint32_t p_size) {
	const uint32_t pos_bytes = uint32_t(r_positions.size()) * sizeof(float);
	if (p_data == nullptr || p_size == 0 || p_byte_offset >= pos_bytes) {
		return;
	}
	const uint32_t count = MIN(p_size, pos_bytes - p_byte_offset);
	memcpy(reinterpret_cast<uint8_t *>(r_positions.ptrw()) + p_byte_offset, p_data, count);
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
	if (mesh_storage == nullptr || !mesh_storage->owns_mesh(p_mesh)) {
		return;
	}
	const bool dynamic = mesh_storage->mesh_nextgi_rt_is_dynamic(p_mesh);
	const LocalVector<MeshStorage::NextGIRTPart> *old_parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
	if (old_parts != nullptr) {
		for (const MeshStorage::NextGIRTPart &old_part : *old_parts) {
			for (uint32_t i = 0; i < pending_compactions.size();) {
				if (pending_compactions[i].blas == old_part.blas) {
					pending_compactions.remove_at(i);
				} else {
					i++;
				}
			}
		}
	}
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
		part.triangle_count = plan.triangle_count;
		// Static BLASes compact one frame later. A dynamic mesh is rebuilt once with ALLOW_UPDATE, then refit.
		BitField<RD::AccelerationStructureFlagBits> flags = RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT;
		if (dynamic) {
			flags = flags | RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT;
		} else {
			flags = flags | RD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT;
		}
		part.blas = RD::get_singleton()->blas_create(Span<RD::AccelerationStructureGeometry>(geometries.ptr(), geometries.size()), flags);
		if (part.blas.is_valid()) {
			RD::get_singleton()->blas_build(part.blas);
			stats.blas_build_count++;
			stats.blas_build_triangles += plan.triangle_count;
			if (!dynamic) {
				PendingCompact pending;
				pending.blas = part.blas;
				pending.triangles = plan.triangle_count;
				pending_compactions.push_back(pending);
			}
		}
		parts->push_back(part);
	}
	mesh_storage->mesh_nextgi_rt_set_allows_update(p_mesh, dynamic);
	mesh_storage->mesh_nextgi_rt_set_blas_dirty(p_mesh, false);
}

// GodotNext: nextgi_as_slots
// Both AS phases gate on the same v0 feed; either phase is a no-op without it.
static const LocalVector<RendererSceneRender::NextGIRTInstance> *_nextgi_rt_fed_instances(const ::RenderDataRD *p_render_data) {
	const RendererSceneRender::RenderSDFGIUpdateData *update_data = p_render_data->sdfgi_update_data;
	if (update_data == nullptr) {
		return nullptr;
	}
	return update_data->nextgi_rt_instances;
}

void NextGIRTScene::rebuild_tlas(const ::RenderDataRD *p_render_data) {
	// This phase runs first in the frame block, so it also clears the per-frame stats;
	// build_dirty_blas() then fills blas_build_count for the same frame.
	stats.fed_instance_count = 0;
	stats.instance_count = 0;
	stats.blas_build_count = 0;
	stats.blas_build_triangles = 0;
	stats.compaction_copies = 0;
	stats.refit_triangles = 0;
	tlas_traceable = false;
	compaction_triangles_this_frame = 0;
	ERR_FAIL_NULL(p_render_data);
	if (tlas.is_null()) {
		return;
	}
	const LocalVector<RendererSceneRender::NextGIRTInstance> *fed_instances = _nextgi_rt_fed_instances(p_render_data);
	if (fed_instances == nullptr || fed_instances->is_empty()) {
		return;
	}
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	ERR_FAIL_NULL(mesh_storage);
	if (!mesh_storage->mesh_nextgi_rt_is_enabled()) {
		return;
	}
	stats.fed_instance_count = fed_instances->size();
	const RendererSceneRender::RenderSDFGIUpdateData *update_data = p_render_data->sdfgi_update_data;
	stats.coverage.not_in_scene = update_data != nullptr ? update_data->nextgi_rt_not_in_scene : 0;
	stats.coverage.instance_cap = update_data != nullptr ? update_data->nextgi_rt_instance_cap : 0;
	stats.coverage.dynamic_excluded = update_data != nullptr ? update_data->nextgi_rt_dynamic_excluded : 0;
	stats.coverage.denominator = update_data != nullptr ? update_data->nextgi_rt_denominator : 0;
	stats.coverage.numerator = 0;
	stats.coverage.blas_pending = 0;
	const uint64_t ticks_start = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;

	LocalVector<RD::AccelerationStructureInstance> tlas_instances;
	LocalVector<NextGIInstanceRecord> instance_records;
	LocalVector<NextGISurfaceRecord> surface_records;

	for (const RendererSceneRender::NextGIRTInstance &fed : *fed_instances) {
		RenderGeometryInstanceBase *geometry_instance = static_cast<RenderGeometryInstanceBase *>(fed.geometry_instance);
		if (geometry_instance == nullptr || geometry_instance->data == nullptr) {
			continue;
		}
		geometry_instance->nextgi_record_index = UINT32_MAX;
		const RID mesh = fed.mesh_override.is_valid() ? fed.mesh_override : geometry_instance->data->base;
		if (!mesh_storage->owns_mesh(mesh)) {
			stats.coverage.blas_pending++;
			continue;
		}
		// A dirty mesh is built in this frame's later "AS builds" phase and enters the
		// TLAS next frame; its parts are skipped here (nextgi_rt_part_enters_tlas).
		bool blas_dirty = mesh_storage->mesh_nextgi_rt_is_blas_dirty(mesh);
		const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(mesh);
		if (parts == nullptr) {
			stats.coverage.blas_pending++;
			continue;
		}
		// A surface updated on each of the last 4 frames refits in this phase, up to the refit cap.
		if (blas_dirty && mesh_storage->mesh_nextgi_rt_is_dynamic(mesh) && mesh_storage->mesh_nextgi_rt_allows_update(mesh)) {
			uint32_t triangles = 0;
			for (const MeshStorage::NextGIRTPart &part : *parts) {
				triangles += part.triangle_count;
			}
			if (nextgi_rt_refit_or_exclude(stats.refit_triangles, triangles, NEXTGI_RT_REFIT_TRIANGLE_CAP) != NEXTGI_RT_INCLUDED) {
				continue;
			}
			for (const MeshStorage::NextGIRTPart &part : *parts) {
				if (part.blas.is_valid()) {
					RD::get_singleton()->blas_build(part.blas, RD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE);
				}
			}
			mesh_storage->mesh_nextgi_rt_set_blas_dirty(mesh, false);
			stats.refit_triangles += triangles;
			blas_dirty = false;
		}

		const Transform3D transform = fed.has_rt_transform ? fed.rt_transform : geometry_instance->transform;
		bool entered_tlas = false;
		const Vector3 scale = transform.basis.get_scale_abs();
		const float max_scale = MAX(scale.x, MAX(scale.y, scale.z));
		for (const MeshStorage::NextGIRTPart &part : *parts) {
			if (!nextgi_rt_part_enters_tlas(blas_dirty, part.blas.is_valid()) || part.geometries.is_empty()) {
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
			// GodotNext: nextgi_src_cards
			// Every part record of the mesh shares its SRC card set (§6.3); UINT32_MAX until built.
			record.card_set = NextGISRC::get_instance_card_set(geometry_instance);
			if (record.card_set == UINT32_MAX) {
				record.card_set = NextGISRC::get_mesh_card_set(mesh);
			}
			if (NextGIRelight::get_singleton() != nullptr) {
				record.page_table = NextGIRelight::get_singleton()->assign_pages(geometry_instance, record, fed.element_index);
			}

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
			if (RSG::rasterizer != nullptr) {
				mesh_last_tlas_frame[mesh] = RSG::rasterizer->get_frame_number();
			}

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
			entered_tlas = true;
		}
		if (entered_tlas) {
			stats.coverage.numerator++;
		} else {
			stats.coverage.blas_pending++;
		}
	}

	stats.instance_count = instance_records.size();
	if (OS::get_singleton() != nullptr) {
		stats.tlas_fill_ms = double(OS::get_singleton()->get_ticks_usec() - ticks_start) / 1000.0;
	}
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

	// No debug label here: the frame block's slot label names this work.
	if (RD::get_singleton()->tlas_build(tlas, Span<RD::AccelerationStructureInstance>(tlas_instances.ptr(), tlas_instances.size())) == OK) {
		stats.tlas_build_count++;
		tlas_traceable = true;
	}
}

// GodotNext: nextgi_as_slots
void NextGIRTScene::build_dirty_blas(const ::RenderDataRD *p_render_data) {
	ERR_FAIL_NULL(p_render_data);
	if (tlas.is_null()) {
		return;
	}
	const LocalVector<RendererSceneRender::NextGIRTInstance> *fed_instances = _nextgi_rt_fed_instances(p_render_data);
	if (fed_instances == nullptr || fed_instances->is_empty()) {
		return;
	}
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	ERR_FAIL_NULL(mesh_storage);
	if (!mesh_storage->mesh_nextgi_rt_is_enabled()) {
		return;
	}
	const uint64_t ticks_start = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	const uint32_t spent_triangles = compaction_triangles_this_frame;

	// New BLASes first, then the largest angular radius. A mesh that does not fit stays dirty.
	HashMap<RID, NextGIBuildItem> unique;
	LocalVector<RID> meshes;
	uint32_t next_id = 0;
	for (const RendererSceneRender::NextGIRTInstance &fed : *fed_instances) {
		RenderGeometryInstanceBase *geometry_instance = static_cast<RenderGeometryInstanceBase *>(fed.geometry_instance);
		if (geometry_instance == nullptr || geometry_instance->data == nullptr) {
			continue;
		}
		const RID mesh = fed.mesh_override.is_valid() ? fed.mesh_override : geometry_instance->data->base;
		if (!mesh_storage->owns_mesh(mesh) || !mesh_storage->mesh_nextgi_rt_is_blas_dirty(mesh)) {
			continue;
		}
		if (mesh_storage->mesh_nextgi_rt_is_dynamic(mesh) && mesh_storage->mesh_nextgi_rt_allows_update(mesh)) {
			// Line 1a already refit this mesh, or excluded it past the refit cap.
			continue;
		}
		if (unique.has(mesh)) {
			NextGIBuildItem &item = unique[mesh];
			item.angular_radius = MAX(item.angular_radius, fed.angular_radius);
			continue;
		}
		NextGIBuildItem item;
		item.id = next_id++;
		item.triangles = _mesh_triangle_count(mesh);
		item.angular_radius = fed.angular_radius;
		const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(mesh);
		item.is_new = parts == nullptr || parts->is_empty();
		if (!item.is_new) {
			item.is_new = true;
			for (const MeshStorage::NextGIRTPart &part : *parts) {
				if (part.blas.is_valid()) {
					item.is_new = false;
					break;
				}
			}
		}
		unique.insert(mesh, item);
		meshes.push_back(mesh);
	}

	LocalVector<NextGIBuildItem> items;
	items.resize(meshes.size());
	for (uint32_t i = 0; i < meshes.size(); i++) {
		items[unique[meshes[i]].id] = unique[meshes[i]];
	}
	LocalVector<uint32_t> accepted_ids;
	accepted_ids.resize(items.size());
	uint32_t scheduled_triangles = 0;
	const uint32_t triangle_budget = spent_triangles >= uint32_t(NEXTGI_RT_BUILD_TRIANGLE_CAP) ? 0 : uint32_t(NEXTGI_RT_BUILD_TRIANGLE_CAP) - spent_triangles;
	const uint32_t accepted_count = nextgi_rt_schedule_builds(items.ptr(), items.size(), triangle_budget, accepted_ids.ptr(), accepted_ids.size(), scheduled_triangles);
	HashMap<uint32_t, bool> accepted;
	for (uint32_t i = 0; i < accepted_count; i++) {
		accepted.insert(accepted_ids[i], true);
	}
	for (const RID &mesh : meshes) {
		if (!accepted.has(unique[mesh].id)) {
			continue;
		}
		_build_dirty_mesh_parts(mesh);
		// Rebuilding a BLAS that this frame's TLAS already references invalidates that TLAS.
		tlas_traceable = false;
	}

	uint64_t frame = 0;
	if (RSG::rasterizer != nullptr) {
		frame = RSG::rasterizer->get_frame_number();
	}
	_apply_as_budget(frame);
	if (OS::get_singleton() != nullptr) {
		stats.scene_update_ms = double(OS::get_singleton()->get_ticks_usec() - ticks_start) / 1000.0;
	}
}

void NextGIRTScene::compact_after_traces() {
	if (tlas.is_null() || RD::get_singleton() == nullptr || pending_compactions.is_empty()) {
		return;
	}
	LocalVector<PendingCompact> deferred;
	for (const PendingCompact &pending : pending_compactions) {
		if (compaction_triangles_this_frame + pending.triangles > uint32_t(NEXTGI_RT_BUILD_TRIANGLE_CAP)) {
			deferred.push_back(pending);
			continue;
		}
		if (RD::get_singleton()->blas_compact(pending.blas) == OK) {
			stats.compaction_copies++;
			compaction_triangles_this_frame += pending.triangles;
			// The copy completes on a later device frame and invalidates this TLAS.
			// Traces in this frame have already been recorded.
			tlas_traceable = false;
		}
	}
	pending_compactions = deferred;
}

uint32_t NextGIRTScene::_mesh_triangle_count(RID p_mesh) const {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr || !mesh_storage->owns_mesh(p_mesh)) {
		return 0;
	}
	const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
	if (parts != nullptr && !parts->is_empty()) {
		uint32_t triangles = 0;
		for (const MeshStorage::NextGIRTPart &part : *parts) {
			triangles += part.triangle_count;
		}
		if (triangles > 0) {
			return triangles;
		}
	}
	uint32_t triangles = 0;
	const uint32_t surface_count = uint32_t(mesh_storage->mesh_get_surface_count(p_mesh));
	for (uint32_t i = 0; i < surface_count; i++) {
		MeshStorage::NextGIRTSurfaceInfo info;
		if (!mesh_storage->mesh_nextgi_rt_get_surface_info(p_mesh, i, info) || !info.eligible || _nextgi_rt_material_is_alpha(info.material)) {
			continue;
		}
		triangles += info.index_count / 3;
	}
	return triangles;
}

void NextGIRTScene::_apply_as_budget(uint64_t p_frame) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr || RD::get_singleton() == nullptr) {
		return;
	}
	LocalVector<RID> ids;
	LocalVector<NextGIASMesh> meshes;
	LocalVector<RID> stale_meshes;
	for (const KeyValue<RID, uint64_t> &entry : mesh_last_tlas_frame) {
		if (!mesh_storage->owns_mesh(entry.key)) {
			stale_meshes.push_back(entry.key);
			continue;
		}
		NextGIASMesh mesh;
		mesh.id = ids.size();
		ids.push_back(entry.key);
		uint64_t bytes = 0;
		const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(entry.key);
		if (parts != nullptr) {
			for (const MeshStorage::NextGIRTPart &part : *parts) {
				if (part.blas.is_valid()) {
					bytes += RD::get_singleton()->acceleration_structure_get_size_bytes(part.blas);
				}
			}
		}
		mesh.bytes = bytes;
		mesh.frames_absent = p_frame > entry.value ? uint32_t(p_frame - entry.value) : 0;
		meshes.push_back(mesh);
	}
	for (const RID &stale : stale_meshes) {
		mesh_last_tlas_frame.erase(stale);
	}
	LocalVector<uint32_t> evict;
	evict.resize(meshes.size());
	uint32_t evict_count = 0;
	const bool over = meshes.is_empty() ? false : nextgi_rt_evict_as_memory(meshes.ptr(), meshes.size(), as_budget_bytes, NEXTGI_RT_ABSENT_FRAMES, evict.ptr(), evict_count);
	for (uint32_t i = 0; i < evict_count; i++) {
		mesh_storage->mesh_nextgi_rt_clear_parts(ids[evict[i]]);
		mesh_last_tlas_frame.erase(ids[evict[i]]);
		tlas_traceable = false;
	}
	uint64_t total = 0;
	for (uint32_t i = 0; i < meshes.size(); i++) {
		bool evicted = false;
		for (uint32_t e = 0; e < evict_count; e++) {
			if (evict[e] == meshes[i].id) {
				evicted = true;
				break;
			}
		}
		if (!evicted) {
			total += meshes[i].bytes;
		}
	}
	stats.as_bytes = total;
	as_memory_exceeded = over;
}

String NextGIRTScene::build_instances_shader_source(int p_layout) {
	NextgiRtInstancesShaderRD shader;
	Vector<String> defines;
	defines.push_back("\n#define INSTANCE_64\n");
	defines.push_back("\n#define INSTANCE_72\n");
	shader.initialize(defines);
	const RID version = shader.version_create(false);
	ERR_FAIL_COND_V(!version.is_valid(), String());
	const Vector<String> sources = shader.version_build_variant_stage_sources(version, CLAMP(p_layout, 0, int(INSTANCE_LAYOUT_MAX) - 1));
	shader.version_free(version);
	if (sources.size() <= RenderingDevice::SHADER_STAGE_COMPUTE) {
		return String();
	}
	return sources[RenderingDevice::SHADER_STAGE_COMPUTE];
}

} // namespace RendererRD
