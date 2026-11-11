/**************************************************************************/
/*  next_gi_swrt_trace.cpp                                                */
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

#include "next_gi_swrt_trace.h"

#include "core/config/project_settings.h"
#include "core/math/face3.h"
#include "core/math/math_funcs.h"
#include "core/math/random_number_generator.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/effects/ss_effects.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_gather.h"
#include "servers/rendering/renderer_rd/environment/next_gi_irradiance_field.h"
#include "servers/rendering/renderer_rd/environment/next_gi_sdf.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_view.h"
#include "servers/rendering/renderer_rd/environment/next_gi_voxel_lighting.h"
#include "servers/rendering/renderer_rd/environment/next_gi_wrc.h"
#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_gather_sdf.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_src_relight_sdf.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_server_globals.h"

#include <cstddef>

using namespace RendererRD;

// GodotNext: ps_swrt_trace (#186)

namespace {

enum TraceVariant {
	VARIANT_GATHER,
	VARIANT_REFLECT,
	VARIANT_LITE_PROBE,
	VARIANT_FROXEL_MARK,
	VARIANT_FROXEL_CULL,
	VARIANT_MAX,
};

struct TraceParamsGPU {
	float inv_view_proj[16];
	float view_proj[16];
	float camera[4];
	float screen[4];
	float trace[4];
	float ambient[4];
	int32_t origins[24];
	float voxel_size[8];
	int32_t clipmap[16];
	float grid[4];
	float flags[4];
	float view_forward[4];
	uint32_t counts[4];
	float froxel[4];
	float froxel_depth[4];
	float depth_coefficients[4];
	float extra[4];
	float view_to_world[12];
};
static_assert(sizeof(TraceParamsGPU) == 560, "SWRT params must match SWRTParams.");

struct InstanceGPU {
	float inverse_row0[4];
	float inverse_row1[4];
	float inverse_row2[4];
	float world_min[4];
	float world_max[4];
	uint32_t flags[4];
	float axis_scale[4]; // NextGIGlobalSDF::instance_axis_scale().
};
static_assert(sizeof(InstanceGPU) == 112, "SWRT instances must match SWRTInstance.");

struct MeshInfoGPU {
	float bounds_voxel[4];
	int32_t dimensions[4];
	int32_t brick_grid[4];
	uint32_t offsets[4];
};
static_assert(sizeof(MeshInfoGPU) == 64, "SWRT mesh info must match SWRTMeshInfo.");

struct ShadowPushGPU {
	uint32_t request_count;
	uint32_t light_count;
	float rt_radius;
	uint32_t pad;
};

struct LiteProbePushGPU {
	uint32_t probe_base;
	uint32_t pad[3];
};

Mutex s_counter_mutex;
HashMap<uint64_t, NextGISWRTTrace *> s_counter_registry;
uint64_t s_counter_next_token = 1;

float _rt_radius() {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr || !settings->has_setting("rendering/global_illumination/nextgi/rt_scene_radius")) {
		return 200.0f;
	}
	const float radius = float(GLOBAL_GET("rendering/global_illumination/nextgi/rt_scene_radius"));
	return radius > 0.0f ? radius : 200.0f;
}

// The step budget: MAX_STEPS (#186 step 2, a start value) unless the debug setting overrides it.
uint32_t _max_steps() {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr || !settings->has_setting("rendering/global_illumination/nextgi/debug/swrt_trace_max_steps")) {
		return NextGISWRTTrace::MAX_STEPS;
	}
	const int steps = int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/swrt_trace_max_steps"));
	return steps > 0 ? uint32_t(steps) : NextGISWRTTrace::MAX_STEPS;
}

// debug/swrt_trace_pool_bricks: a pool size for testing SWRT_REDUCED, 0 for the default.
uint32_t _pool_override() {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr || !settings->has_setting("rendering/global_illumination/nextgi/debug/swrt_trace_pool_bricks")) {
		return 0;
	}
	return uint32_t(MAX(int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/swrt_trace_pool_bricks")), 0));
}

bool _readback_enabled() {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	return settings != nullptr && settings->has_setting("rendering/global_illumination/nextgi/debug/swrt_trace_readback") && bool(GLOBAL_GET("rendering/global_illumination/nextgi/debug/swrt_trace_readback"));
}

// The scene fields every kernel reads (levels, origins, step budget, pool, instance
// counts). The view fields (matrices, screen, grid) stay zero unless a view sets them.
void _fill_scene_fields(TraceParamsGPU &r_params, const NextGIGlobalSDFClipmaps &p_clipmaps, const NextGIGlobalSDFGPU *p_field, uint32_t p_levels, uint32_t p_steps, uint32_t p_instances, uint32_t p_two_sided, uint32_t p_capacity) {
	r_params.camera[3] = float(p_levels);
	r_params.trace[1] = float(p_steps);
	for (uint32_t level = 0; level < 6; level++) {
		const Vector3i origin = p_clipmaps.get_origin(level);
		r_params.origins[level * 4] = origin.x;
		r_params.origins[level * 4 + 1] = origin.y;
		r_params.origins[level * 4 + 2] = origin.z;
		// 2: a partial level, whose missing bricks the trace reads from a coarser level.
		const bool partial = p_field != nullptr && p_field->get_missing_bricks(level) > 0;
		r_params.origins[level * 4 + 3] = level < p_levels && p_clipmaps.is_level_valid(level) ? (partial ? 2 : 1) : 0;
		r_params.voxel_size[level] = NextGIGlobalSDF::level_voxel_size(level);
	}
	r_params.flags[3] = float(p_instances);
	r_params.view_forward[3] = float(p_two_sided);
	r_params.counts[0] = uint32_t(RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0);
	r_params.counts[3] = p_capacity;
	r_params.froxel_depth[2] = float(p_instances);
}

void _free_rid(RenderingDevice *p_rd, RID &r_rid) {
	if (r_rid.is_valid()) {
		p_rd->free_rid(r_rid);
		r_rid = RID();
	}
}

// Grows a storage buffer to at least p_bytes. Returns true when it was recreated.
bool _ensure_storage(RenderingDevice *p_rd, RID &r_buffer, uint32_t &r_capacity, uint32_t p_bytes) {
	const uint32_t bytes = MAX(p_bytes, 16u);
	if (r_buffer.is_valid() && r_capacity >= bytes) {
		return false;
	}
	_free_rid(p_rd, r_buffer);
	r_capacity = Math::next_power_of_2(bytes);
	r_buffer = p_rd->storage_buffer_create(r_capacity);
	return true;
}

Vector<Face3> _box_faces() {
	const Vector3 lo(-0.5f, -0.5f, -0.5f);
	const Vector3 hi(0.5f, 0.5f, 0.5f);
	const Vector3 corners[8] = {
		Vector3(lo.x, lo.y, lo.z), Vector3(hi.x, lo.y, lo.z), Vector3(hi.x, hi.y, lo.z), Vector3(lo.x, hi.y, lo.z),
		Vector3(lo.x, lo.y, hi.z), Vector3(hi.x, lo.y, hi.z), Vector3(hi.x, hi.y, hi.z), Vector3(lo.x, hi.y, hi.z)
	};
	const int quads[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 3, 7, 6, 2 }, { 0, 4, 7, 3 }, { 1, 2, 6, 5 } };
	Vector<Face3> faces;
	for (int quad = 0; quad < 6; quad++) {
		faces.push_back(Face3(corners[quads[quad][0]], corners[quads[quad][1]], corners[quads[quad][2]]));
		faces.push_back(Face3(corners[quads[quad][0]], corners[quads[quad][2]], corners[quads[quad][3]]));
	}
	return faces;
}

} // namespace

struct NextGISWRTTrace::GpuState {
	NextgiGatherSdfShaderRD gather_shader;
	NextgiSrcRelightSdfShaderRD shadow_shader;
	RID gather_version;
	RID shadow_version;
	RID pipelines[VARIANT_MAX];
	RID shadow_pipeline;
	RID nearest_sampler;
	RID linear_sampler;
	RID dummy_depth;
	RID dummy_color;
	RID dummy_buffer;
	RID counter_buffer;
	RID params_buffer;
	// The shadow kernel's own params: the scene fields only, so it never reads a view's.
	RID shadow_params_buffer;
	// GodotNext: ps_swrt_trace (#186). The siblings' set-1 params: the frame's scene fields, and
	// one view's.
	RID scene_params_buffer;
	RID view_params_buffer;
	RID instance_buffer;
	uint32_t instance_capacity = 0;
	RID two_sided_buffer;
	uint32_t two_sided_capacity = 0;
	RID mesh_info_buffer;
	RID mesh_page_buffer;
	RID mesh_brick_buffer;
	uint32_t detail_info_count = 0;
	RID froxel_buffer;
	uint32_t froxel_capacity = 0;
	uint32_t ray_capacity[RAY_KIND_MAX] = {};
	RID requests_buffer;
	uint32_t requests_capacity = 0;
	RID results_buffer;
	uint32_t results_capacity = 0;
	RID lights_buffer;
	uint32_t lights_capacity = 0;
	bool ready = false;
};

uint64_t NextGISWRTTrace::last_benchmark_usec = 0;
uint32_t NextGISWRTTrace::last_fixture_checked = 0;
uint32_t NextGISWRTTrace::last_fixture_agreed = 0;

uint32_t NextGISWRTTrace::InstanceKeyHasher::hash(const InstanceKey &p_key) {
	uint32_t h = hash_murmur3_one_64(uint64_t(uintptr_t(p_key.geometry)));
	return hash_fmix32(hash_murmur3_one_32(p_key.element, h));
}

uint32_t NextGISWRTTrace::pool_bricks_for_levels(uint32_t p_levels) {
	const uint32_t levels = CLAMP(p_levels, 1u, NextGIGlobalSDF::BASE_LEVELS);
	return NextGIGlobalSDFGPU::DEFAULT_POOL_BRICKS / NextGIGlobalSDF::BASE_LEVELS * levels + (levels == NextGIGlobalSDF::BASE_LEVELS ? NextGIGlobalSDFGPU::DEFAULT_POOL_BRICKS % NextGIGlobalSDF::BASE_LEVELS : 0u);
}

