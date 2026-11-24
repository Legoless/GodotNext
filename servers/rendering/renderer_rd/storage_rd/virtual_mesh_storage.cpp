/**************************************************************************/
/*  virtual_mesh_storage.cpp                                              */
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

#include "virtual_mesh_storage.h"

#include "servers/rendering/rendering_device.h"

namespace RendererRD {

VirtualMeshStorage::VirtualMeshStorage() {
}

VirtualMeshStorage::~VirtualMeshStorage() {
	free_resources();
}

void VirtualMeshStorage::free_resources() {
	wait_for_streaming_tasks(true);
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	if (record_buffer.is_valid()) {
		rd->free_rid(record_buffer);
		record_buffer = RID();
	}
	if (instance_attributes_buffer.is_valid()) {
		rd->free_rid(instance_attributes_buffer);
		instance_attributes_buffer = RID();
	}
	if (mesh_record_buffer.is_valid()) {
		rd->free_rid(mesh_record_buffer);
		mesh_record_buffer = RID();
	}
	buffer_capacity = 0;
	mesh_buffer_capacity = 0;
	_reset_auxiliary_buffers();
	_reset_dag();
	_reset_pages();
	_reset_clusters();
	_reset_residency();
	_reset_splines();
	_reset_skin_palette();
	_reset_skin_instances();
	_reset_cluster_skin();
	_reset_wind();
}

uint64_t VirtualMeshStorage::_get_max_storage_buffer_range() const {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return 0;
	}
	return rd->limit_get(RD::LIMIT_MAX_STORAGE_BUFFER_RANGE);
}

bool VirtualMeshStorage::_resize_dag(uint32_t p_bytes) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	const uint64_t max_range = get_effective_max_storage_buffer_range();
	ERR_FAIL_COND_V_MSG(max_range > 0 && uint64_t(p_bytes) > max_range, false,
			vformat("NextGeo: DAG buffer size (%d B) exceeds device maxStorageBufferRange (%d B).", uint64_t(p_bytes), max_range));
	if (dag_buffer.is_valid()) {
		rd->free_rid(dag_buffer);
		dag_buffer = RID();
	}
	dag_bytes = p_bytes;
	if (p_bytes == 0) {
		return true;
	}
	dag_buffer = rd->storage_buffer_create(p_bytes);
	return dag_buffer.is_valid();
}

void VirtualMeshStorage::_upload_dag(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || dag_buffer.is_null() || p_size == 0) {
		return;
	}
	ERR_FAIL_COND(p_offset + p_size > dag_bytes);
	rd->buffer_update(dag_buffer, p_offset, p_size, p_data);
}

void VirtualMeshStorage::_reset_dag() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && dag_buffer.is_valid()) {
		rd->free_rid(dag_buffer);
	}
	dag_buffer = RID();
	dag_bytes = 0;
}

bool VirtualMeshStorage::_resize_pages(uint32_t p_bytes) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	const uint64_t max_range = get_effective_max_storage_buffer_range();
	ERR_FAIL_COND_V_MSG(max_range > 0 && uint64_t(p_bytes) > max_range, false,
			vformat("NextGeo: page arena size (%d B) exceeds device maxStorageBufferRange (%d B).", uint64_t(p_bytes), max_range));
	if (page_buffer.is_valid()) {
		rd->free_rid(page_buffer);
		page_buffer = RID();
	}
	page_bytes = p_bytes;
	if (p_bytes == 0) {
		return true;
	}
	page_buffer = rd->storage_buffer_create(p_bytes);
	return page_buffer.is_valid();
}

void VirtualMeshStorage::_upload_pages(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || page_buffer.is_null() || p_size == 0) {
		return;
	}
	ERR_FAIL_COND(p_offset + p_size > page_bytes);
	rd->buffer_update(page_buffer, p_offset, p_size, p_data);
}

void VirtualMeshStorage::_reset_pages() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && page_buffer.is_valid()) {
		rd->free_rid(page_buffer);
	}
	page_buffer = RID();
	page_bytes = 0;
}

bool VirtualMeshStorage::_resize_clusters(uint32_t p_bytes) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	const uint64_t max_range = get_effective_max_storage_buffer_range();
	ERR_FAIL_COND_V_MSG(max_range > 0 && uint64_t(p_bytes) > max_range, false,
			vformat("NextGeo: cluster table size (%d B) exceeds device maxStorageBufferRange (%d B).", uint64_t(p_bytes), max_range));
	if (cluster_buffer.is_valid()) {
		rd->free_rid(cluster_buffer);
		cluster_buffer = RID();
	}
	cluster_bytes = p_bytes;
	if (p_bytes == 0) {
		return true;
	}
	cluster_buffer = rd->storage_buffer_create(p_bytes);
	return cluster_buffer.is_valid();
}

