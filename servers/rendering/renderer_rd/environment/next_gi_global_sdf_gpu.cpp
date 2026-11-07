/**************************************************************************/
/*  next_gi_global_sdf_gpu.cpp                                            */
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

#include "next_gi_global_sdf_gpu.h"

#include "core/math/math_funcs.h"
#include "core/math/random_number_generator.h"
#include "servers/rendering/rendering_device.h"

namespace {
struct GPUMeshInfo {
	float bounds_position[3];
	float voxel_size;
	int32_t dimensions[3];
	uint32_t flags;
	int32_t brick_grid[3];
	float band;
	uint32_t page_offset;
	uint32_t brick_offset;
	uint32_t pad[2];
};
static_assert(sizeof(GPUMeshInfo) == 64, "GPUMeshInfo must stay 64 bytes.");

struct GPUInstance {
	float inverse[12];
	float world_min[4];
	float world_max[4];
	uint32_t flags;
	uint32_t pad[3];
};
static_assert(sizeof(GPUInstance) == 96, "GPUInstance must stay 96 bytes.");

struct BrickWork {
	int32_t brick[4];
	uint32_t slot;
	uint32_t instance_offset;
	uint32_t instance_count;
	uint32_t pad;
};
static_assert(sizeof(BrickWork) == 32, "BrickWork must stay 32 bytes.");

struct CompositeParams {
	uint32_t level;
	uint32_t level_padding[3];
	int32_t origin[4];
	int32_t brick[4];
	uint32_t slot;
	uint32_t instance_count;
	uint32_t pad[2];
};
static_assert(sizeof(CompositeParams) == 64, "CompositeParams must stay 64 bytes.");

struct CoarseParams {
	int32_t origin[4];
	uint32_t level;
	uint32_t propagate;
	uint32_t pad[2];
};
static_assert(sizeof(CoarseParams) == 32, "CoarseParams must stay 32 bytes.");
} // namespace

NextGIGlobalSDFGPU::NextGIGlobalSDFGPU(uint32_t p_pool_bricks) {
	capacity = MAX(p_pool_bricks, 1u);
	pool_data.resize(capacity * BRICK_BYTES);
	pool_data.fill(0);
	slot_used.resize(capacity);
	free_slots.resize(capacity);
	for (uint32_t index = 0; index < capacity; index++) {
		slot_used.set(index, 0);
		free_slots.set(index, int32_t(capacity - 1 - index));
	}
	page_tables.resize(NextGIGlobalSDF::MAX_LEVELS);
	for (uint32_t level = 0; level < NextGIGlobalSDF::MAX_LEVELS; level++) {
		PackedInt32Array &table = page_tables.write[level];
		table.resize(BRICKS_PER_LEVEL);
		table.fill(PAGE_UNALLOCATED);
	}
	page_buffers.resize(NextGIGlobalSDF::MAX_LEVELS);
	coarse_buffers.resize(NextGIGlobalSDF::MAX_LEVELS);
	coarse_uniforms.resize(NextGIGlobalSDF::MAX_LEVELS * 2);
}

NextGIGlobalSDFGPU::~NextGIGlobalSDFGPU() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr) {
		if (pipeline.is_valid()) {
			rd->free_rid(pipeline);
		}
		if (version.is_valid()) {
			shader.version_free(version);
		}
		if (coarse_pipeline.is_valid()) {
			rd->free_rid(coarse_pipeline);
		}
		if (coarse_version.is_valid()) {
			coarse_shader.version_free(coarse_version);
		}
		if (pool_buffer.is_valid()) {
			rd->free_rid(pool_buffer);
		}
		if (dynamic_pool_buffer.is_valid()) {
			rd->free_rid(dynamic_pool_buffer);
		}
		if (coverage_buffer.is_valid()) {
			rd->free_rid(coverage_buffer);
		}
		for (const RID &buffer : page_buffers) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
		}
		for (const RID &buffer : coarse_buffers) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
		}
		if (coarse_scratch.is_valid()) {
			rd->free_rid(coarse_scratch);
		}
	}
	pool_buffer = RID();
	dynamic_pool_buffer = RID();
	coverage_buffer = RID();
	page_buffers.clear();
	coarse_buffers.clear();
	coarse_uniforms.clear();
	coarse_scratch = RID();
}