Vector2i NextGISWRTTrace::ray_grid(const Vector2i &p_internal_size, uint32_t p_cells_per_axis) {
	const int cells = int(MAX(p_cells_per_axis, 1u));
	return Vector2i(MIN(cells, MAX(p_internal_size.x, 1)), MIN(cells, MAX(p_internal_size.y, 1)));
}

Vector3i NextGISWRTTrace::froxel_cells(const Vector2i &p_internal_size) {
	const int tile = int(FROXEL_TILE_PIXELS);
	return Vector3i(MAX((p_internal_size.x + tile - 1) / tile, 1), MAX((p_internal_size.y + tile - 1) / tile, 1), int(FROXEL_SLICES));
}

NextGISWRTTrace::NextGISWRTTrace() {
}

NextGISWRTTrace::~NextGISWRTTrace() {
	{
		MutexLock lock(s_counter_mutex);
		if (counter_token != 0) {
			s_counter_registry.erase(counter_token);
		}
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (gpu != nullptr && rd != nullptr) {
		RID *rids[] = { &gpu->shadow_pipeline, &gpu->nearest_sampler, &gpu->linear_sampler, &gpu->dummy_depth, &gpu->dummy_color, &gpu->dummy_buffer, &gpu->counter_buffer, &gpu->params_buffer, &gpu->shadow_params_buffer, &gpu->scene_params_buffer, &gpu->view_params_buffer, &gpu->instance_buffer, &gpu->two_sided_buffer, &gpu->mesh_info_buffer, &gpu->mesh_page_buffer, &gpu->mesh_brick_buffer, &gpu->froxel_buffer, &gpu->requests_buffer, &gpu->results_buffer, &gpu->lights_buffer };
		for (RID *rid : rids) {
			_free_rid(rd, *rid);
		}
		for (RID &pipeline : gpu->pipelines) {
			_free_rid(rd, pipeline);
		}
		for (uint32_t kind = 0; kind < RAY_KIND_MAX; kind++) {
			_free_rid(rd, ray_buffers[kind]);
		}
		if (gpu->gather_version.is_valid()) {
			gpu->gather_shader.version_free(gpu->gather_version);
		}
		if (gpu->shadow_version.is_valid()) {
			gpu->shadow_shader.version_free(gpu->shadow_version);
		}
	}
	if (gpu != nullptr) {
		memdelete(gpu);
		gpu = nullptr;
	}
	if (field != nullptr) {
		memdelete(field);
		field = nullptr;
	}
}

bool NextGISWRTTrace::_ensure_gpu() {
	if (pipelines_failed) {
		return false;
	}
	if (gpu != nullptr && gpu->ready) {
		return true;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	if (gpu == nullptr) {
		gpu = memnew(GpuState);
	}
	if (!gpu->gather_version.is_valid()) {
		gpu->gather_shader.initialize(Vector<String>({ "\n#define NEXTGI_SWRT_GATHER\n", "\n#define NEXTGI_SWRT_REFLECT\n", "\n#define NEXTGI_SWRT_LITE_PROBE\n", "\n#define NEXTGI_SWRT_FROXEL_MARK\n", "\n#define NEXTGI_SWRT_FROXEL_CULL\n" }));
		gpu->gather_version = gpu->gather_shader.version_create(false);
		if (gpu->gather_version.is_valid()) {
			for (uint32_t variant = 0; variant < VARIANT_MAX; variant++) {
				const RID shader = gpu->gather_shader.version_get_shader(gpu->gather_version, variant);
				if (shader.is_valid()) {
					gpu->pipelines[variant] = rd->compute_pipeline_create(shader);
				}
			}
		}
	}
	if (!gpu->shadow_version.is_valid()) {
		gpu->shadow_shader.initialize(Vector<String>({ "" }));
		gpu->shadow_version = gpu->shadow_shader.version_create(false);
		if (gpu->shadow_version.is_valid()) {
			gpu->shadow_pipeline = rd->compute_pipeline_create(gpu->shadow_shader.version_get_shader(gpu->shadow_version, 0));
		}
	}
	bool pipelines_valid = gpu->shadow_pipeline.is_valid();
	for (const RID &pipeline : gpu->pipelines) {
		pipelines_valid = pipelines_valid && pipeline.is_valid();
	}
	if (!pipelines_valid) {
		pipelines_failed = true;
		ERR_PRINT("NextGI software ray tracing shaders failed to compile.");
		return false;
	}
	RD::SamplerState nearest;
	nearest.mag_filter = RD::SAMPLER_FILTER_NEAREST;
	nearest.min_filter = RD::SAMPLER_FILTER_NEAREST;
	nearest.mip_filter = RD::SAMPLER_FILTER_NEAREST;
	gpu->nearest_sampler = rd->sampler_create(nearest);
	RD::SamplerState linear;
	linear.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	linear.min_filter = RD::SAMPLER_FILTER_LINEAR;
	linear.mip_filter = RD::SAMPLER_FILTER_NEAREST;
	linear.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	gpu->linear_sampler = rd->sampler_create(linear);
	{
		RD::TextureFormat format;
		format.width = 1;
		format.height = 1;
		format.format = RD::DATA_FORMAT_R32_SFLOAT;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		const float zero = 0.0f;
		Vector<uint8_t> data;
		data.resize(sizeof(float));
		memcpy(data.ptrw(), &zero, sizeof(float));
		gpu->dummy_depth = rd->texture_create(format, RD::TextureView(), Vector<Vector<uint8_t>>({ data }));
	}
	{
		RD::TextureFormat format;
		format.width = 1;
		format.height = 1;
		format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		Vector<uint8_t> data;
		data.resize(4);
		data.fill(0);
		gpu->dummy_color = rd->texture_create(format, RD::TextureView(), Vector<Vector<uint8_t>>({ data }));
	}
	// Zeros also stand in for the WRC params: camera.w = 0 marks the cache invalid.
	Vector<uint8_t> zeros;
	zeros.resize(256);
	zeros.fill(0);
	gpu->dummy_buffer = rd->storage_buffer_create(zeros.size(), zeros);
	gpu->counter_buffer = rd->storage_buffer_create(sizeof(Counters));
	gpu->params_buffer = rd->storage_buffer_create(sizeof(TraceParamsGPU));
	gpu->shadow_params_buffer = rd->storage_buffer_create(sizeof(TraceParamsGPU));
	gpu->scene_params_buffer = rd->storage_buffer_create(sizeof(TraceParamsGPU));
	gpu->view_params_buffer = rd->storage_buffer_create(sizeof(TraceParamsGPU));
	gpu->ready = gpu->nearest_sampler.is_valid() && gpu->linear_sampler.is_valid() && gpu->dummy_depth.is_valid() && gpu->dummy_color.is_valid() && gpu->dummy_buffer.is_valid() && gpu->counter_buffer.is_valid() && gpu->params_buffer.is_valid() && gpu->shadow_params_buffer.is_valid() && gpu->scene_params_buffer.is_valid() && gpu->view_params_buffer.is_valid();
	if (!gpu->ready) {
		pipelines_failed = true;
		ERR_PRINT("NextGI software ray tracing failed to allocate its buffers.");
	}
	return gpu->ready;
}

// SWRT_REDUCED (#187 step 2): a pool that cannot be allocated is retried with
// room for one level fewer, down to MIN_LEVELS. The tier never switches.
bool NextGISWRTTrace::_ensure_field() {
	if (field != nullptr) {
		return true;
	}
	if (field_failed) {
		return false;
	}
	const uint32_t pool_override = benchmark ? 0 : _pool_override();
	const uint32_t levels_before = active_levels;
	while (true) {
		field = memnew(NextGIGlobalSDFGPU(pool_override > 0 ? pool_override : pool_bricks_for_levels(active_levels)));
		field->ensure_buffers();
		if (field->has_buffers()) {
			clipmaps.reset();
			_note_reduced(levels_before);
			return true;
		}
		memdelete(field);
		field = nullptr;
		if (active_levels <= MIN_LEVELS) {
			field_failed = true;
			ERR_PRINT(vformat("NextGI software ray tracing could not allocate the global SDF pool for %d levels.", active_levels));
			return false;
		}
		active_levels--;
		reduced = true;
	}
}

// Tells NextGI once per reduction, with the level count the tracer settled on.
void NextGISWRTTrace::_note_reduced(uint32_t p_levels_before) {
	if (active_levels != p_levels_before && !benchmark && NextGI::get_singleton() != nullptr) {
		NextGI::get_singleton()->note_swrt_reduced(active_levels);
	}
}

void NextGISWRTTrace::_collect_instances(RenderDataRD *p_render_data) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr) {
		return;
	}
	const uint64_t generation = mesh_storage->get_sdf_atlas().get_generation();
	if (generation != sdf_generation) {
		// A mesh SDF arrived or changed: every brick may differ. detail_mesh_order points
		// into field_cache, so it goes with it (_upload_instances() reads it in every mode).
		field_cache.clear();
		detail_mesh_order.clear();
		fed.clear();
		clipmaps.reset();
		field->free_all();
		sdf_generation = generation;
		instances_dirty = true;
		detail_meshes_dirty = true;
	}
	instances.clear();
	instance_card_sets.clear();
	two_sided.clear();
	const LocalVector<RendererSceneRender::NextGIRTInstance> *feed = p_render_data->sdfgi_update_data != nullptr ? p_render_data->sdfgi_update_data->nextgi_rt_instances : nullptr;
	HashMap<InstanceKey, FedInstance, InstanceKeyHasher> current;
	LocalVector<AABB> dirty;
	const bool src_ready = NextGISRC::get_singleton() != nullptr;
	if (feed != nullptr) {
		for (const RendererSceneRender::NextGIRTInstance &source : *feed) {
			// Dynamic MultiMeshes and particles are GPU sources, as on the RT scene's SDF table.
			if (source.gpu_source.is_valid() || source.geometry_instance == nullptr) {
				continue;
			}
			RenderGeometryInstanceBase *geometry = static_cast<RenderGeometryInstanceBase *>(source.geometry_instance);
			if (geometry->data == nullptr) {
				continue;
			}
			// No skinned occluders in SWRT (#186 out of scope; plan L3).
			if (geometry->mesh_instance.is_valid()) {
				continue;
			}
			const RID mesh = source.mesh_override.is_valid() ? source.mesh_override : geometry->data->base;
			if (!mesh.is_valid() || !mesh_storage->owns_mesh(mesh)) {
				continue;
			}
			const Transform3D transform = source.has_rt_transform ? source.rt_transform : geometry->transform;
			if (!(Math::abs(transform.basis.determinant()) > 1.0e-8f)) {
				continue;
			}
			const NextGISDFAtlas::MeshPages *pages = mesh_storage->get_sdf_atlas().get_pages(mesh);
			if (pages == nullptr || pages->field.is_null()) {
				continue;
			}
			MeshSDFData *data = field_cache.getptr(mesh);
			if (data == nullptr) {
				data = &field_cache.insert(mesh, pages->field->get_data())->value;
				detail_meshes_dirty = true;
			}
			if (!data->is_valid() || data->page_tables.is_empty() || data->brick_data.is_empty()) {
				continue;
			}
			NextGIGlobalSDF::Instance instance;
			instance.transform = transform;
			instance.field = data;
			instance.dynamic = mesh_storage->mesh_nextgi_rt_is_dynamic(mesh);
			instance.two_sided = pages->field->is_two_sided();
			if (instance.two_sided) {
				two_sided.push_back(instances.size());
			}
			instances.push_back(instance);
			instance_card_sets.push_back(src_ready ? NextGISRC::get_instance_card_set(source.geometry_instance) : UINT32_MAX);

			// Instances that moved, appeared or changed mesh dirty their old and new bounds.
			InstanceKey key;
			key.geometry = source.geometry_instance;
			key.element = source.element_index;
			FedInstance now;
			now.bounds = transform.xform(data->bounds);
			now.mesh = mesh;
			const FedInstance *before = fed.getptr(key);
			if (before == nullptr || before->mesh != mesh || !before->bounds.is_equal_approx(now.bounds)) {
				dirty.push_back(now.bounds);
				if (before != nullptr) {
					dirty.push_back(before->bounds);
				}
				instances_dirty = true;
			}
			current.insert(key, now);
		}
	}
	for (const KeyValue<InstanceKey, FedInstance> &entry : fed) {
		if (!current.has(entry.key)) {
			dirty.push_back(entry.value.bounds);
			instances_dirty = true;
		}
	}
	fed = current;
	clipmaps.add_dirty_volumes(dirty.ptr(), dirty.size());
}

