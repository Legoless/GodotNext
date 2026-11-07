/**************************************************************************/
/*  next_gi_far_field.cpp                                                 */
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

#include "next_gi_far_field.h"

#include "core/math/math_funcs.h"

#include <thirdparty/meshoptimizer/meshoptimizer.h>

uint64_t NextGIFarField::ProxySurface::bytes() const {
	return uint64_t(positions.size() + normals.size() + uvs.size()) * sizeof(float) + uint64_t(indices.size()) * sizeof(uint32_t);
}

Vector3i NextGIFarField::cell_coordinate(const Vector3 &p_position) {
	return Vector3i(
			int(Math::floor(p_position.x / CELL_SIZE)),
			int(Math::floor(p_position.y / CELL_SIZE)),
			int(Math::floor(p_position.z / CELL_SIZE)));
}

Transform3D NextGIFarField::cell_transform(const Vector3i &p_cell) {
	Transform3D transform;
	transform.origin = Vector3(p_cell) * CELL_SIZE + Vector3(CELL_SIZE * 0.5f, CELL_SIZE * 0.5f, CELL_SIZE * 0.5f);
	return transform;
}

bool NextGIFarField::include_source(const Source &p_source, bool p_visible, bool p_gi_contributing, bool p_opaque, bool p_skinned, bool p_dynamic, float p_near_distance) {
	if (p_source.mesh.is_null() || !p_visible || !p_gi_contributing || !p_opaque || p_skinned || p_dynamic || p_source.mode == RSE::FAR_FIELD_NEVER) {
		return false;
	}
	if (p_source.mode == RSE::FAR_FIELD_ALWAYS) {
		return true;
	}
	return p_source.visibility_end <= 0.0f || p_source.visibility_end >= p_near_distance;
}

bool NextGIFarField::intersects_shell(const AABB &p_bounds, const Vector3 &p_camera, float p_near_distance, float p_far_distance) {
	if (p_far_distance <= p_near_distance || p_bounds.size.x < 0.0f || p_bounds.size.y < 0.0f || p_bounds.size.z < 0.0f) {
		return false;
	}
	const Vector3 nearest = p_camera.clamp(p_bounds.position, p_bounds.position + p_bounds.size);
	if (nearest.distance_squared_to(p_camera) > p_far_distance * p_far_distance) {
		return false;
	}
	float farthest_squared = 0.0f;
	for (int axis = 0; axis < 3; axis++) {
		const float low = p_bounds.position[axis] - p_camera[axis];
		const float high = p_bounds.position[axis] + p_bounds.size[axis] - p_camera[axis];
		const float farthest = MAX(Math::abs(low), Math::abs(high));
		farthest_squared += farthest * farthest;
	}
	return farthest_squared >= p_near_distance * p_near_distance;
}

float NextGIFarField::occlusion_visibility(bool p_hit) {
	return p_hit ? 0.0f : 1.0f;
}

uint64_t NextGIFarField::fingerprint(const Source &p_source) {
	uint64_t hash = 14695981039346656037ull;
	auto mix = [&hash](uint64_t p_value) {
		hash ^= p_value;
		hash *= 1099511628211ull;
	};
	mix(p_source.mesh.get_id());
	mix(p_source.revision);
	mix(p_source.mode);
	for (int row = 0; row < 3; row++) {
		for (int column = 0; column < 3; column++) {
			const float value = p_source.transform.basis.rows[row][column];
			uint32_t bits = 0;
			memcpy(&bits, &value, sizeof(bits));
			mix(bits);
		}
		const float value = p_source.transform.origin[row];
		uint32_t bits = 0;
		memcpy(&bits, &value, sizeof(bits));
		mix(bits);
	}
	for (const RID &material : p_source.materials) {
		mix(material.get_id());
	}
	return hash;
}