void VirtualMeshStorage::_upload_clusters(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || cluster_buffer.is_null() || p_size == 0) {
		return;
	}
	ERR_FAIL_COND(p_offset + p_size > cluster_bytes);
	rd->buffer_update(cluster_buffer, p_offset, p_size, p_data);
}

void VirtualMeshStorage::_reset_clusters() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && cluster_buffer.is_valid()) {
		rd->free_rid(cluster_buffer);
	}
	cluster_buffer = RID();
	cluster_bytes = 0;
}

bool VirtualMeshStorage::_resize_residency(uint32_t p_bytes) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	const uint64_t max_range = get_effective_max_storage_buffer_range();
	ERR_FAIL_COND_V_MSG(max_range > 0 && uint64_t(p_bytes) > max_range, false,
			vformat("NextGeo: residency buffer size (%d B) exceeds device maxStorageBufferRange (%d B).", uint64_t(p_bytes), max_range));
	if (residency_buffer.is_valid()) {
		rd->free_rid(residency_buffer);
		residency_buffer = RID();
	}
	residency_bytes = p_bytes;
	if (p_bytes == 0) {
		return true;
	}
	residency_buffer = rd->storage_buffer_create(p_bytes);
	return residency_buffer.is_valid();
}

void VirtualMeshStorage::_upload_residency(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || residency_buffer.is_null() || p_size == 0) {
		return;
	}
	ERR_FAIL_COND(p_offset + p_size > residency_bytes);
	rd->buffer_update(residency_buffer, p_offset, p_size, p_data);
}

void VirtualMeshStorage::_reset_residency() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && residency_buffer.is_valid()) {
		rd->free_rid(residency_buffer);
	}
	residency_buffer = RID();
	residency_bytes = 0;
}

bool VirtualMeshStorage::_resize_skin_palette(uint32_t p_bytes) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	const uint64_t max_range = get_effective_max_storage_buffer_range();
	ERR_FAIL_COND_V_MSG(max_range > 0 && uint64_t(p_bytes) > max_range, false,
			vformat("NextGeo: skin palette arena size (%d B) exceeds device maxStorageBufferRange (%d B).", uint64_t(p_bytes), max_range));
	if (skin_palette_buffer.is_valid()) {
		rd->free_rid(skin_palette_buffer);
		skin_palette_buffer = RID();
	}
	skin_palette_bytes = p_bytes;
	if (p_bytes == 0) {
		return true;
	}
	skin_palette_buffer = rd->storage_buffer_create(p_bytes);
	return skin_palette_buffer.is_valid();
}

void VirtualMeshStorage::_upload_skin_palette(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || skin_palette_buffer.is_null() || p_size == 0) {
		return;
	}
	ERR_FAIL_COND(p_offset + p_size > skin_palette_bytes);
	rd->buffer_update(skin_palette_buffer, p_offset, p_size, p_data);
}

void VirtualMeshStorage::_reset_skin_palette() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && skin_palette_buffer.is_valid()) {
		rd->free_rid(skin_palette_buffer);
	}
	skin_palette_buffer = RID();
	skin_palette_bytes = 0;
}

bool VirtualMeshStorage::_resize_skin_instances(uint32_t p_bytes) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	const uint64_t max_range = get_effective_max_storage_buffer_range();
	ERR_FAIL_COND_V_MSG(max_range > 0 && uint64_t(p_bytes) > max_range, false,
			vformat("NextGeo: skin instance side table size (%d B) exceeds device maxStorageBufferRange (%d B).", uint64_t(p_bytes), max_range));
	if (skin_instance_buffer.is_valid()) {
		rd->free_rid(skin_instance_buffer);
		skin_instance_buffer = RID();
	}
	skin_instance_bytes = p_bytes;
	if (p_bytes == 0) {
		return true;
	}
	skin_instance_buffer = rd->storage_buffer_create(p_bytes);
	return skin_instance_buffer.is_valid();
}

void VirtualMeshStorage::_upload_skin_instances(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || skin_instance_buffer.is_null() || p_size == 0) {
		return;
	}
	ERR_FAIL_COND(p_offset + p_size > skin_instance_bytes);
	rd->buffer_update(skin_instance_buffer, p_offset, p_size, p_data);
}

void VirtualMeshStorage::_reset_skin_instances() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && skin_instance_buffer.is_valid()) {
		rd->free_rid(skin_instance_buffer);
	}
	skin_instance_buffer = RID();
	skin_instance_bytes = 0;
}