void NextGISWRTTrace::_upload_detail_meshes() {
	if (!detail_meshes_dirty) {
		return;
	}
	detail_meshes_dirty = false;
	instances_dirty = true;
	RenderingDevice *rd = RenderingDevice::get_singleton();
	detail_mesh_order.clear();
	Vector<MeshInfoGPU> infos;
	PackedInt32Array pages;
	PackedByteArray bricks;
	for (KeyValue<RID, MeshSDFData> &entry : field_cache) {
		const MeshSDFData &data = entry.value;
		if (!data.is_valid()) {
			continue;
		}
		detail_mesh_order.push_back(&data);
		for (int mip = 0; mip < data.get_mip_count(); mip++) {
			MeshInfoGPU info = {};
			const float voxel = data.get_mip_voxel_size(mip);
			info.bounds_voxel[0] = data.bounds.position.x;
			info.bounds_voxel[1] = data.bounds.position.y;
			info.bounds_voxel[2] = data.bounds.position.z;
			info.bounds_voxel[3] = voxel;
			for (int axis = 0; axis < 3; axis++) {
				info.dimensions[axis] = data.mip_dimensions[mip][axis];
				info.brick_grid[axis] = mip < data.mip_brick_grid_dimensions.size() ? data.mip_brick_grid_dimensions[mip][axis] : 1;
			}
			info.offsets[0] = uint32_t(pages.size());
			info.offsets[1] = uint32_t(bricks.size() / 4);
			const float band = mip < data.mip_bands.size() ? data.mip_bands[mip] : voxel * 4.0f;
			memcpy(&info.offsets[3], &band, sizeof(float));
			if (mip < data.page_tables.size()) {
				pages.append_array(data.page_tables[mip]);
			}
			if (mip < data.brick_data.size()) {
				bricks.append_array(data.brick_data[mip]);
			}
			while (bricks.size() % 4 != 0) {
				bricks.push_back(0);
			}
			infos.push_back(info);
		}
	}
	_free_rid(rd, gpu->mesh_info_buffer);
	_free_rid(rd, gpu->mesh_page_buffer);
	_free_rid(rd, gpu->mesh_brick_buffer);
	gpu->detail_info_count = infos.size();
	if (infos.is_empty() || pages.is_empty() || bricks.is_empty()) {
		return;
	}
	gpu->mesh_info_buffer = rd->storage_buffer_create(infos.size() * sizeof(MeshInfoGPU), Span<uint8_t>((const uint8_t *)infos.ptr(), infos.size() * sizeof(MeshInfoGPU)));
	gpu->mesh_page_buffer = rd->storage_buffer_create(pages.size() * sizeof(int32_t), Span<uint8_t>((const uint8_t *)pages.ptr(), pages.size() * sizeof(int32_t)));
	gpu->mesh_brick_buffer = rd->storage_buffer_create(bricks.size(), Span<uint8_t>(bricks.ptr(), bricks.size()));
}

void NextGISWRTTrace::_upload_instances() {
	if (!instances_dirty) {
		return;
	}
	instances_dirty = false;
	RenderingDevice *rd = RenderingDevice::get_singleton();
	HashMap<const MeshSDFData *, uint32_t> first_info;
	uint32_t info = 0;
	for (const MeshSDFData *data : detail_mesh_order) {
		first_info.insert(data, info);
		info += uint32_t(data->get_mip_count());
	}
	Vector<InstanceGPU> gpu_instances;
	gpu_instances.resize(MAX(instances.size(), 1));
	memset(gpu_instances.ptrw(), 0, gpu_instances.size() * sizeof(InstanceGPU));
	for (uint32_t index = 0; index < uint32_t(instances.size()); index++) {
		const NextGIGlobalSDF::Instance &instance = instances[index];
		InstanceGPU &out = gpu_instances.write[index];
		const Transform3D inverse = instance.transform.affine_inverse();
		float *rows[3] = { out.inverse_row0, out.inverse_row1, out.inverse_row2 };
		for (int row = 0; row < 3; row++) {
			rows[row][0] = inverse.basis.rows[row].x;
			rows[row][1] = inverse.basis.rows[row].y;
			rows[row][2] = inverse.basis.rows[row].z;
			rows[row][3] = inverse.origin[row];
		}
		const AABB world = instance.transform.xform(instance.field->bounds);
		// Per-axis scale: the largest one let rays tunnel through non-uniformly scaled instances.
		const Vector4 axis_scale = NextGIGlobalSDF::instance_axis_scale(instance.transform.basis);
		for (int axis = 0; axis < 3; axis++) {
			out.world_min[axis] = world.position[axis];
			out.world_max[axis] = world.position[axis] + world.size[axis];
		}
		for (int axis = 0; axis < 4; axis++) {
			out.axis_scale[axis] = axis_scale[axis];
		}
		out.flags[0] = instance.two_sided ? 2u : 0u;
		const uint32_t *first = first_info.getptr(instance.field);
		out.flags[1] = first != nullptr ? *first : 0u;
		out.flags[2] = first != nullptr ? uint32_t(instance.field->get_mip_count()) : 0u;
		out.flags[3] = index < instance_card_sets.size() ? instance_card_sets[index] : UINT32_MAX;
	}
	const uint32_t instance_bytes = gpu_instances.size() * sizeof(InstanceGPU);
	_ensure_storage(rd, gpu->instance_buffer, gpu->instance_capacity, instance_bytes);
	rd->buffer_update(gpu->instance_buffer, 0, instance_bytes, gpu_instances.ptr());
	LocalVector<uint32_t> list;
	for (const uint32_t index : two_sided) {
		list.push_back(index);
	}
	if (list.is_empty()) {
		list.push_back(0);
	}
	_ensure_storage(rd, gpu->two_sided_buffer, gpu->two_sided_capacity, list.size() * sizeof(uint32_t));
	rd->buffer_update(gpu->two_sided_buffer, 0, list.size() * sizeof(uint32_t), list.ptr());
}