bool NextGIFarField::simplify_surface(ProxySurface &r_surface, uint32_t p_target_index_count, float p_error) {
	if (r_surface.positions.is_empty() || r_surface.positions.size() % 3 != 0 || r_surface.indices.is_empty() || r_surface.indices.size() % 3 != 0) {
		return false;
	}
	const uint32_t vertex_count = r_surface.positions.size() / 3;
	for (const uint32_t index : r_surface.indices) {
		if (index >= vertex_count) {
			return false;
		}
	}
	LocalVector<uint32_t> simplified;
	simplified.resize(r_surface.indices.size());
	const uint32_t target = MIN(p_target_index_count - p_target_index_count % 3, r_surface.indices.size());
	if (target == 0) {
		return false;
	}
	float result_error = 0.0f;
	const size_t simplified_count = meshopt_simplify(simplified.ptr(), r_surface.indices.ptr(), r_surface.indices.size(), r_surface.positions.ptr(), vertex_count, sizeof(float) * 3, target, CLAMP(p_error, 0.0f, 1.0f), 0, &result_error);
	if (simplified_count == 0 || simplified_count % 3 != 0) {
		return false;
	}
	simplified.resize(simplified_count);

	LocalVector<uint32_t> remap;
	remap.resize(vertex_count);
	for (uint32_t &index : remap) {
		index = UINT32_MAX;
	}
	LocalVector<float> positions;
	LocalVector<float> normals;
	LocalVector<float> uvs;
	for (uint32_t &index : simplified) {
		const uint32_t old = index;
		if (remap[old] == UINT32_MAX) {
			remap[old] = positions.size() / 3;
			for (int component = 0; component < 3; component++) {
				positions.push_back(r_surface.positions[old * 3 + component]);
			}
			if (r_surface.normals.size() == r_surface.positions.size()) {
				for (int component = 0; component < 3; component++) {
					normals.push_back(r_surface.normals[old * 3 + component]);
				}
			}
			if (r_surface.uvs.size() == vertex_count * 2) {
				uvs.push_back(r_surface.uvs[old * 2]);
				uvs.push_back(r_surface.uvs[old * 2 + 1]);
			}
		}
		index = remap[old];
	}
	r_surface.positions = positions;
	r_surface.normals = normals;
	r_surface.uvs = uvs;
	r_surface.indices = simplified;
	return true;
}

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererRD;

uint64_t NextGIFarField::_cell_key(const Vector3i &p_cell) {
	return (uint64_t(uint32_t(p_cell.x) & 0x1FFFFFu)) |
			(uint64_t(uint32_t(p_cell.y) & 0x1FFFFFu) << 21) |
			(uint64_t(uint32_t(p_cell.z) & 0x1FFFFFu) << 42);
}