bool VirtualMeshStorage::_resize_cluster_skin(uint32_t p_bytes) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	const uint64_t max_range = get_effective_max_storage_buffer_range();
	ERR_FAIL_COND_V_MSG(max_range > 0 && uint64_t(p_bytes) > max_range, false,
			vformat("NextGeo: cluster skin table size (%d B) exceeds device maxStorageBufferRange (%d B).", uint64_t(p_bytes), max_range));
	if (cluster_skin_buffer.is_valid()) {
		rd->free_rid(cluster_skin_buffer);
		cluster_skin_buffer = RID();
	}
	cluster_skin_bytes = p_bytes;
	if (p_bytes == 0) {
		return true;
	}
	cluster_skin_buffer = rd->storage_buffer_create(p_bytes);
	return cluster_skin_buffer.is_valid();
}

void VirtualMeshStorage::_upload_cluster_skin(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || cluster_skin_buffer.is_null() || p_size == 0) {
		return;
	}
	ERR_FAIL_COND(p_offset + p_size > cluster_skin_bytes);
	rd->buffer_update(cluster_skin_buffer, p_offset, p_size, p_data);
}

void VirtualMeshStorage::_reset_cluster_skin() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && cluster_skin_buffer.is_valid()) {
		rd->free_rid(cluster_skin_buffer);
	}
	cluster_skin_buffer = RID();
	cluster_skin_bytes = 0;
}

bool VirtualMeshStorage::_ensure_record_gpu_capacity(uint32_t p_capacity) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_capacity == 0) {
		return false;
	}
	if (record_buffer.is_valid() && instance_attributes_buffer.is_valid() && buffer_capacity >= p_capacity) {
		return false;
	}
	if (record_buffer.is_valid()) {
		rd->free_rid(record_buffer);
		record_buffer = RID();
	}
	if (instance_attributes_buffer.is_valid()) {
		rd->free_rid(instance_attributes_buffer);
		instance_attributes_buffer = RID();
	}
	buffer_capacity = MAX(p_capacity, record_capacity);
	record_buffer = rd->storage_buffer_create(buffer_capacity * sizeof(Record));
	instance_attributes_buffer = rd->storage_buffer_create(buffer_capacity * sizeof(InstanceAttributes));
	ERR_FAIL_COND_V(record_buffer.is_null() || instance_attributes_buffer.is_null(), false);
	if (!records.is_empty()) {
		rd->buffer_update(record_buffer, 0, records.size() * sizeof(Record), records.ptr());
	}
	if (!instance_attributes.is_empty()) {
		rd->buffer_update(instance_attributes_buffer, 0, instance_attributes.size() * sizeof(InstanceAttributes), instance_attributes.ptr());
	}
	return true;
}

void VirtualMeshStorage::_clear_record_range(uint32_t p_first, uint32_t p_count) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_count == 0 || record_buffer.is_null() || p_first + p_count > buffer_capacity) {
		return;
	}
	rd->buffer_clear(record_buffer, p_first * sizeof(Record), p_count * sizeof(Record));
}

void VirtualMeshStorage::_upload_bucket_surface_masks(const BucketSurfaceMaskGPU *p_data, uint32_t p_count) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_data == nullptr || p_count == 0) {
		return;
	}
	if (bucket_surface_masks_buffer.is_null() || bucket_surface_masks_capacity < p_count) {
		if (bucket_surface_masks_buffer.is_valid()) {
			rd->free_rid(bucket_surface_masks_buffer);
		}
		bucket_surface_masks_capacity = MAX(Math::next_power_of_2(p_count), 256u);
		bucket_surface_masks_buffer = rd->storage_buffer_create(bucket_surface_masks_capacity * sizeof(BucketSurfaceMaskGPU));
		ERR_FAIL_COND(bucket_surface_masks_buffer.is_null());
	}
	rd->buffer_update(bucket_surface_masks_buffer, 0, p_count * sizeof(BucketSurfaceMaskGPU), p_data);
}