void NextGISWRTTrace::update_scene(RenderDataRD *p_render_data) {
	if (p_render_data == nullptr || p_render_data->scene_data == nullptr) {
		return;
	}
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings != nullptr && settings->has_setting("rendering/global_illumination/nextgi/software_ray_tracing_mode")) {
		mode = int(GLOBAL_GET("rendering/global_illumination/nextgi/software_ray_tracing_mode")) == MODE_DETAIL ? MODE_DETAIL : MODE_GLOBAL;
	}
	if (!_ensure_gpu() || !_ensure_field()) {
		return;
	}
	// GodotNext: ps_swrt_trace (#186). The frame block's "AS updates" slot (plan §21.2.4, line 1a).
	last_update_slot = NextGI::get_singleton() != nullptr ? NextGI::get_singleton()->get_active_slot_name() : String("none");
	_collect_instances(p_render_data);
	if (p_render_data->sdfgi_update_data != nullptr) {
		clipmaps.add_dirty_volumes(p_render_data->sdfgi_update_data->nextgi_dirty_volumes, p_render_data->sdfgi_update_data->nextgi_dirty_volume_count);
	}
	const Vector3 camera = p_render_data->scene_data->cam_transform.origin;
	const uint64_t frame = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0;
	NextGIGlobalSDFClipmaps::Stats stats = clipmaps.update(*field, instances, camera, frame, active_levels);
	// SWRT_REDUCED (#187 step 2): out of pool slots, drop the top level and rebuild the level that failed.
	const uint32_t levels_before = active_levels;
	while (stats.allocation_failed && active_levels > MIN_LEVELS) {
		active_levels = NextGIGlobalSDFClipmaps::drop_top_level(*field, clipmaps, active_levels, MIN_LEVELS);
		reduced = true;
		if (stats.failed_level < active_levels) {
			clipmaps.invalidate_level(stats.failed_level);
		}
		stats = clipmaps.update(*field, instances, camera, frame, active_levels);
	}
	_note_reduced(levels_before);
	if (stats.allocation_failed) {
		// At the floor the level stays resident with the bricks it got (NextGIGlobalSDFClipmaps
		// keeps it). The coarser levels were built first, and the trace reads the missing bricks
		// from them. They are rebuilt when a scroll or a dirty volume reaches them.
		WARN_PRINT_ONCE(vformat("NextGI software ray tracing: the global SDF pool is full at %d levels; level %d is partial, and the trace reads the bricks that did not fit from a coarser level.", active_levels, stats.failed_level));
	}
	if (mode == MODE_DETAIL) {
		_upload_detail_meshes();
	}
	_upload_instances();
}

void NextGISWRTTrace::_fill_params(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting, RID p_normal_roughness, bool p_screen_trace, bool p_wrc, bool p_readback, RID p_target) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	RenderSceneDataRD *scene_data = p_render_data->scene_data;
	TraceParamsGPU params = {};
	const Projection view_projection = NextGIView::view_projection(*scene_data);
	NextGIViewMath::store(view_projection.inverse(), params.inv_view_proj);
	NextGIViewMath::store(view_projection, params.view_proj);
	const Vector3 camera = scene_data->cam_transform.origin;
	_fill_scene_fields(params, clipmaps, field, active_levels, _max_steps(), instances.size(), two_sided.size(), field != nullptr ? field->get_capacity() : 0);
	params.camera[0] = camera.x;
	params.camera[1] = camera.y;
	params.camera[2] = camera.z;
	params.screen[0] = float(MAX(screen_size.x, 1));
	params.screen[1] = float(MAX(screen_size.y, 1));
	params.screen[2] = screen_size.x > 0 ? 1.0f : 0.0f;
	params.screen[3] = 0.0f;
	const float rt_radius = _rt_radius();
	params.trace[0] = rt_radius;
	params.trace[2] = mode == MODE_DETAIL ? 1.0f : 0.0f;
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	NextGI *next_gi = NextGI::get_singleton();
	float env_max_roughness = 0.4f;
	if (scene != nullptr && p_render_data->environment.is_valid()) {
		env_max_roughness = scene->environment_get_nextgi_max_roughness_to_trace(p_render_data->environment);
	}
	const NextGI::QualityParams &quality = NextGI::get_quality_params(next_gi != nullptr ? next_gi->get_quality_setting() : NextGI::QUALITY_HIGH);
	params.trace[3] = NextGI::nextgi_effective_max_roughness(quality.max_trace_roughness, env_max_roughness);
	Color ambient(0, 0, 0, 1);
	params.ambient[3] = 0.0f;
	if (scene != nullptr && p_render_data->environment.is_valid()) {
		ambient = scene->environment_get_ambient_light(p_render_data->environment) * scene->environment_get_ambient_light_energy(p_render_data->environment);
		const RID sky = scene->environment_get_sky(p_render_data->environment);
		SkyRD *sky_rd = scene->get_sky();
		if (sky.is_valid() && sky_rd != nullptr && sky_rd->sky_get_radiance_texture_2d(sky).is_valid()) {
			params.ambient[3] = 1.0f;
		}
	}
	params.ambient[0] = ambient.r;
	params.ambient[1] = ambient.g;
	params.ambient[2] = ambient.b;
	const bool voxel_valid = p_lighting != nullptr && p_lighting->get_radiance_buffer().is_valid() && p_lighting->get_weight_buffer().is_valid();
	if (voxel_valid) {
		NextGIVoxelLighting::ClipmapLevelGPU clipmap[NextGIVoxelLighting::LEVELS];
		p_lighting->copy_clipmap(clipmap, NextGIVoxelLighting::LEVELS);
		for (uint32_t level = 0; level < NextGIVoxelLighting::LEVELS; level++) {
			params.clipmap[level * 4] = clipmap[level].origin_x;
			params.clipmap[level * 4 + 1] = clipmap[level].origin_y;
			params.clipmap[level * 4 + 2] = clipmap[level].origin_z;
			params.clipmap[level * 4 + 3] = int32_t(clipmap[level].voxel_size_bits);
		}
	}
	params.grid[0] = float(MAX(grid.x, 1));
	params.grid[1] = float(MAX(grid.y, 1));
	params.grid[2] = float(MAX(screen_size.x, 1)) / float(MAX(grid.x, 1));
	params.grid[3] = float(MAX(screen_size.y, 1)) / float(MAX(grid.y, 1));
	params.flags[0] = voxel_valid ? 1.0f : 0.0f;
	params.flags[1] = p_screen_trace ? 1.0f : 0.0f;
	params.flags[2] = p_wrc ? 1.0f : 0.0f;
	const Vector3 forward = -scene_data->cam_transform.basis.get_column(2).normalized();
	params.view_forward[0] = forward.x;
	params.view_forward[1] = forward.y;
	params.view_forward[2] = forward.z;
	const Vector3i froxel = froxel_cells(screen_size);
	params.froxel[0] = float(froxel.x);
	params.froxel[1] = float(froxel.y);
	params.froxel[2] = float(froxel.z);
	params.froxel[3] = rt_radius;
	params.froxel_depth[0] = MAX(scene_data->z_near, 0.01f);
	params.froxel_depth[1] = MAX(MIN(rt_radius, scene_data->z_far), params.froxel_depth[0] * 2.0f);
	const Vector4 coefficients = NextGIViewMath::depth_coefficients(NextGIView::depth_projection(*scene_data));
	for (int i = 0; i < 4; i++) {
		params.depth_coefficients[i] = coefficients[i];
	}
	params.extra[0] = p_normal_roughness.is_valid() ? 1.0f : 0.0f;
	params.extra[1] = mode == MODE_DETAIL && gpu->mesh_info_buffer.is_valid() ? 1.0f : 0.0f;
	params.extra[2] = p_readback ? 1.0f : 0.0f;
	NextGIViewMath::store_basis(scene_data->cam_transform.basis, params.view_to_world);
	rd->buffer_update(p_target, 0, sizeof(params), &params);
}

void NextGISWRTTrace::_append_trace_uniforms(Vector<RD::Uniform> &r_uniforms, RID p_params, NextGIVoxelLighting *p_lighting, RID p_normal_roughness, bool p_view_inputs, bool p_screen_trace) const {
	_append_core_uniforms(r_uniforms, p_params, p_lighting, true);
	_append_view_uniforms(r_uniforms, p_normal_roughness, p_view_inputs, p_screen_trace);
}

void NextGISWRTTrace::_append_core_uniforms(Vector<RD::Uniform> &r_uniforms, RID p_params, NextGIVoxelLighting *p_lighting, bool p_counters) const {
	auto buffer = [&](uint32_t p_binding, RID p_rid) {
		r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, p_binding, p_rid.is_valid() ? p_rid : gpu->dummy_buffer));
	};
	if (p_counters) {
		buffer(0, gpu->counter_buffer);
	}
	buffer(1, p_params);
	buffer(2, field != nullptr ? field->get_pool_buffer() : RID());
	buffer(3, field != nullptr ? field->get_dynamic_pool_buffer() : RID());
	buffer(4, field != nullptr ? field->get_coverage_buffer() : RID());
	for (uint32_t level = 0; level < 6; level++) {
		buffer(5 + level, field != nullptr ? field->get_page_buffer(level) : RID());
	}
	const bool voxel_valid = p_lighting != nullptr && p_lighting->get_radiance_buffer().is_valid() && p_lighting->get_weight_buffer().is_valid();
	buffer(11, voxel_valid ? p_lighting->get_radiance_buffer() : RID());
	buffer(12, voxel_valid ? p_lighting->get_weight_buffer() : RID());
	buffer(21, gpu->instance_buffer);
	buffer(22, gpu->mesh_info_buffer);
	buffer(23, gpu->mesh_page_buffer);
	buffer(24, gpu->mesh_brick_buffer);
	buffer(25, gpu->froxel_buffer);
	buffer(26, gpu->two_sided_buffer);
}

void NextGISWRTTrace::_append_view_uniforms(Vector<RD::Uniform> &r_uniforms, RID p_normal_roughness, bool p_view_inputs, bool p_screen_trace) const {
	auto buffer = [&](uint32_t p_binding, RID p_rid) {
		r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, p_binding, p_rid.is_valid() ? p_rid : gpu->dummy_buffer));
	};
	auto texture = [&](uint32_t p_binding, RID p_sampler, RID p_texture, RID p_dummy) {
		RD::Uniform uniform;
		uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		uniform.binding = p_binding;
		uniform.append_id(p_sampler);
		uniform.append_id(p_texture.is_valid() ? p_texture : p_dummy);
		r_uniforms.push_back(uniform);
	};
	// Without view inputs (the shadow kernel) every screen, sky and WRC input is a dummy,
	// so no RID of an earlier view (freed on a resize) is bound.
	const bool screen = p_view_inputs && p_screen_trace;
	texture(13, gpu->nearest_sampler, p_view_inputs ? screen_depth : RID(), gpu->dummy_depth);
	texture(14, gpu->nearest_sampler, p_view_inputs ? p_normal_roughness : RID(), gpu->dummy_color);
	// The Hi-Z pyramid holds depths: nearest, as the hardware gather samples it.
	texture(15, gpu->nearest_sampler, screen ? screen_hiz : RID(), gpu->dummy_depth);
	texture(16, gpu->linear_sampler, screen ? screen_last_frame : RID(), gpu->dummy_color);
	texture(17, gpu->linear_sampler, p_view_inputs ? sky_radiance : RID(), gpu->dummy_color);
	texture(18, gpu->linear_sampler, p_view_inputs ? wrc_radiance : RID(), gpu->dummy_color);
	texture(19, gpu->linear_sampler, p_view_inputs ? wrc_visibility : RID(), gpu->dummy_color);
	buffer(20, p_view_inputs ? wrc_params : RID());
}