void NextGIFarField::_collect_geometry(const Source &p_source, const Vector3i &p_cell, LocalVector<ProxySurface> &r_surfaces) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RenderingServer *server = RenderingServer::get_singleton();
	if (mesh_storage == nullptr || server == nullptr) {
		return;
	}
	const Transform3D to_cell = cell_transform(p_cell).affine_inverse() * p_source.transform;
	const Basis normal_basis = to_cell.basis.inverse().transposed();
	const int surface_count = mesh_storage->mesh_get_surface_count(p_source.mesh);
	for (int surface = 0; surface < surface_count; surface++) {
		RendererRD::MeshStorage::NextGIRTSurfaceInfo info;
		if (!mesh_storage->mesh_nextgi_rt_get_surface_info(p_source.mesh, surface, info) || !info.eligible) {
			continue;
		}
		const RID material = surface < p_source.materials.size() && p_source.materials[surface].is_valid() ? p_source.materials[surface] : info.material;
		if (material_storage != nullptr && material.is_valid() && material_storage->material_get_nextgi_rt_class(material) != MaterialStorage::ShaderData::NEXTGI_RT_OPAQUE) {
			continue;
		}
		const Array arrays = server->mesh_surface_get_arrays(p_source.mesh, surface);
		if (arrays.size() != RSE::ARRAY_MAX) {
			continue;
		}
		const PackedVector3Array positions = arrays[RSE::ARRAY_VERTEX];
		const PackedVector3Array normals = arrays[RSE::ARRAY_NORMAL];
		const PackedVector2Array uvs = arrays[RSE::ARRAY_TEX_UV];
		const PackedInt32Array indices = arrays[RSE::ARRAY_INDEX];
		if (positions.is_empty()) {
			continue;
		}
		ProxySurface *proxy = nullptr;
		for (ProxySurface &candidate : r_surfaces) {
			if (candidate.material == material) {
				proxy = &candidate;
				break;
			}
		}
		if (proxy == nullptr) {
			ProxySurface created;
			created.material = material;
			r_surfaces.push_back(created);
			proxy = &r_surfaces[r_surfaces.size() - 1];
		}
		const uint32_t first_vertex = proxy->positions.size() / 3;
		for (int vertex = 0; vertex < positions.size(); vertex++) {
			const Vector3 position = to_cell.xform(positions[vertex]);
			proxy->positions.push_back(position.x);
			proxy->positions.push_back(position.y);
			proxy->positions.push_back(position.z);
			if (vertex < normals.size()) {
				const Vector3 normal = normal_basis.xform(normals[vertex]).normalized();
				proxy->normals.push_back(normal.x);
				proxy->normals.push_back(normal.y);
				proxy->normals.push_back(normal.z);
			}
			if (vertex < uvs.size()) {
				proxy->uvs.push_back(uvs[vertex].x);
				proxy->uvs.push_back(uvs[vertex].y);
			}
		}
		if (indices.is_empty()) {
			for (int index = 0; index < positions.size(); index++) {
				proxy->indices.push_back(first_vertex + index);
			}
		} else {
			for (const int32_t index : indices) {
				proxy->indices.push_back(first_vertex + index);
			}
		}
	}
}

void NextGIFarField::_build_work(void *p_userdata) {
	Work *work = static_cast<Work *>(p_userdata);
	uint64_t total_indices = 0;
	for (const ProxySurface &surface : work->surfaces) {
		total_indices += surface.indices.size();
	}
	for (ProxySurface &surface : work->surfaces) {
		if (surface.indices.is_empty()) {
			continue;
		}
		const uint32_t target = total_indices > MAX_CELL_TRIANGLES * 3 ? MAX(uint64_t(3), uint64_t(MAX_CELL_TRIANGLES) * surface.indices.size() / total_indices) : surface.indices.size();
		if (simplify_surface(surface, target, 0.25f / CELL_SIZE)) {
			work->bytes += surface.bytes();
		} else {
			surface.indices.clear();
		}
	}
	for (uint32_t i = 0; i < work->surfaces.size();) {
		if (work->surfaces[i].indices.is_empty()) {
			work->surfaces.remove_at(i);
		} else {
			i++;
		}
	}
	MutexLock lock(completed_mutex);
	completed_work.push_back(work);
}