uint32_t NextGIGlobalSDFGPU::_table_index(const Vector3i &p_brick) {
	return uint32_t(p_brick.x) + uint32_t(p_brick.y) * NextGIGlobalSDF::PAGE_SIZE + uint32_t(p_brick.z) * NextGIGlobalSDF::PAGE_SIZE * NextGIGlobalSDF::PAGE_SIZE;
}

void NextGIGlobalSDFGPU::ensure_buffers() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return;
	}
	if (!pool_buffer.is_valid()) {
		pool_buffer = rd->storage_buffer_create(capacity * BRICK_BYTES);
		if (pool_buffer.is_valid()) {
			rd->buffer_update(pool_buffer, 0, pool_data.size(), pool_data.ptr());
		}
	}
	if (!dynamic_pool_buffer.is_valid()) {
		dynamic_pool_buffer = rd->storage_buffer_create(capacity * BRICK_BYTES);
	}
	if (!coverage_buffer.is_valid()) {
		coverage_buffer = rd->storage_buffer_create(capacity * COVERAGE_BYTES);
	}
	PackedByteArray empty_coarse;
	empty_coarse.resize(NextGIGlobalSDF::COARSE_VOXELS * sizeof(float));
	empty_coarse.fill(0);
	for (uint32_t level = 0; level < NextGIGlobalSDF::MAX_LEVELS; level++) {
		if (!page_buffers[level].is_valid()) {
			page_buffers.write[level] = rd->storage_buffer_create(BRICKS_PER_LEVEL * sizeof(int32_t));
			if (page_buffers[level].is_valid()) {
				rd->buffer_update(page_buffers[level], 0, page_tables[level].size() * sizeof(int32_t), page_tables[level].ptr());
			}
		}
		if (!coarse_buffers[level].is_valid()) {
			coarse_buffers.write[level] = rd->storage_buffer_create(empty_coarse.size(), Span<uint8_t>(empty_coarse.ptr(), empty_coarse.size()));
		}
	}
	if (!coarse_scratch.is_valid()) {
		coarse_scratch = rd->storage_buffer_create(empty_coarse.size(), Span<uint8_t>(empty_coarse.ptr(), empty_coarse.size()));
	}
}