// GodotNext: ps_swrt_trace (#186). The production siblings' set 1.
bool NextGISWRTTrace::is_ready() const {
	return gpu != nullptr && gpu->ready && field != nullptr;
}

bool NextGISWRTTrace::readback_enabled() {
	return _readback_enabled();
}

bool NextGISWRTTrace::is_detail_active() const {
	return mode == MODE_DETAIL && gpu != nullptr && gpu->mesh_info_buffer.is_valid() && !instances.is_empty();
}

void NextGISWRTTrace::prepare_scene_params(NextGIVoxelLighting *p_lighting) {
	if (!is_ready()) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	TraceParamsGPU params = {};
	_fill_scene_fields(params, clipmaps, field, active_levels, _max_steps(), instances.size(), two_sided.size(), field->get_capacity());
	params.trace[0] = _rt_radius();
	const bool voxel_valid = p_lighting != nullptr && p_lighting->get_radiance_buffer().is_valid() && p_lighting->get_weight_buffer().is_valid();
	if (voxel_valid) {
		NextGIVoxelLighting::ClipmapLevelGPU clipmap[NextGIVoxelLighting::LEVELS];
		p_lighting->copy_clipmap(clipmap, NextGIVoxelLighting::LEVELS);
		for (uint32_t level = 0; level < NextGIVoxelLighting::LEVELS; level++) {
			params.clipmap[level * 4] = clipmap[level].origin_x;
			params.clipmap[level * 4 + 1] = clipmap[level].origin_y;
			params.clipmap[level * 4 + 2] = clipmap[level].origin_z;
			params.clipmap[level * 4 + 3] = int32_t(clipmap[level].voxel_size_bits);
		}
	}
	params.flags[0] = voxel_valid ? 1.0f : 0.0f;
	rd->buffer_update(gpu->scene_params_buffer, 0, sizeof(params), &params);
}

void NextGISWRTTrace::prepare_view_params(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting) {
	if (!is_ready() || p_render_data == nullptr || p_render_data->scene_data == nullptr) {
		return;
	}
	Ref<RenderSceneBuffersRD> buffers = p_render_data->render_buffers;
	if (buffers.is_null()) {
		return;
	}
	// The siblings bind their own depth, screen-trace, sky and WRC inputs; the view params carry
	// this view's matrices, its internal size and the detail froxel grid. The froxel mark runs
	// over the gather's screen probes (one per 16 px tile).
	screen_size = buffers->get_internal_size();
	screen_depth = buffers->get_depth_texture();
	// Only the depth is a view input of the froxel mark; no earlier view's RID stays bound.
	screen_hiz = RID();
	screen_last_frame = RID();
	sky_radiance = RID();
	wrc_radiance = RID();
	wrc_visibility = RID();
	wrc_params = RID();
	grid = Vector2i(int(nextgi_gather_probe_count_axis(uint32_t(MAX(screen_size.x, 1)))), int(nextgi_gather_probe_count_axis(uint32_t(MAX(screen_size.y, 1)))));
	_fill_params(p_render_data, p_lighting, RID(), false, false, false, gpu->view_params_buffer);
}

RID NextGISWRTTrace::create_trace_uniform_set(RID p_shader, uint32_t p_set, bool p_view, NextGIVoxelLighting *p_lighting) const {
	if (!is_ready() || !p_shader.is_valid()) {
		return RID();
	}
	Vector<RD::Uniform> uniforms;
	_append_core_uniforms(uniforms, p_view ? gpu->view_params_buffer : gpu->scene_params_buffer, p_lighting, false);
	return RenderingDevice::get_singleton()->uniform_set_create(uniforms, p_shader, p_set);
}

void NextGISWRTTrace::build_detail_froxels(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting) {
	if (!is_detail_active() || p_render_data == nullptr || !screen_depth.is_valid()) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const Vector3i froxel = froxel_cells(screen_size);
	const uint32_t froxel_count = uint32_t(froxel.x * froxel.y * froxel.z);
	if (_ensure_storage(rd, gpu->froxel_buffer, gpu->froxel_capacity, froxel_count * (FROXEL_CAP + 1) * sizeof(uint32_t))) {
		// A new buffer: the caller's set 1 is created after this call, so it binds the new one.
	}
	rd->buffer_clear(gpu->froxel_buffer, 0, gpu->froxel_capacity);
	Vector<RD::Uniform> common;
	_append_core_uniforms(common, gpu->view_params_buffer, p_lighting, true);
	_append_view_uniforms(common, RID(), true, false);
	common.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, gpu->dummy_buffer));
	common.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, gpu->dummy_buffer));
	common.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, gpu->dummy_buffer));
	const RID mark_set = rd->uniform_set_create(common, gpu->gather_shader.version_get_shader(gpu->gather_version, VARIANT_FROXEL_MARK), 0);
	const RID cull_set = rd->uniform_set_create(common, gpu->gather_shader.version_get_shader(gpu->gather_version, VARIANT_FROXEL_CULL), 0);
	if (mark_set.is_valid() && cull_set.is_valid()) {
		rd->draw_command_begin_label("NextGI SDF Detail Froxels");
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, gpu->pipelines[VARIANT_FROXEL_MARK]);
		rd->compute_list_bind_uniform_set(list, mark_set, 0);
		rd->compute_list_dispatch(list, (uint32_t(grid.x) + 7u) / 8u, (uint32_t(grid.y) + 7u) / 8u, 1);
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, gpu->pipelines[VARIANT_FROXEL_CULL]);
		rd->compute_list_bind_uniform_set(list, cull_set, 0);
		const uint32_t cell_groups = (froxel_count + 7u) / 8u;
		const uint32_t groups_z = (cell_groups + 1023u) / 1024u;
		rd->compute_list_dispatch(list, (uint32_t(instances.size()) + 7u) / 8u, MIN(cell_groups, 1024u), groups_z);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}
	if (mark_set.is_valid()) {
		rd->free_rid(mark_set);
	}
	if (cull_set.is_valid()) {
		rd->free_rid(cull_set);
	}
}

void NextGISWRTTrace::trace_lite_probes(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting) {
	if (p_render_data == nullptr || p_render_data->scene_data == nullptr || gpu == nullptr || !gpu->ready || field == nullptr) {
		return;
	}
	NextGI *next_gi = NextGI::get_singleton();
	NextGIIrradianceField *lite_field = next_gi != nullptr ? next_gi->ensure_irradiance_field() : nullptr;
	if (lite_field == nullptr || !lite_field->ensure_probe_buffer()) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const RID probe_buffer = lite_field->get_probe_buffer();
	const uint32_t probe_base = lite_field->consume_swrt_probe_base();
	screen_size = Vector2i();
	screen_depth = RID();
	_bind_wrc(next_gi);
	_fill_params(p_render_data, p_lighting, RID(), false, wrc_params.is_valid(), false, gpu->params_buffer);
	Vector<RD::Uniform> uniforms;
	_append_trace_uniforms(uniforms, gpu->params_buffer, p_lighting, RID(), true, false);
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, gpu->dummy_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, probe_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, gpu->dummy_buffer));
	const RID set = rd->uniform_set_create(uniforms, gpu->gather_shader.version_get_shader(gpu->gather_version, VARIANT_LITE_PROBE), 0);
	if (!set.is_valid()) {
		return;
	}
	RENDER_TIMESTAMP("NextGI SDF Lite Probes");
	LiteProbePushGPU push = { probe_base, { 0u, 0u, 0u } };
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, gpu->pipelines[VARIANT_LITE_PROBE]);
	rd->compute_list_bind_uniform_set(list, set, 0);
	rd->compute_list_set_push_constant(list, &push, sizeof(push));
	rd->compute_list_dispatch(list, NextGI::LITE_PROBES_PER_FRAME, 1, 1);
	rd->compute_list_end();
	rd->free_rid(set);
	next_gi->note_lite_probe_traces(NextGI::LITE_PROBES_PER_FRAME);
}

void NextGISWRTTrace::_bind_wrc(NextGI *p_next_gi) {
	wrc_radiance = RID();
	wrc_visibility = RID();
	wrc_params = RID();
	NextGIWRC *wrc = p_next_gi != nullptr ? p_next_gi->get_wrc() : nullptr;
	if (wrc != nullptr && wrc->is_ready() && wrc->get_radiance().is_valid() && wrc->get_visibility().is_valid() && wrc->get_params_buffer().is_valid()) {
		wrc_radiance = wrc->get_radiance();
		wrc_visibility = wrc->get_visibility();
		wrc_params = wrc->get_params_buffer();
	}
}