void VirtualMeshStorage::_upload_assembly_buffers(const AssemblyHeaderGPU *p_headers, uint32_t p_header_count, const AssemblyChildGPU *p_children, uint32_t p_child_count, const AssemblyPartGPU *p_parts, uint32_t p_part_count) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	auto ensure_upload = [&](RID &r_buf, uint32_t &r_cap, const void *p_src, uint32_t p_count, uint32_t p_stride) {
		const uint32_t needed = MAX(p_count, 1u);
		if (r_buf.is_null() || r_cap < needed) {
			if (r_buf.is_valid()) {
				rd->free_rid(r_buf);
			}
			r_cap = MAX(Math::next_power_of_2(needed), 16u);
			Vector<uint8_t> zeros;
			zeros.resize_initialized(r_cap * p_stride);
			r_buf = rd->storage_buffer_create(r_cap * p_stride, zeros);
			ERR_FAIL_COND(r_buf.is_null());
		}
		if (p_src != nullptr && p_count > 0) {
			rd->buffer_update(r_buf, 0, p_count * p_stride, p_src);
		}
	};
	ensure_upload(assembly_headers_buffer, assembly_headers_capacity, p_headers, p_header_count, sizeof(AssemblyHeaderGPU));
	ensure_upload(assembly_children_buffer, assembly_children_capacity, p_children, p_child_count, sizeof(AssemblyChildGPU));
	ensure_upload(assembly_parts_buffer, assembly_parts_capacity, p_parts, p_part_count, sizeof(AssemblyPartGPU));
}

void VirtualMeshStorage::_reset_auxiliary_buffers() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr) {
		if (bucket_surface_masks_buffer.is_valid()) {
			rd->free_rid(bucket_surface_masks_buffer);
		}
		if (assembly_headers_buffer.is_valid()) {
			rd->free_rid(assembly_headers_buffer);
		}
		if (assembly_children_buffer.is_valid()) {
			rd->free_rid(assembly_children_buffer);
		}
		if (assembly_parts_buffer.is_valid()) {
			rd->free_rid(assembly_parts_buffer);
		}
	}
	bucket_surface_masks_buffer = RID();
	bucket_surface_masks_capacity = 0;
	assembly_headers_buffer = RID();
	assembly_headers_capacity = 0;
	assembly_children_buffer = RID();
	assembly_children_capacity = 0;
	assembly_parts_buffer = RID();
	assembly_parts_capacity = 0;
}

void VirtualMeshStorage::_upload_records(uint32_t p_first, uint32_t p_count) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_count == 0) {
		return;
	}
	if (_ensure_record_gpu_capacity(record_capacity)) {
		return;
	}
	ERR_FAIL_COND(record_buffer.is_null() || instance_attributes_buffer.is_null());
	ERR_FAIL_COND(p_first + p_count > record_capacity);
	rd->buffer_update(record_buffer, p_first * sizeof(Record), p_count * sizeof(Record), records.ptr() + p_first);
	rd->buffer_update(instance_attributes_buffer, p_first * sizeof(InstanceAttributes), p_count * sizeof(InstanceAttributes), instance_attributes.ptr() + p_first);
}

void VirtualMeshStorage::_upload_mesh_records(uint32_t p_first, uint32_t p_count) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_count == 0) {
		return;
	}
	const uint32_t required = uint32_t(mesh_records.size());
	if (mesh_record_buffer.is_null() || mesh_buffer_capacity < required) {
		if (mesh_record_buffer.is_valid()) {
			rd->free_rid(mesh_record_buffer);
		}
		mesh_buffer_capacity = required;
		mesh_record_buffer = rd->storage_buffer_create(mesh_buffer_capacity * sizeof(MeshRecord));
		ERR_FAIL_COND(mesh_record_buffer.is_null());
		rd->buffer_update(mesh_record_buffer, 0, required * sizeof(MeshRecord), mesh_records.ptr());
		return;
	}
	ERR_FAIL_COND(mesh_record_buffer.is_null());
	ERR_FAIL_COND(p_first + p_count > mesh_buffer_capacity);
	rd->buffer_update(mesh_record_buffer, p_first * sizeof(MeshRecord), p_count * sizeof(MeshRecord), mesh_records.ptr() + p_first);
}

RID VirtualMeshStorage::get_spline_buffer() const {
	if (spline_buffer.is_null()) {
		RenderingDevice *rd = RD::get_singleton();
		if (rd != nullptr) {
			SplineDeformEntry zero_entry;
			spline_buffer = rd->storage_buffer_create(sizeof(SplineDeformEntry), Span<uint8_t>(reinterpret_cast<uint8_t *>(&zero_entry), sizeof(SplineDeformEntry)));
			spline_buffer_capacity = spline_buffer.is_valid() ? 1u : 0u;
		}
	}
	return spline_buffer;
}