bool NextGIGlobalSDFGPU::_ensure_pipeline() {
	if (pipeline_failed) {
		return false;
	}
	if (pipeline.is_valid()) {
		return true;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	shader.initialize(Vector<String>({ "" }));
	version = shader.version_create();
	if (!version.is_valid()) {
		pipeline_failed = true;
		return false;
	}
	pipeline = rd->compute_pipeline_create(shader.version_get_shader(version, 0));
	if (!pipeline.is_valid()) {
		shader.version_free(version);
		version = RID();
		pipeline_failed = true;
		return false;
	}
	return true;
}

bool NextGIGlobalSDFGPU::_update_coarse(uint32_t p_level, const Vector3i &p_origin) {
	if (p_level >= NextGIGlobalSDF::MAX_LEVELS || coarse_pipeline_failed) {
		return false;
	}
	ensure_buffers();
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (!coarse_pipeline.is_valid()) {
		coarse_shader.initialize(Vector<String>({ "" }));
		coarse_version = coarse_shader.version_create();
		if (!coarse_version.is_valid()) {
			coarse_pipeline_failed = true;
			return false;
		}
		coarse_pipeline = rd->compute_pipeline_create(coarse_shader.version_get_shader(coarse_version, 0));
		if (!coarse_pipeline.is_valid()) {
			coarse_shader.version_free(coarse_version);
			coarse_version = RID();
			coarse_pipeline_failed = true;
			return false;
		}
	}
	for (uint32_t phase = 0; phase < 2; phase++) {
		const RID output = phase == 0 ? coarse_buffers[p_level] : coarse_scratch;
		const RID input = phase == 0 ? coarse_scratch : coarse_buffers[p_level];
		RID &set = coarse_uniforms.write[p_level * 2 + phase];
		if (set.is_valid()) {
			continue;
		}
		const Vector<RD::Uniform> uniforms = {
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, pool_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, dynamic_pool_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, page_buffers[p_level]),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, output),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, input),
		};
		set = rd->uniform_set_create(uniforms, coarse_shader.version_get_shader(coarse_version, 0), 0);
		ERR_FAIL_COND_V(!set.is_valid(), false);
	}
	CoarseParams push = {};
	for (int axis = 0; axis < 3; axis++) {
		push.origin[axis] = p_origin[axis];
	}
	push.level = p_level;
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, coarse_pipeline);
	for (uint32_t iteration = 0; iteration < 5; iteration++) {
		push.propagate = iteration > 0 ? 1u : 0u;
		rd->compute_list_bind_uniform_set(list, coarse_uniforms[p_level * 2 + (iteration % 2)], 0);
		rd->compute_list_set_push_constant(list, &push, sizeof(push));
		rd->compute_list_dispatch(list, 4, 4, 4);
		rd->compute_list_add_barrier(list);
	}
	rd->compute_list_end();
	return true;
}

bool NextGIGlobalSDFGPU::_allocate_slot(uint32_t &r_slot) {
	if (free_slots.is_empty()) {
		return false;
	}
	r_slot = uint32_t(free_slots[free_slots.size() - 1]);
	free_slots.remove_at(free_slots.size() - 1);
	slot_used.set(r_slot, 1);
	used++;
	return true;
}

void NextGIGlobalSDFGPU::_free_slot(uint32_t p_slot) {
	if (p_slot >= capacity || slot_used[p_slot] == 0) {
		return;
	}
	slot_used.set(p_slot, 0);
	free_slots.push_back(int32_t(p_slot));
	used--;
}

uint32_t NextGIGlobalSDFGPU::_allocate_brick(uint32_t p_level, const Vector3i &p_brick) {
	if (p_level >= NextGIGlobalSDF::MAX_LEVELS || p_brick.x < 0 || p_brick.y < 0 || p_brick.z < 0 ||
			p_brick.x >= int(NextGIGlobalSDF::PAGE_SIZE) || p_brick.y >= int(NextGIGlobalSDF::PAGE_SIZE) || p_brick.z >= int(NextGIGlobalSDF::PAGE_SIZE)) {
		return UINT32_MAX;
	}
	const uint32_t index = _table_index(p_brick);
	PackedInt32Array &table = page_tables.write[p_level];
	if (table[index] >= 0) {
		return uint32_t(table[index]);
	}
	uint32_t slot = 0;
	if (!_allocate_slot(slot)) {
		return UINT32_MAX;
	}
	table.set(index, int32_t(slot));
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr && page_buffers[p_level].is_valid()) {
		rd->buffer_update(page_buffers[p_level], index * sizeof(int32_t), sizeof(int32_t), table.ptr() + index);
	}
	return slot;
}

void NextGIGlobalSDFGPU::_release_brick(uint32_t p_level, const Vector3i &p_brick) {
	const int32_t slot = get_slot(p_level, p_brick);
	if (slot < 0) {
		return;
	}
	const uint32_t index = _table_index(p_brick);
	_free_slot(uint32_t(slot));
	page_tables.write[p_level].set(index, PAGE_UNALLOCATED);
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr && page_buffers[p_level].is_valid()) {
		rd->buffer_update(page_buffers[p_level], index * sizeof(int32_t), sizeof(int32_t), page_tables[p_level].ptr() + index);
	}
}