void NextGISWRTTrace::trace_view(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting, RID p_normal_roughness) {
	if (p_render_data == nullptr || p_render_data->scene_data == nullptr || gpu == nullptr || !gpu->ready || field == nullptr) {
		return;
	}
	Ref<RenderSceneBuffersRD> buffers = p_render_data->render_buffers;
	if (buffers.is_null() || !buffers->get_depth_texture().is_valid()) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	NextGI *next_gi = NextGI::get_singleton();
	screen_size = buffers->get_internal_size();
	screen_depth = buffers->get_depth_texture();
	grid = ray_grid(screen_size);
	const uint32_t cells = uint32_t(grid.x * grid.y);
	for (uint32_t kind = 0; kind < RAY_KIND_MAX; kind++) {
		_ensure_storage(rd, ray_buffers[kind], gpu->ray_capacity[kind], cells * sizeof(RayResult));
	}
	// The Hi-Z pyramid and the last frame, as the hardware screen trace reads them.
	screen_hiz = RID();
	screen_last_frame = RID();
	uint32_t hiz_mips = 0;
	if (buffers->has_texture(RB_SCOPE_SSR, RB_HIZ) && buffers->has_texture(RB_SCOPE_SSLF, RB_LAST_FRAME)) {
		const RD::TextureFormat hiz_format = rd->texture_get_format(buffers->get_texture(RB_SCOPE_SSR, RB_HIZ));
		const RD::TextureFormat color_format = rd->texture_get_format(buffers->get_texture(RB_SCOPE_SSLF, RB_LAST_FRAME));
		screen_hiz = buffers->get_texture_slice(RB_SCOPE_SSR, RB_HIZ, 0, 0, 1, hiz_format.mipmaps);
		screen_last_frame = buffers->get_texture_slice(RB_SCOPE_SSLF, RB_LAST_FRAME, 0, 0, 1, color_format.mipmaps);
		hiz_mips = hiz_format.mipmaps;
	}
	const bool screen_trace = screen_hiz.is_valid() && screen_last_frame.is_valid();
	sky_radiance = RID();
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	if (scene != nullptr && p_render_data->environment.is_valid()) {
		const RID sky = scene->environment_get_sky(p_render_data->environment);
		if (sky.is_valid() && scene->get_sky() != nullptr) {
			sky_radiance = scene->get_sky()->sky_get_radiance_texture_2d(sky);
		}
	}
	_bind_wrc(next_gi);
	const bool readback = _readback_enabled();
	// GodotNext: ps_swrt_trace (#186). This is the verification kernel (readback runs); the frame
	// block's software gather does the production rays with the WRC handoff. Since the software
	// tier has its own WRC producer, the verification rays keep the full trace range (no handoff,
	// the sky on a miss), so the ray-by-ray check covers the trace beyond the 1.5 m handoff.
	const bool verification_wrc = false;
	_fill_params(p_render_data, p_lighting, p_normal_roughness, screen_trace, verification_wrc, readback, gpu->params_buffer);
	if (screen_trace) {
		// The last mip index, as the hardware gather passes it.
		const float max_mip = float(hiz_mips > 0 ? hiz_mips - 1 : 0);
		rd->buffer_update(gpu->params_buffer, offsetof(TraceParamsGPU, screen) + 3 * sizeof(float), sizeof(float), &max_mip);
	}
	const Counters zero;
	rd->buffer_update(gpu->counter_buffer, 0, sizeof(zero), &zero);
	if (readback) {
		_ensure_storage(rd, gpu->requests_buffer, gpu->requests_capacity, cells * sizeof(ShadowRequest));
		_ensure_storage(rd, gpu->results_buffer, gpu->results_capacity, cells * sizeof(uint32_t));
	}

	const bool detail = mode == MODE_DETAIL && gpu->mesh_info_buffer.is_valid() && !instances.is_empty();
	const Vector3i froxel = froxel_cells(screen_size);
	const uint32_t froxel_count = uint32_t(froxel.x * froxel.y * froxel.z);
	if (detail) {
		_ensure_storage(rd, gpu->froxel_buffer, gpu->froxel_capacity, froxel_count * (FROXEL_CAP + 1) * sizeof(uint32_t));
		rd->buffer_clear(gpu->froxel_buffer, 0, gpu->froxel_capacity);
	}
	Vector<RD::Uniform> common;
	_append_trace_uniforms(common, gpu->params_buffer, p_lighting, p_normal_roughness, true, screen_trace);
	const uint32_t groups_x = (uint32_t(grid.x) + 7u) / 8u;
	const uint32_t groups_y = (uint32_t(grid.y) + 7u) / 8u;
	LocalVector<RID> sets;
	auto make_set = [&](TraceVariant p_variant, RID p_rays) -> RID {
		Vector<RD::Uniform> uniforms = common;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, p_rays.is_valid() ? p_rays : gpu->dummy_buffer));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, gpu->dummy_buffer));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, readback && gpu->requests_buffer.is_valid() ? gpu->requests_buffer : gpu->dummy_buffer));
		const RID set = rd->uniform_set_create(uniforms, gpu->gather_shader.version_get_shader(gpu->gather_version, p_variant), 0);
		if (set.is_valid()) {
			sets.push_back(set);
		}
		return set;
	};

	// Detail mode: the froxel object grid from this frame's screen-probe positions.
	if (detail) {
		const RID mark_set = make_set(VARIANT_FROXEL_MARK, RID());
		const RID cull_set = make_set(VARIANT_FROXEL_CULL, RID());
		if (mark_set.is_valid() && cull_set.is_valid()) {
			RENDER_TIMESTAMP("NextGI SDF Detail Froxels");
			RD::ComputeListID list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, gpu->pipelines[VARIANT_FROXEL_MARK]);
			rd->compute_list_bind_uniform_set(list, mark_set, 0);
			rd->compute_list_dispatch(list, groups_x, groups_y, 1);
			rd->compute_list_add_barrier(list);
			rd->compute_list_bind_compute_pipeline(list, gpu->pipelines[VARIANT_FROXEL_CULL]);
			rd->compute_list_bind_uniform_set(list, cull_set, 0);
			const uint32_t cell_groups = (froxel_count + 7u) / 8u;
			const uint32_t groups_z = (cell_groups + 1023u) / 1024u;
			rd->compute_list_dispatch(list, (uint32_t(instances.size()) + 7u) / 8u, MIN(cell_groups, 1024u), groups_z);
			rd->compute_list_end();
		}
	}

	RENDER_TIMESTAMP("NextGI SDF Trace");
	const RID gather_set = make_set(VARIANT_GATHER, ray_buffers[RAY_GATHER]);
	if (gather_set.is_valid()) {
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, gpu->pipelines[VARIANT_GATHER]);
		rd->compute_list_bind_uniform_set(list, gather_set, 0);
		rd->compute_list_dispatch(list, groups_x, groups_y, 1);
		rd->compute_list_end();
		// GodotNext: ps_swrt_trace (#186). The verification kernel: the frame block's gather
		// counts the production dispatches, so this one is not noted.
	}
	if (NextGI::is_reflections_enabled()) {
		const RID reflect_set = make_set(VARIANT_REFLECT, ray_buffers[RAY_REFLECT]);
		if (reflect_set.is_valid()) {
			RD::ComputeListID list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, gpu->pipelines[VARIANT_REFLECT]);
			rd->compute_list_bind_uniform_set(list, reflect_set, 0);
			rd->compute_list_dispatch(list, groups_x, groups_y, 1);
			rd->compute_list_end();
		}
	}
	readback_meta.clear();
	if (readback) {
		// Verification: the gather cells' surfaces toward the first directional light.
		NextGILight light = {};
		uint32_t light_count = 0;
		LightStorage *light_storage = LightStorage::get_singleton();
		const Vector<RID> *directional = p_render_data->sdfgi_update_data != nullptr ? p_render_data->sdfgi_update_data->directional_lights : nullptr;
		if (light_storage != nullptr && directional != nullptr) {
			for (const RID &instance : *directional) {
				if (!light_storage->owns_light_instance(instance)) {
					continue;
				}
				const Vector3 travel = -light_storage->light_instance_get_base_transform(instance).basis.get_column(Vector3::AXIS_Z).normalized();
				light.direction_type[0] = travel.x;
				light.direction_type[1] = travel.y;
				light.direction_type[2] = travel.z;
				light.direction_type[3] = 0.0f;
				light_count = 1;
				readback_meta["light_direction"] = travel;
				break;
			}
		}
		_ensure_storage(rd, gpu->lights_buffer, gpu->lights_capacity, sizeof(NextGILight));
		rd->buffer_update(gpu->lights_buffer, 0, sizeof(NextGILight), &light);
		trace_shadow_rays(gpu->requests_buffer, light_count > 0 ? cells : 0, gpu->lights_buffer, light_count, gpu->results_buffer, _rt_radius());
		readback_meta["camera"] = p_render_data->scene_data->cam_transform;
		readback_meta["grid"] = grid;
		readback_meta["screen"] = screen_size;
		readback_meta["light_count"] = light_count;
		readback_meta["screen_trace"] = screen_trace;
		readback_meta["wrc"] = verification_wrc;
		readback_meta["normal_roughness"] = p_normal_roughness.is_valid();
		readback_meta["levels"] = active_levels;
		readback_meta["max_steps"] = _max_steps();
		readback_meta["froxel_cells"] = detail ? froxel_count : 0u;
	}
	for (const RID &set : sets) {
		rd->free_rid(set);
	}
	_request_counters();
}

bool NextGISWRTTrace::trace_shadow_rays(RID p_requests, uint32_t p_count, RID p_lights, uint32_t p_light_count, RID p_results, float p_rt_radius) {
	if (p_count == 0 || !p_requests.is_valid() || !p_results.is_valid() || !p_lights.is_valid() || gpu == nullptr || !gpu->ready || field == nullptr) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	// The kernel reads only the scene fields, from its own buffer filled here from the
	// current clipmaps: it does not depend on a trace_view() of this frame.
	TraceParamsGPU params = {};
	// The microbenchmark's tracer keeps the default budget: the debug override is for the views.
	_fill_scene_fields(params, clipmaps, field, active_levels, benchmark ? MAX_STEPS : _max_steps(), instances.size(), two_sided.size(), field->get_capacity());
	rd->buffer_update(gpu->shadow_params_buffer, 0, sizeof(params), &params);
	Vector<RD::Uniform> uniforms;
	_append_trace_uniforms(uniforms, gpu->shadow_params_buffer, nullptr, RID(), false, false);
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, p_requests));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, p_results));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, p_lights));
	const RID set = rd->uniform_set_create(uniforms, gpu->shadow_shader.version_get_shader(gpu->shadow_version, 0), 0);
	if (!set.is_valid()) {
		return false;
	}
	if (!benchmark) {
		// The microbenchmark runs during start-up, before the renderer's timestamp capture exists.
		RENDER_TIMESTAMP("NextGI SDF Shadow");
	}
	ShadowPushGPU push = { p_count, p_light_count, p_rt_radius, 0u };
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, gpu->shadow_pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	rd->compute_list_set_push_constant(list, &push, sizeof(push));
	rd->compute_list_dispatch(list, (p_count + 63u) / 64u, 1, 1);
	rd->compute_list_end();
	rd->free_rid(set);
	return true;
}