void NextGIFarField::_finish_work(Work *p_work) {
	Cell *cell = cells.getptr(_cell_key(p_work->coordinate));
	if (cell == nullptr || cell->fingerprint != p_work->fingerprint) {
		return;
	}
	_release_cell(*cell);
	cell->failed = false;
	if (p_work->surfaces.is_empty()) {
		cell->failed = true;
		return;
	}
	RenderingServer *server = RenderingServer::get_singleton();
	const RID mesh = server->mesh_create();
	for (const ProxySurface &surface : p_work->surfaces) {
		PackedVector3Array positions;
		PackedVector3Array normals;
		PackedVector2Array uvs;
		PackedInt32Array indices;
		positions.resize(surface.positions.size() / 3);
		normals.resize(surface.normals.size() / 3);
		uvs.resize(surface.uvs.size() / 2);
		indices.resize(surface.indices.size());
		for (uint32_t i = 0; i < positions.size(); i++) {
			positions.set(i, Vector3(surface.positions[i * 3], surface.positions[i * 3 + 1], surface.positions[i * 3 + 2]));
		}
		for (uint32_t i = 0; i < normals.size(); i++) {
			normals.set(i, Vector3(surface.normals[i * 3], surface.normals[i * 3 + 1], surface.normals[i * 3 + 2]));
		}
		for (uint32_t i = 0; i < uvs.size(); i++) {
			uvs.set(i, Vector2(surface.uvs[i * 2], surface.uvs[i * 2 + 1]));
		}
		for (uint32_t i = 0; i < indices.size(); i++) {
			indices.set(i, surface.indices[i]);
		}
		Array arrays;
		arrays.resize(RSE::ARRAY_MAX);
		arrays[RSE::ARRAY_VERTEX] = positions;
		arrays[RSE::ARRAY_NORMAL] = normals;
		arrays[RSE::ARRAY_TEX_UV] = uvs;
		arrays[RSE::ARRAY_INDEX] = indices;
		server->mesh_add_surface_from_arrays(mesh, RSE::PRIMITIVE_TRIANGLES, arrays);
		server->mesh_surface_set_material(mesh, server->mesh_get_surface_count(mesh) - 1, surface.material);
	}
	cell->mesh = mesh;
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	cell->instance = scene != nullptr ? scene->geometry_instance_create(mesh) : nullptr;
	if (cell->instance != nullptr) {
		const Transform3D transform = cell_transform(cell->coordinate);
		const AABB local_bounds = RendererRD::MeshStorage::get_singleton()->mesh_get_aabb(mesh);
		cell->instance->set_use_baked_light(true);
		cell->instance->set_transform(transform, local_bounds, transform.xform(local_bounds));
	}
	cell->blas = owner->build_far_proxy_mesh(mesh);
	if (cell->instance == nullptr || cell->blas.is_null()) {
		cell->failed = true;
		_release_cell(*cell);
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	cell->geometry_bytes = p_work->bytes;
	cell->bytes = cell->geometry_bytes + rd->acceleration_structure_get_size_bytes(cell->blas);
	proxy_bytes += cell->bytes;
	if (proxy_bytes > MAX_PROXY_BYTES) {
		budget_exceeded = true;
		WARN_PRINT("NextGI: far field dropped because its 128 MiB proxy budget was exceeded.");
		for (KeyValue<uint64_t, Cell> &entry : cells) {
			_release_cell(entry.value);
		}
		cells.clear();
		tlas_ready = false;
		if (tlas.is_valid()) {
			rd->free_rid(tlas);
			tlas = RID();
		}
	}
}

void NextGIFarField::_release_cell(Cell &r_cell) {
	tlas_dirty = true;
	tlas_ready = false;
	proxy_bytes -= MIN(proxy_bytes, r_cell.bytes);
	r_cell.bytes = 0;
	if (r_cell.instance != nullptr) {
		if (NextGIRelight::get_singleton() != nullptr) {
			NextGIRelight::get_singleton()->release_instance(r_cell.instance);
		}
		if (RendererSceneRenderRD::get_singleton() != nullptr) {
			RendererSceneRenderRD::get_singleton()->geometry_instance_free(r_cell.instance);
		}
		r_cell.instance = nullptr;
	}
	if (r_cell.mesh.is_valid()) {
		if (RendererRD::MeshStorage::get_singleton() != nullptr) {
			RendererRD::MeshStorage::get_singleton()->mesh_nextgi_rt_clear_parts(r_cell.mesh);
		}
		RenderingServer::get_singleton()->free_rid(r_cell.mesh);
		r_cell.mesh = RID();
	}
	r_cell.blas = RID();
	r_cell.compacted = false;
	r_cell.building = false;
}

void NextGIFarField::init(RendererRD::NextGIRTScene *p_owner) {
	owner = p_owner;
	if (tlas.is_valid()) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr) {
		tlas = rd->tlas_create(4096, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	}
}

void NextGIFarField::update(const RenderDataRD *p_render_data, LocalVector<RendererRD::NextGIInstanceRecord> &r_records, LocalVector<RendererRD::NextGISurfaceRecord> &r_surfaces) {
	if (!bool(GLOBAL_GET("rendering/global_illumination/nextgi/far_field")) || p_render_data == nullptr || p_render_data->sdfgi_update_data == nullptr) {
		free();
		return;
	}
	if (!tlas.is_valid()) {
		init(owner);
	}
	if (budget_exceeded) {
		return;
	}
	LocalVector<Work *> completed;
	{
		MutexLock lock(completed_mutex);
		completed = completed_work;
		completed_work.clear();
	}
	for (Work *work : completed) {
		_finish_work(work);
		for (uint32_t i = 0; i < pending_work.size(); i++) {
			if (pending_work[i] == work) {
				pending_work.remove_at(i);
				break;
			}
		}
		memdelete(work);
	}
	if (budget_exceeded) {
		return;
	}
	const float near_distance = float(GLOBAL_GET("rendering/global_illumination/nextgi/rt_scene_radius"));
	const float far_distance = float(GLOBAL_GET("rendering/global_illumination/nextgi/far_field_distance"));
	const Vector3 camera = p_render_data->scene_data != nullptr ? p_render_data->scene_data->cam_transform.origin : Vector3();
	struct Group {
		Vector3i coordinate;
		AABB bounds;
		uint64_t fingerprint = 14695981039346656037ull;
		LocalVector<Source> sources;
	};
	HashMap<uint64_t, Group> groups;
	const Source *sources = p_render_data->sdfgi_update_data->nextgi_far_sources;
	const uint32_t source_count = p_render_data->sdfgi_update_data->nextgi_far_source_count;
	for (uint32_t index = 0; index < source_count; index++) {
		const Source &source = sources[index];
		if (!intersects_shell(source.bounds, camera, near_distance, far_distance)) {
			continue;
		}
		const Vector3i coordinate = cell_coordinate(source.bounds.get_center());
		const uint64_t key = _cell_key(coordinate);
		Group &group = groups[key];
		group.coordinate = coordinate;
		if (group.sources.is_empty()) {
			group.bounds = source.bounds;
		} else {
			group.bounds.merge_with(source.bounds);
		}
		group.sources.push_back(source);
		group.fingerprint = group.fingerprint * 1099511628211ull ^ fingerprint(source);
	}
	HashSet<uint64_t> desired;
	for (KeyValue<uint64_t, Group> &entry : groups) {
		desired.insert(entry.key);
		Group &group = entry.value;
		Cell &cell = cells[entry.key];
		if (cell.fingerprint == group.fingerprint) {
			cell.bounds = group.bounds;
			continue;
		}
		cell.coordinate = group.coordinate;
		cell.fingerprint = group.fingerprint;
		cell.bounds = group.bounds;
		cell.failed = false;
		cell.building = true;
		Work *work = memnew(Work);
		work->coordinate = group.coordinate;
		work->fingerprint = group.fingerprint;
		work->sources = group.sources;
		for (const Source &source : group.sources) {
			_collect_geometry(source, group.coordinate, work->surfaces);
		}
		if (work->surfaces.is_empty()) {
			cell.failed = true;
			memdelete(work);
			continue;
		}
		work->task = WorkerThreadPool::get_singleton()->add_template_task(this, &NextGIFarField::_build_work, work, false, "NextGI far field proxy");
		pending_work.push_back(work);
	}
	LocalVector<uint64_t> removed;
	for (KeyValue<uint64_t, Cell> &entry : cells) {
		if (!desired.has(entry.key)) {
			removed.push_back(entry.key);
		}
	}
	for (const uint64_t key : removed) {
		_release_cell(cells[key]);
		cells.erase(key);
	}
	_update_tlas(camera, near_distance, far_distance, r_records, r_surfaces);
	stats.pending_count = pending_work.size();
	stats.proxy_bytes = proxy_bytes;
	stats.budget_exceeded = budget_exceeded;
}

void NextGIFarField::free() {
	for (Work *work : pending_work) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(work->task);
		memdelete(work);
	}
	pending_work.clear();
	{
		MutexLock lock(completed_mutex);
		completed_work.clear();
	}
	for (KeyValue<uint64_t, Cell> &entry : cells) {
		_release_cell(entry.value);
	}
	cells.clear();
	if (tlas.is_valid() && RenderingDevice::get_singleton() != nullptr) {
		RenderingDevice::get_singleton()->free_rid(tlas);
	}
	tlas = RID();
	tlas_ready = false;
	tlas_dirty = true;
	budget_exceeded = false;
	proxy_bytes = 0;
	camera_cell = Vector3i(INT32_MIN, INT32_MIN, INT32_MIN);
	last_record_base = 0;
	last_tlas_count = 0;
	stats = Stats();
}