bool NextGIGlobalSDFGPU::upload_brick(uint32_t p_level, const Vector3i &p_brick, const float *p_voxels) {
	if (p_voxels == nullptr) {
		return false;
	}
	const uint32_t slot = _allocate_brick(p_level, p_brick);
	if (slot == UINT32_MAX) {
		return false;
	}
	const uint32_t byte_offset = slot * BRICK_BYTES;
	uint16_t *packed = reinterpret_cast<uint16_t *>(pool_data.ptrw() + byte_offset);
	for (uint32_t voxel = 0; voxel < NextGIGlobalSDF::BRICK_VOXELS; voxel++) {
		packed[voxel] = Math::make_half_float(p_voxels[voxel]);
	}
	ensure_buffers();
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr && pool_buffer.is_valid()) {
		rd->buffer_update(pool_buffer, byte_offset, BRICK_BYTES, pool_data.ptr() + byte_offset);
	}
	return true;
}

bool NextGIGlobalSDFGPU::update_bricks(const Vector<NextGIGlobalSDF::Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, const Vector3i *p_bricks, uint32_t p_count) {
	if (p_level >= NextGIGlobalSDF::MAX_LEVELS || (p_count > 0 && p_bricks == nullptr)) {
		return false;
	}
	if (p_count == 0) {
		return true;
	}
	ensure_buffers();
	if (!_ensure_pipeline()) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	Vector<NextGIGlobalSDF::Instance> instances;
	Vector<GPUInstance> gpu_instances;
	Vector<GPUMeshInfo> mesh_infos;
	PackedInt32Array page_data;
	PackedByteArray brick_data;
	for (const NextGIGlobalSDF::Instance &instance : p_instances) {
		if (instance.field == nullptr || !instance.field->is_valid() || instance.field->page_tables.is_empty() || instance.field->brick_data.is_empty()) {
			continue;
		}
		instances.push_back(instance);
		const MeshSDFData *field = instance.field;
		GPUMeshInfo mesh = {};
		mesh.voxel_size = field->voxel_size;
		mesh.flags = field->flags;
		mesh.band = field->mip_bands[0];
		for (int axis = 0; axis < 3; axis++) {
			mesh.bounds_position[axis] = field->bounds.position[axis];
			mesh.dimensions[axis] = field->mip_dimensions[0][axis];
			mesh.brick_grid[axis] = field->mip_brick_grid_dimensions[0][axis];
		}
		mesh.page_offset = page_data.size();
		page_data.append_array(field->page_tables[0]);
		mesh.brick_offset = brick_data.size() / sizeof(uint32_t);
		brick_data.append_array(field->brick_data[0]);
		mesh_infos.push_back(mesh);

		GPUInstance gpu = {};
		const Transform3D inverse = instance.transform.affine_inverse();
		for (int row = 0; row < 3; row++) {
			gpu.inverse[row * 4] = inverse.basis.rows[row].x;
			gpu.inverse[row * 4 + 1] = inverse.basis.rows[row].y;
			gpu.inverse[row * 4 + 2] = inverse.basis.rows[row].z;
			gpu.inverse[row * 4 + 3] = inverse.origin[row];
		}
		const AABB world_bounds = instance.transform.xform(field->bounds);
		const Vector3 scale = instance.transform.basis.get_scale();
		for (int axis = 0; axis < 3; axis++) {
			gpu.world_min[axis] = world_bounds.position[axis];
			gpu.world_max[axis] = world_bounds.position[axis] + world_bounds.size[axis];
		}
		gpu.world_min[3] = MAX(scale.x, MAX(scale.y, scale.z));
		gpu.flags = (instance.dynamic ? 1u : 0u) | (instance.two_sided ? 2u : 0u);
		gpu_instances.push_back(gpu);
	}

	Vector<BrickWork> work;
	Vector<uint32_t> work_indices;
	LocalVector<uint32_t> culled;
	culled.resize(instances.size());
	bool allocation_failed = false;
	for (uint32_t index = 0; index < p_count; index++) {
		const Vector3i &brick = p_bricks[index];
		const uint32_t culled_count = NextGIGlobalSDF::cull_instances_for_brick(instances, p_level, p_origin, brick, culled.ptr(), culled.size());
		if (culled_count == 0) {
			_release_brick(p_level, brick);
			continue;
		}
		const uint32_t slot = _allocate_brick(p_level, brick);
		if (slot == UINT32_MAX) {
			allocation_failed = true;
			continue;
		}
		BrickWork item = {};
		for (int axis = 0; axis < 3; axis++) {
			item.brick[axis] = brick[axis];
		}
		item.slot = slot;
		item.instance_offset = work_indices.size();
		item.instance_count = culled_count;
		for (uint32_t i = 0; i < culled_count; i++) {
			work_indices.push_back(culled[i]);
		}
		work.push_back(item);
		uint32_t coverage_zero[16] = {};
		rd->buffer_update(coverage_buffer, slot * COVERAGE_BYTES, COVERAGE_BYTES, coverage_zero);
	}
	if (work.is_empty()) {
		return _update_coarse(p_level, p_origin);
	}

	const RID instance_buffer = rd->storage_buffer_create(gpu_instances.size() * sizeof(GPUInstance), Span<uint8_t>((const uint8_t *)gpu_instances.ptr(), gpu_instances.size() * sizeof(GPUInstance)));
	const RID mesh_buffer = rd->storage_buffer_create(mesh_infos.size() * sizeof(GPUMeshInfo), Span<uint8_t>((const uint8_t *)mesh_infos.ptr(), mesh_infos.size() * sizeof(GPUMeshInfo)));
	const RID page_buffer = rd->storage_buffer_create(page_data.size() * sizeof(int32_t), Span<uint8_t>((const uint8_t *)page_data.ptr(), page_data.size() * sizeof(int32_t)));
	const RID brick_buffer = rd->storage_buffer_create(brick_data.size(), Span<uint8_t>(brick_data.ptr(), brick_data.size()));
	const RID work_buffer = rd->storage_buffer_create(work.size() * sizeof(BrickWork), Span<uint8_t>((const uint8_t *)work.ptr(), work.size() * sizeof(BrickWork)));
	const RID indices_buffer = rd->storage_buffer_create(work_indices.size() * sizeof(uint32_t), Span<uint8_t>((const uint8_t *)work_indices.ptr(), work_indices.size() * sizeof(uint32_t)));
	if (!instance_buffer.is_valid() || !mesh_buffer.is_valid() || !page_buffer.is_valid() || !brick_buffer.is_valid() || !work_buffer.is_valid() || !indices_buffer.is_valid()) {
		for (RID buffer : { instance_buffer, mesh_buffer, page_buffer, brick_buffer, work_buffer, indices_buffer }) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
		}
		return false;
	}
	rd->set_resource_name(instance_buffer, "NextGI global SDF instances");
	rd->set_resource_name(mesh_buffer, "NextGI global SDF meshes");
	rd->set_resource_name(page_buffer, "NextGI global SDF mesh pages");
	rd->set_resource_name(brick_buffer, "NextGI global SDF mesh bricks");
	const Vector<RD::Uniform> uniforms = {
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, instance_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, mesh_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, page_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, brick_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, pool_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, dynamic_pool_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, coverage_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, work_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, indices_buffer),
	};
	const RID set = rd->uniform_set_create(uniforms, shader.version_get_shader(version, 0), 0);
	if (!set.is_valid()) {
		for (RID buffer : { instance_buffer, mesh_buffer, page_buffer, brick_buffer, work_buffer, indices_buffer }) {
			rd->free_rid(buffer);
		}
		return false;
	}
	CompositeParams push = {};
	push.level = p_level;
	for (int axis = 0; axis < 3; axis++) {
		push.origin[axis] = p_origin[axis];
	}
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	rd->compute_list_set_push_constant(list, &push, sizeof(push));
	rd->compute_list_dispatch(list, work.size(), 1, 1);
	rd->compute_list_end();
	for (RID buffer : { instance_buffer, mesh_buffer, page_buffer, brick_buffer, work_buffer, indices_buffer }) {
		rd->free_rid(buffer);
	}
	return !allocation_failed && _update_coarse(p_level, p_origin);
}