void NextGISWRTTrace::_request_counters() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	uint64_t token = 0;
	{
		MutexLock lock(s_counter_mutex);
		if (counters_pending) {
			return;
		}
		counters_pending = true;
		token = s_counter_next_token++;
		counter_token = token;
		s_counter_registry.insert(token, this);
	}
	if (rd->buffer_get_data_async(gpu->counter_buffer, callable_mp_static(&NextGISWRTTrace::_counters_received).bind(token), 0, sizeof(Counters)) != OK) {
		MutexLock lock(s_counter_mutex);
		s_counter_registry.erase(token);
		counter_token = 0;
		counters_pending = false;
	}
}

void NextGISWRTTrace::_counters_received(const Vector<uint8_t> &p_data, uint64_t p_token) {
	MutexLock lock(s_counter_mutex);
	NextGISWRTTrace **trace = s_counter_registry.getptr(p_token);
	if (trace == nullptr) {
		return;
	}
	NextGISWRTTrace *self = *trace;
	s_counter_registry.erase(p_token);
	self->counter_token = 0;
	self->counters_pending = false;
	if (p_data.size() >= int(sizeof(Counters))) {
		memcpy(&self->counters, p_data.ptr(), sizeof(Counters));
	}
}

NextGISWRTTrace::Counters NextGISWRTTrace::get_counters() const {
	MutexLock lock(s_counter_mutex);
	return counters;
}

void NextGISWRTTrace::fill_info(Dictionary &r_info) const {
	const Counters snapshot = get_counters();
	r_info["swrt_trace_levels"] = active_levels;
	r_info["swrt_trace_instances"] = instances.size();
	r_info["swrt_trace_two_sided"] = two_sided.size();
	r_info["swrt_trace_used_bricks"] = field != nullptr ? field->get_used_bricks() : 0;
	r_info["swrt_trace_pool_bricks"] = field != nullptr ? field->get_capacity() : 0;
	r_info["swrt_trace_full_builds"] = clipmaps.get_full_builds();
	r_info["swrt_trace_grid"] = grid;
	// GodotNext: ps_swrt_trace (#186). The frame-block slot the global SDF was last updated in.
	r_info["swrt_update_slot"] = last_update_slot;
	r_info["swrt_gather_rays"] = snapshot.gather_rays;
	r_info["swrt_gather_hits"] = snapshot.gather_hits;
	r_info["swrt_reflect_rays"] = snapshot.reflect_rays;
	r_info["swrt_reflect_hits"] = snapshot.reflect_hits;
	r_info["swrt_shadow_traced"] = snapshot.shadow_rays;
	r_info["swrt_shadow_occluded"] = snapshot.shadow_occluded;
	r_info["swrt_exhausted"] = snapshot.exhausted;
	r_info["swrt_screen_hits"] = snapshot.screen_hits;
	r_info["swrt_wrc_samples"] = snapshot.wrc_samples;
	r_info["swrt_detail_hits"] = snapshot.detail_hits;
	r_info["swrt_skipped"] = snapshot.skipped;
	if (!_readback_enabled() || readback_meta.is_empty() || gpu == nullptr || !gpu->ready) {
		return;
	}
	// Verification readback: stalls for the last traced frame.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const uint32_t cells = uint32_t(grid.x * grid.y);
	Dictionary readback = readback_meta.duplicate();
	readback["gather"] = rd->buffer_get_data(ray_buffers[RAY_GATHER], 0, cells * sizeof(RayResult));
	if (NextGI::is_reflections_enabled()) {
		readback["reflect"] = rd->buffer_get_data(ray_buffers[RAY_REFLECT], 0, cells * sizeof(RayResult));
	}
	if (int(readback_meta.get("light_count", 0)) > 0) {
		readback["shadow"] = rd->buffer_get_data(gpu->results_buffer, 0, cells * sizeof(uint32_t));
	}
	// Detail mode: how many froxels the mark pass found, and how many instances the cull
	// found per marked froxel (more than FROXEL_CAP means the list is incomplete).
	const uint32_t froxel_cells_traced = uint32_t(readback_meta.get("froxel_cells", 0));
	if (froxel_cells_traced > 0 && gpu->froxel_buffer.is_valid()) {
		const Vector<uint8_t> froxels = rd->buffer_get_data(gpu->froxel_buffer, 0, froxel_cells_traced * (FROXEL_CAP + 1) * sizeof(uint32_t));
		uint32_t marked = 0;
		uint32_t listed = 0;
		uint32_t overflow = 0;
		uint32_t most = 0;
		if (froxels.size() == int(froxel_cells_traced * (FROXEL_CAP + 1) * sizeof(uint32_t))) {
			const uint32_t *words = reinterpret_cast<const uint32_t *>(froxels.ptr());
			for (uint32_t cell = 0; cell < froxel_cells_traced; cell++) {
				const uint32_t word = words[cell * (FROXEL_CAP + 1)];
				if ((word & 0x80000000u) == 0) {
					continue;
				}
				const uint32_t found = word & 0x7fffffffu;
				marked++;
				listed += found > 0 ? 1 : 0;
				overflow += found > FROXEL_CAP ? 1 : 0;
				most = MAX(most, found);
			}
		}
		readback["froxel_marked"] = marked;
		readback["froxel_listed"] = listed;
		readback["froxel_overflow"] = overflow;
		readback["froxel_most_found"] = most;
	}
	// The #187 auto-quality inputs: the GPU microbenchmark (run here once when
	// auto quality did not run it) and the driver's GPU-core count.
	if (last_benchmark_usec == 0) {
		uint64_t usec = 0;
		String failure;
		run_microbenchmark(usec, failure);
	}
	readback["benchmark_usec"] = last_benchmark_usec;
	readback["benchmark_fixture_checked"] = last_fixture_checked;
	readback["benchmark_fixture_agreed"] = last_fixture_agreed;
	readback["compute_units"] = rd->get_device_compute_units();
	readback["driver"] = rd->get_device_api_name();
	// Resident bricks per level: what each level takes of the pool (SWRT_REDUCED drops
	// levels from the top until the rest fit).
	PackedInt32Array level_bricks;
	for (uint32_t level = 0; field != nullptr && level < active_levels; level++) {
		int32_t resident = 0;
		for (const int32_t slot : field->get_page_table(level)) {
			resident += slot >= 0 ? 1 : 0;
		}
		level_bricks.push_back(resident);
	}
	readback["level_bricks"] = level_bricks;
	// The composite (GPU) against its CPU twin (NextGIGlobalSDF::composite_distance) on level 0,
	// and against every instance: a resident voxel must not read past an unlisted one.
	if (field != nullptr && clipmaps.is_level_valid(0)) {
		float max_error = 0.0f;
		uint32_t sign_mismatches = 0;
		uint32_t checked = 0;
		uint32_t overestimates = 0;
		if (field->validate_reference(instances, 0, clipmaps.get_origin(0), 4096, max_error, sign_mismatches, checked, &overestimates)) {
			readback["composite_checked"] = checked;
			readback["composite_max_error"] = max_error;
			readback["composite_sign_mismatches"] = sign_mismatches;
			readback["composite_overestimates"] = overestimates;
		}
	}
	// The #182 coverage words against the CPU rule, on the two finest resident levels (a
	// two-sided mesh just past level 0 only reaches it with distances above the brick cap).
	uint32_t coverage_checked = 0;
	uint32_t coverage_expected_set = 0;
	uint32_t coverage_mismatches = 0;
	bool coverage_valid = false;
	for (uint32_t level = 0; field != nullptr && level < MIN(active_levels, 2u); level++) {
		uint32_t checked = 0;
		uint32_t expected_set = 0;
		uint32_t mismatches = 0;
		if (clipmaps.is_level_valid(level) && field->validate_coverage(instances, level, clipmaps.get_origin(level), 256, 0.01f * NextGIGlobalSDF::level_voxel_size(level), checked, expected_set, mismatches)) {
			coverage_valid = true;
			coverage_checked += checked;
			coverage_expected_set += expected_set;
			coverage_mismatches += mismatches;
		}
	}
	if (coverage_valid) {
		readback["coverage_checked"] = coverage_checked;
		readback["coverage_expected_set"] = coverage_expected_set;
		readback["coverage_mismatches"] = coverage_mismatches;
	}
	r_info["swrt_trace_readback"] = readback;
}