void NextGIFarField::_update_tlas(const Vector3 &p_camera, float p_near_distance, float p_far_distance, LocalVector<RendererRD::NextGIInstanceRecord> &r_records, LocalVector<RendererRD::NextGISurfaceRecord> &r_surfaces) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || !tlas.is_valid()) {
		tlas_ready = false;
		return;
	}
	camera = p_camera;
	const Vector3i current_cell = cell_coordinate(p_camera);
	if (current_cell != camera_cell) {
		camera_cell = current_cell;
		tlas_dirty = true;
	}
	for (KeyValue<uint64_t, Cell> &entry : cells) {
		Cell &cell = entry.value;
		if (cell.blas.is_valid() && !cell.compacted && rd->blas_is_compacted(cell.blas)) {
			proxy_bytes -= MIN(proxy_bytes, cell.bytes);
			cell.bytes = cell.geometry_bytes + rd->acceleration_structure_get_size_bytes(cell.blas);
			proxy_bytes += cell.bytes;
			cell.compacted = true;
			tlas_dirty = true;
		}
	}
	if (proxy_bytes > MAX_PROXY_BYTES) {
		budget_exceeded = true;
		WARN_PRINT("NextGI: far field dropped because its 128 MiB proxy budget was exceeded.");
		for (KeyValue<uint64_t, Cell> &entry : cells) {
			_release_cell(entry.value);
		}
		cells.clear();
		tlas_ready = false;
		stats.budget_exceeded = true;
		return;
	}
	const uint32_t record_base = r_records.size();
	LocalVector<RD::AccelerationStructureInstance> instances;
	for (KeyValue<uint64_t, Cell> &entry : cells) {
		Cell &cell = entry.value;
		if (!cell.compacted || cell.mesh.is_null() || cell.instance == nullptr || !intersects_shell(cell.bounds, p_camera, p_near_distance, p_far_distance)) {
			continue;
		}
		if (NextGISRC::get_instance_card_set(cell.instance) == UINT32_MAX) {
			continue;
		}
		RD::AccelerationStructureInstance instance;
		if (owner->append_far_proxy(cell.mesh, cell.instance, cell_transform(cell.coordinate), r_records, r_surfaces, instance)) {
			instances.push_back(instance);
		}
	}
	if (instances.is_empty()) {
		tlas_ready = false;
		last_record_base = record_base;
		last_tlas_count = 0;
		stats.cell_count = 0;
		return;
	}
	if (tlas_dirty || record_base != last_record_base || instances.size() != last_tlas_count) {
		tlas_ready = rd->tlas_build(tlas, Span<RD::AccelerationStructureInstance>(instances.ptr(), instances.size())) == OK;
		if (tlas_ready) {
			stats.tlas_build_count++;
		}
		tlas_dirty = false;
		last_record_base = record_base;
		last_tlas_count = instances.size();
	}
	stats.cell_count = instances.size();
}

void NextGIFarField::capture_sources(LocalVector<CaptureSource> &r_sources) const {
	r_sources.clear();
	if (budget_exceeded) {
		return;
	}
	for (const KeyValue<uint64_t, Cell> &entry : cells) {
		const Cell &cell = entry.value;
		if (!cell.compacted || cell.instance == nullptr || cell.mesh.is_null()) {
			continue;
		}
		CaptureSource source;
		source.instance = cell.instance;
		source.mesh = cell.mesh;
		source.transform = cell_transform(cell.coordinate);
		source.angular_radius = RendererRD::nextgi_rt_angular_radius(cell.bounds.get_center().distance_to(camera), cell.bounds.size.length() * 0.5f);
		r_sources.push_back(source);
	}
}