bool NextGIGlobalSDFGPU::download_brick(uint32_t p_level, const Vector3i &p_brick, NextGIGlobalSDF::BrickSplit &r_split) const {
	const int32_t slot = get_slot(p_level, p_brick);
	if (slot < 0) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	const Vector<uint8_t> static_data = rd->buffer_get_data(pool_buffer, uint32_t(slot) * BRICK_BYTES, BRICK_BYTES);
	const Vector<uint8_t> dynamic_data = rd->buffer_get_data(dynamic_pool_buffer, uint32_t(slot) * BRICK_BYTES, BRICK_BYTES);
	const Vector<uint8_t> coverage_data = rd->buffer_get_data(coverage_buffer, uint32_t(slot) * COVERAGE_BYTES, COVERAGE_BYTES);
	if (static_data.size() != BRICK_BYTES || dynamic_data.size() != BRICK_BYTES || coverage_data.size() != COVERAGE_BYTES) {
		return false;
	}
	r_split.static_distance.resize(NextGIGlobalSDF::BRICK_VOXELS);
	r_split.dynamic_distance.resize(NextGIGlobalSDF::BRICK_VOXELS);
	const uint16_t *static_half = reinterpret_cast<const uint16_t *>(static_data.ptr());
	const uint16_t *dynamic_half = reinterpret_cast<const uint16_t *>(dynamic_data.ptr());
	for (uint32_t index = 0; index < NextGIGlobalSDF::BRICK_VOXELS; index++) {
		r_split.static_distance[index] = Math::half_to_float(static_half[index]);
		r_split.dynamic_distance[index] = Math::half_to_float(dynamic_half[index]);
	}
	memcpy(r_split.coverage, coverage_data.ptr(), COVERAGE_BYTES);
	memcpy(pool_data.ptrw() + uint32_t(slot) * BRICK_BYTES, static_data.ptr(), BRICK_BYTES);
	return true;
}