bool NextGISWRTTrace::run_microbenchmark(uint64_t &r_usec, String &r_failure) {
	r_usec = 0;
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		r_failure = "no rendering device";
		return false;
	}
	NextGISWRTTrace bench;
	// Its pool is its own: an allocation failure here is not the tier's SWRT_REDUCED.
	bench.benchmark = true;
	if (!bench._ensure_gpu() || !bench._ensure_field()) {
		r_failure = "the software trace pipelines are unavailable";
		return false;
	}
	// The fixture: a 24 m room with a floor, four walls and a grid of boxes.
	MeshSDFSettings settings;
	settings.voxels_per_meter = 10.0f;
	settings.max_voxels_per_axis = 32;
	MeshSDFData box;
	if (MeshSDFBuilder::build(_box_faces(), settings, box) != OK || !box.is_valid()) {
		r_failure = "the fixture mesh SDF did not build";
		return false;
	}
	bench.field_cache.insert(RID(), box);
	const MeshSDFData *box_field = bench.field_cache.getptr(RID());
	auto add = [&](const Vector3 &p_origin, const Vector3 &p_scale) {
		NextGIGlobalSDF::Instance instance;
		instance.transform = Transform3D(Basis().scaled(p_scale), p_origin);
		instance.field = box_field;
		bench.instances.push_back(instance);
	};
	add(Vector3(0.0f, -0.1f, 0.0f), Vector3(24.0f, 0.2f, 24.0f));
	add(Vector3(12.0f, 3.0f, 0.0f), Vector3(0.2f, 6.0f, 24.0f));
	add(Vector3(-12.0f, 3.0f, 0.0f), Vector3(0.2f, 6.0f, 24.0f));
	add(Vector3(0.0f, 3.0f, 12.0f), Vector3(24.0f, 6.0f, 0.2f));
	add(Vector3(0.0f, 3.0f, -12.0f), Vector3(24.0f, 6.0f, 0.2f));
	for (int z = -2; z <= 2; z++) {
		for (int x = -2; x <= 2; x++) {
			add(Vector3(float(x) * 4.0f, 0.5f, float(z) * 4.0f), Vector3(1.0f, 1.0f, 1.0f));
		}
	}
	const Vector3 camera(0.0f, 1.5f, 0.0f);
	bench.clipmaps.update(*bench.field, bench.instances, camera, 0, bench.active_levels);
	bench._upload_instances();
	TraceParamsGPU params = {};
	_fill_scene_fields(params, bench.clipmaps, bench.field, bench.active_levels, MAX_STEPS, bench.instances.size(), bench.two_sided.size(), bench.field->get_capacity());
	NextGIViewMath::store(Projection(), params.inv_view_proj);
	NextGIViewMath::store(Projection(), params.view_proj);
	params.camera[0] = camera.x;
	params.camera[1] = camera.y;
	params.camera[2] = camera.z;
	params.trace[0] = 32.0f;
	params.ambient[0] = 0.1f;
	params.ambient[1] = 0.1f;
	params.ambient[2] = 0.1f;
	rd->buffer_update(bench.gpu->params_buffer, 0, sizeof(params), &params);
	// GodotNext: ps_lumen_lite (#183). The Lite probe layout: L0 and L1, four vec4 per probe.
	const RID probes = rd->storage_buffer_create(NextGI::LITE_PROBE_COUNT * NextGI::LITE_PROBE_VEC4S * 4 * sizeof(float));
	Vector<RD::Uniform> uniforms;
	bench._append_trace_uniforms(uniforms, bench.gpu->params_buffer, nullptr, RID(), false, false);
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, bench.gpu->dummy_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, probes));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, bench.gpu->dummy_buffer));
	const RID set = rd->uniform_set_create(uniforms, bench.gpu->gather_shader.version_get_shader(bench.gpu->gather_version, VARIANT_LITE_PROBE), 0);
	if (!set.is_valid()) {
		rd->free_rid(probes);
		r_failure = "the benchmark uniform set failed";
		return false;
	}
	// A High-size workload: 2048 probes x 64 rays, about one 1080p frame of High gather rays.
	const uint32_t workgroups = 2048;
	auto dispatch = [&](uint32_t p_groups) {
		LiteProbePushGPU push = { 0u, { 0u, 0u, 0u } };
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, bench.gpu->pipelines[VARIANT_LITE_PROBE]);
		rd->compute_list_bind_uniform_set(list, set, 0);
		rd->compute_list_set_push_constant(list, &push, sizeof(push));
		rd->compute_list_dispatch(list, p_groups, 1, 1);
		rd->compute_list_end();
	};
	// The warm-up pays for pipeline creation and the fixture upload. Each timing is a
	// submit, the dispatch and a blocking readback; the same round trip with one
	// workgroup is subtracted, so the result is the trace. The fastest of three runs
	// keeps a GPU that is still raising its clock from deciding.
	dispatch(1);
	rd->buffer_get_data(probes, 0, 16);
	Vector<uint8_t> result;
	auto fastest_of_three = [&](uint32_t p_groups) {
		uint64_t fastest = UINT64_MAX;
		for (int run = 0; run < 3; run++) {
			const uint64_t started = OS::get_singleton()->get_ticks_usec();
			dispatch(p_groups);
			result = rd->buffer_get_data(probes, 0, 16);
			fastest = MIN(fastest, OS::get_singleton()->get_ticks_usec() - started);
		}
		return fastest;
	};
	const uint64_t round_trip = fastest_of_three(1);
	const uint64_t traced = fastest_of_three(workgroups);
	r_usec = traced > round_trip ? traced - round_trip : uint64_t(1);
	last_benchmark_usec = r_usec;
	rd->free_rid(set);
	rd->free_rid(probes);
	if (result.size() != 16) {
		r_failure = "the benchmark readback failed";
		return false;
	}
	// The timing only counts for a trace that sees its room: segments between free points
	// of the fixture, traced with the SDF shadow kernel, against the fixture's own boxes.
	LocalVector<AABB> boxes;
	for (const NextGIGlobalSDF::Instance &instance : bench.instances) {
		boxes.push_back(instance.transform.xform(AABB(Vector3(-0.5f, -0.5f, -0.5f), Vector3(1.0f, 1.0f, 1.0f))));
	}
	LocalVector<ShadowRequest> requests;
	LocalVector<NextGILight> lights;
	LocalVector<uint8_t> expected_occluded;
	fixture_segments(boxes, FIXTURE_SEGMENTS, requests, lights, expected_occluded);
	const RID requests_buffer = rd->storage_buffer_create(requests.size() * sizeof(ShadowRequest), Span<uint8_t>((const uint8_t *)requests.ptr(), requests.size() * sizeof(ShadowRequest)));
	const RID lights_buffer = rd->storage_buffer_create(lights.size() * sizeof(NextGILight), Span<uint8_t>((const uint8_t *)lights.ptr(), lights.size() * sizeof(NextGILight)));
	const RID results_buffer = rd->storage_buffer_create(requests.size() * sizeof(uint32_t));
	uint32_t agreed = 0;
	if (bench.trace_shadow_rays(requests_buffer, requests.size(), lights_buffer, lights.size(), results_buffer, 2.0f * NextGIGlobalSDF::level_extent(0))) {
		const Vector<uint8_t> results = rd->buffer_get_data(results_buffer, 0, requests.size() * sizeof(uint32_t));
		if (results.size() == int(requests.size() * sizeof(uint32_t))) {
			const uint32_t *words = reinterpret_cast<const uint32_t *>(results.ptr());
			for (uint32_t index = 0; index < requests.size(); index++) {
				agreed += (words[index] == SHADOW_OCCLUDED) == (expected_occluded[index] != 0) ? 1 : 0;
			}
		}
	}
	rd->free_rid(requests_buffer);
	rd->free_rid(lights_buffer);
	rd->free_rid(results_buffer);
	last_fixture_checked = requests.size();
	last_fixture_agreed = agreed;
	if (float(agreed) < FIXTURE_MIN_AGREEMENT * float(requests.size())) {
		r_failure = vformat("the fixture trace agrees with the fixture's boxes on %d of %d segments", agreed, requests.size());
		return false;
	}
	return true;
}

void NextGISWRTTrace::fixture_segments(const LocalVector<AABB> &p_boxes, uint32_t p_per_class, LocalVector<ShadowRequest> &r_requests, LocalVector<NextGILight> &r_lights, LocalVector<uint8_t> &r_expected_occluded) {
	r_requests.clear();
	r_lights.clear();
	r_expected_occluded.clear();
	AABB extent;
	for (uint32_t index = 0; index < p_boxes.size(); index++) {
		extent = index == 0 ? p_boxes[index] : extent.merge(p_boxes[index]);
	}
	extent = extent.grow(2.0f);
	// Endpoints and lit segments keep FIXTURE_CLEARANCE from every box, so no ray starts in a
	// surface band and none grazes one; an occluded segment crosses a box's core.
	auto clear_of = [&](const Vector3 &p_point) {
		for (const AABB &box : p_boxes) {
			if (box.grow(FIXTURE_CLEARANCE).has_point(p_point)) {
				return false;
			}
		}
		return true;
	};
	RandomNumberGenerator rng;
	rng.set_seed(187);
	uint32_t counts[2] = {};
	for (uint32_t attempt = 0; attempt < 200000 && (counts[0] < p_per_class || counts[1] < p_per_class); attempt++) {
		const Vector3 from = extent.position + Vector3(rng.randf(), rng.randf(), rng.randf()) * extent.size;
		const Vector3 to = extent.position + Vector3(rng.randf(), rng.randf(), rng.randf()) * extent.size;
		const float length = from.distance_to(to);
		if (length < 1.0f || length > FIXTURE_MAX_LENGTH || !clear_of(from) || !clear_of(to)) {
			continue;
		}
		bool crosses = false;
		bool near = false;
		for (const AABB &box : p_boxes) {
			crosses = crosses || box.grow(-FIXTURE_CORE_INSET).intersects_segment(from, to);
			near = near || box.grow(FIXTURE_CLEARANCE).intersects_segment(from, to);
		}
		if (!crosses && near) {
			continue; // Grazes a box: ambiguous within the surface band.
		}
		const uint32_t kind = crosses ? 1 : 0;
		if (counts[kind] >= p_per_class) {
			continue;
		}
		counts[kind]++;
		const Vector3 direction = (to - from) / length;
		ShadowRequest request = {};
		request.position_offset[0] = from.x;
		request.position_offset[1] = from.y;
		request.position_offset[2] = from.z;
		request.position_offset[3] = 0.01f;
		request.normal_light[0] = direction.x;
		request.normal_light[1] = direction.y;
		request.normal_light[2] = direction.z;
		request.normal_light[3] = float(r_lights.size());
		r_requests.push_back(request);
		NextGILight light;
		light.position_inv_range[0] = to.x;
		light.position_inv_range[1] = to.y;
		light.position_inv_range[2] = to.z;
		light.direction_type[3] = 1.0f; // Positional: the ray ends at the light.
		r_lights.push_back(light);
		r_expected_occluded.push_back(crosses ? 1 : 0);
	}
}