void VirtualMeshStorage::_upload_splines(uint32_t p_first, uint32_t p_count) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_count == 0) {
		return;
	}
	const uint32_t required = MAX(uint32_t(spline_entries.size()), 1u);
	if (spline_buffer.is_null() || spline_buffer_capacity < required) {
		if (spline_buffer.is_valid()) {
			rd->free_rid(spline_buffer);
			spline_buffer = RID();
		}
		uint32_t new_cap = MAX(spline_buffer_capacity, 1u);
		while (new_cap < required) {
			new_cap *= 2u;
		}
		spline_buffer = rd->storage_buffer_create(new_cap * sizeof(SplineDeformEntry));
		ERR_FAIL_COND(spline_buffer.is_null());
		spline_buffer_capacity = new_cap;
		rd->buffer_update(spline_buffer, 0, spline_entries.size() * sizeof(SplineDeformEntry), spline_entries.ptr());
		return;
	}
	ERR_FAIL_COND(p_first + p_count > spline_buffer_capacity);
	rd->buffer_update(spline_buffer, p_first * sizeof(SplineDeformEntry), p_count * sizeof(SplineDeformEntry), spline_entries.ptr() + p_first);
}

void VirtualMeshStorage::_reset_splines() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && spline_buffer.is_valid()) {
		rd->free_rid(spline_buffer);
	}
	spline_buffer = RID();
	spline_buffer_capacity = 0;
}

RID VirtualMeshStorage::get_wind_params_buffer() const {
	if (wind_params_buffer.is_null()) {
		RenderingDevice *rd = RD::get_singleton();
		if (rd != nullptr) {
			WindParamsGPU init_params = wind_params;
			wind_params_buffer = rd->storage_buffer_create(sizeof(WindParamsGPU), Span<uint8_t>(reinterpret_cast<uint8_t *>(&init_params), sizeof(WindParamsGPU)));
		}
	}
	return wind_params_buffer;
}

RID VirtualMeshStorage::get_wind_meshes_buffer() const {
	if (wind_meshes_buffer.is_null()) {
		RenderingDevice *rd = RD::get_singleton();
		if (rd != nullptr) {
			WindMeshGPU zero_mesh;
			wind_meshes_buffer = rd->storage_buffer_create(sizeof(WindMeshGPU), Span<uint8_t>(reinterpret_cast<uint8_t *>(&zero_mesh), sizeof(WindMeshGPU)));
			wind_meshes_capacity = wind_meshes_buffer.is_valid() ? 1u : 0u;
		}
	}
	return wind_meshes_buffer;
}

void VirtualMeshStorage::_upload_wind_params(const WindParamsGPU &p_params) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	RID buf = get_wind_params_buffer();
	if (buf.is_valid()) {
		rd->buffer_update(buf, 0, sizeof(WindParamsGPU), &p_params);
	}
}

void VirtualMeshStorage::_upload_wind_meshes(uint32_t p_first, uint32_t p_count) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_count == 0) {
		return;
	}
	const uint32_t required = MAX(uint32_t(wind_meshes.size()), 1u);
	if (wind_meshes_buffer.is_null() || wind_meshes_capacity < required) {
		if (wind_meshes_buffer.is_valid()) {
			rd->free_rid(wind_meshes_buffer);
			wind_meshes_buffer = RID();
		}
		uint32_t new_cap = MAX(wind_meshes_capacity, 1u);
		while (new_cap < required) {
			new_cap *= 2u;
		}
		wind_meshes_buffer = rd->storage_buffer_create(new_cap * sizeof(WindMeshGPU));
		ERR_FAIL_COND(wind_meshes_buffer.is_null());
		wind_meshes_capacity = new_cap;
		if (!wind_meshes.is_empty()) {
			rd->buffer_update(wind_meshes_buffer, 0, wind_meshes.size() * sizeof(WindMeshGPU), wind_meshes.ptr());
		}
		return;
	}
	ERR_FAIL_COND(p_first + p_count > wind_meshes_capacity);
	rd->buffer_update(wind_meshes_buffer, p_first * sizeof(WindMeshGPU), p_count * sizeof(WindMeshGPU), wind_meshes.ptr() + p_first);
}

void VirtualMeshStorage::_reset_wind() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr) {
		if (wind_params_buffer.is_valid()) {
			rd->free_rid(wind_params_buffer);
		}
		if (wind_meshes_buffer.is_valid()) {
			rd->free_rid(wind_meshes_buffer);
		}
	}
	wind_params_buffer = RID();
	wind_meshes_buffer = RID();
	wind_meshes_capacity = 0;
}

bool VirtualMeshStorage::_query_device_dynamic_wind_tier() const {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	return rd->has_feature(RD::SUPPORTS_DRAW_INDIRECT_COUNT) &&
			(rd->has_feature(RD::SUPPORTS_IMAGE_INT64_ATOMICS) || rd->has_feature(RD::SUPPORTS_BUFFER_INT64_ATOMICS));
}

} // namespace RendererRD