bool NextGIGlobalSDFGPU::download_coarse(uint32_t p_level, LocalVector<float> &r_coarse) const {
	if (p_level >= NextGIGlobalSDF::MAX_LEVELS || !coarse_buffers[p_level].is_valid()) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	const Vector<uint8_t> data = rd->buffer_get_data(coarse_buffers[p_level]);
	if (data.size() != int(NextGIGlobalSDF::COARSE_VOXELS * sizeof(float))) {
		return false;
	}
	r_coarse.resize(NextGIGlobalSDF::COARSE_VOXELS);
	memcpy(r_coarse.ptr(), data.ptr(), data.size());
	return true;
}

bool NextGIGlobalSDFGPU::validate_reference(const Vector<NextGIGlobalSDF::Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, uint32_t p_count, float &r_max_error, uint32_t &r_sign_mismatches, uint32_t &r_checked) const {
	r_max_error = 0.0f;
	r_sign_mismatches = 0;
	r_checked = 0;
	if (p_level >= NextGIGlobalSDF::MAX_LEVELS) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	const Vector<uint8_t> static_data = rd->buffer_get_data(pool_buffer);
	const Vector<uint8_t> dynamic_data = rd->buffer_get_data(dynamic_pool_buffer);
	if (static_data.size() != int(capacity * BRICK_BYTES) || dynamic_data.size() != int(capacity * BRICK_BYTES)) {
		return false;
	}
	memcpy(pool_data.ptrw(), static_data.ptr(), static_data.size());
	RandomNumberGenerator rng;
	rng.set_seed(424242);
	const float extent = NextGIGlobalSDF::level_extent(p_level);
	const float voxel_size = NextGIGlobalSDF::level_voxel_size(p_level);
	const Vector3 minimum = Vector3(p_origin) * voxel_size;
	float worst_error = 0.0f;
	Vector3 worst_point;
	float worst_reference = 0.0f;
	float worst_distance = 0.0f;
	int32_t worst_slot = -1;
	LocalVector<uint32_t> culled;
	culled.resize(p_instances.size());
	for (uint32_t sample = 0; sample < p_count; sample++) {
		const Vector3 point = minimum + Vector3(rng.randf_range(0.0f, extent), rng.randf_range(0.0f, extent), rng.randf_range(0.0f, extent));
		const Vector3i voxel = NextGIGlobalSDF::world_to_voxel(p_level, p_origin, point);
		const Vector3i brick = voxel / int(NextGIGlobalSDF::BRICK_SIZE);
		const int32_t slot = get_slot(p_level, brick);
		float distance = 65504.0f;
		if (slot >= 0) {
			const Vector3i local = voxel - brick * int(NextGIGlobalSDF::BRICK_SIZE);
			const uint32_t index = uint32_t(slot) * NextGIGlobalSDF::BRICK_VOXELS + local.x + 8 * (local.y + 8 * local.z);
			const uint16_t *static_half = reinterpret_cast<const uint16_t *>(static_data.ptr());
			const uint16_t *dynamic_half = reinterpret_cast<const uint16_t *>(dynamic_data.ptr());
			distance = MIN(Math::half_to_float(static_half[index]), Math::half_to_float(dynamic_half[index]));
		}
		float reference = 65504.0f;
		const uint32_t culled_count = NextGIGlobalSDF::cull_instances_for_brick(p_instances, p_level, p_origin, brick, culled.ptr(), culled.size());
		for (uint32_t index = 0; index < culled_count; index++) {
			reference = MIN(reference, NextGIGlobalSDF::sample_instance(p_instances[culled[index]], point));
		}
		const float error = Math::abs(distance - reference);
		if (error > worst_error) {
			worst_error = error;
			worst_point = point;
			worst_reference = reference;
			worst_distance = distance;
			worst_slot = slot;
		}
		r_max_error = MAX(r_max_error, error);
		if (culled_count > 0 && Math::abs(reference) > voxel_size && ((distance < 0.0f) != (reference < 0.0f))) {
			r_sign_mismatches++;
		}
		r_checked++;
	}
	if (r_max_error > NextGIGlobalSDF::level_voxel_size(p_level)) {
		print_line(vformat("GLOBAL_SDF_VALIDATE point=%s reference=%f actual=%f slot=%d origin=%s", worst_point, worst_reference, worst_distance, worst_slot, p_origin));
	}
	return true;
}

void NextGIGlobalSDFGPU::scroll_level(uint32_t p_level, const Vector3i &p_delta_bricks) {
	if (p_level >= NextGIGlobalSDF::MAX_LEVELS || p_delta_bricks == Vector3i()) {
		return;
	}
	if (Math::abs(p_delta_bricks.x) >= int(NextGIGlobalSDF::PAGE_SIZE) || Math::abs(p_delta_bricks.y) >= int(NextGIGlobalSDF::PAGE_SIZE) || Math::abs(p_delta_bricks.z) >= int(NextGIGlobalSDF::PAGE_SIZE)) {
		release_level(p_level);
		return;
	}
	const PackedInt32Array old_table = page_tables[p_level];
	PackedInt32Array new_table;
	new_table.resize(BRICKS_PER_LEVEL);
	new_table.fill(PAGE_UNALLOCATED);
	for (uint32_t index = 0; index < capacity; index++) {
		slot_used.set(index, 0);
	}
	free_slots.clear();
	used = 0;
	for (int z = 0; z < int(NextGIGlobalSDF::PAGE_SIZE); z++) {
		for (int y = 0; y < int(NextGIGlobalSDF::PAGE_SIZE); y++) {
			for (int x = 0; x < int(NextGIGlobalSDF::PAGE_SIZE); x++) {
				const Vector3i old = Vector3i(x, y, z) + p_delta_bricks;
				if (old.x < 0 || old.y < 0 || old.z < 0 || old.x >= int(NextGIGlobalSDF::PAGE_SIZE) || old.y >= int(NextGIGlobalSDF::PAGE_SIZE) || old.z >= int(NextGIGlobalSDF::PAGE_SIZE)) {
					continue;
				}
				const int32_t slot = old_table[_table_index(old)];
				if (slot < 0) {
					continue;
				}
				new_table.set(_table_index(Vector3i(x, y, z)), slot);
				slot_used.set(slot, 1);
				used++;
			}
		}
	}
	for (uint32_t index = 0; index < capacity; index++) {
		if (slot_used[index] == 0) {
			free_slots.push_back(capacity - 1 - index);
		}
	}
	page_tables.write[p_level] = new_table;
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr && page_buffers[p_level].is_valid()) {
		rd->buffer_update(page_buffers[p_level], 0, new_table.size() * sizeof(int32_t), new_table.ptr());
	}
}

void NextGIGlobalSDFGPU::release_level(uint32_t p_level) {
	if (p_level >= NextGIGlobalSDF::MAX_LEVELS) {
		return;
	}
	PackedInt32Array &table = page_tables.write[p_level];
	for (int index = 0; index < table.size(); index++) {
		if (table[index] >= 0) {
			_free_slot(uint32_t(table[index]));
			table.set(index, PAGE_UNALLOCATED);
		}
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr && page_buffers[p_level].is_valid()) {
		rd->buffer_update(page_buffers[p_level], 0, table.size() * sizeof(int32_t), table.ptr());
	}
}

void NextGIGlobalSDFGPU::free_all() {
	for (uint32_t level = 0; level < NextGIGlobalSDF::MAX_LEVELS; level++) {
		release_level(level);
	}
}

int32_t NextGIGlobalSDFGPU::get_slot(uint32_t p_level, const Vector3i &p_brick) const {
	if (p_level >= NextGIGlobalSDF::MAX_LEVELS || p_brick.x < 0 || p_brick.y < 0 || p_brick.z < 0 ||
			p_brick.x >= int(NextGIGlobalSDF::PAGE_SIZE) || p_brick.y >= int(NextGIGlobalSDF::PAGE_SIZE) || p_brick.z >= int(NextGIGlobalSDF::PAGE_SIZE)) {
		return PAGE_UNALLOCATED;
	}
	return page_tables[p_level][_table_index(p_brick)];
}

bool NextGIGlobalSDFGPU::has_brick(uint32_t p_level, const Vector3i &p_brick) const {
	return get_slot(p_level, p_brick) >= 0;
}

float NextGIGlobalSDFGPU::get_voxel(uint32_t p_level, const Vector3i &p_brick, const Vector3i &p_voxel) const {
	const int32_t slot = get_slot(p_level, p_brick);
	if (slot < 0 || p_voxel.x < 0 || p_voxel.y < 0 || p_voxel.z < 0 ||
			p_voxel.x >= int(NextGIGlobalSDF::BRICK_SIZE) || p_voxel.y >= int(NextGIGlobalSDF::BRICK_SIZE) || p_voxel.z >= int(NextGIGlobalSDF::BRICK_SIZE)) {
		return 1e30f;
	}
	const uint32_t index = uint32_t(p_voxel.x) + uint32_t(p_voxel.y) * NextGIGlobalSDF::BRICK_SIZE + uint32_t(p_voxel.z) * NextGIGlobalSDF::BRICK_SIZE * NextGIGlobalSDF::BRICK_SIZE;
	const uint16_t *packed = reinterpret_cast<const uint16_t *>(pool_data.ptr() + uint32_t(slot) * BRICK_BYTES);
	return Math::half_to_float(packed[index]);
}
