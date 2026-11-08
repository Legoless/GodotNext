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
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_irradiance_field.h"
#include "servers/rendering/renderer_rd/environment/next_gi_sdf.h"
#include "servers/rendering/renderer_rd/environment/next_gi_wrc.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_gather_sdf.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_src_relight_sdf.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

namespace {

constexpr float MISSING_DISTANCE = 1.0e20f;
constexpr uint32_t RAY_GRID = 64;

bool _missing(float p_distance) {
	return !(p_distance < MISSING_DISTANCE);
}

int _detail_mip(const MeshSDFData *p_field, float p_distance) {
	if (p_field == nullptr || p_field->get_mip_count() <= 0) {
		return 0;
	}
	const float finest = p_field->get_mip_voxel_size(0);
	const float limit = MAX(p_distance, finest);
	for (int mip = p_field->get_mip_count() - 1; mip >= 0; mip--) {
		if (p_field->get_mip_voxel_size(mip) <= limit) {
			return mip;
		}
	}
	return 0;
}

const NextGIGlobalSDF::Instance *_two_sided_instance(const NextGIGlobalSDF::Instance *p_instances, uint32_t p_count, const Vector3 &p_position, float p_distance) {
	if (p_instances == nullptr) {
		return nullptr;
	}
	for (uint32_t index = 0; index < p_count; index++) {
		if (!p_instances[index].two_sided || p_instances[index].field == nullptr) {
			continue;
		}
		const float sampled = NextGIGlobalSDF::sample_instance(p_instances[index], p_position);
		// Nearest-voxel compositing can sit up to half a voxel off the mesh SDF.
		if (!_missing(sampled) && Math::abs(sampled - p_distance) <= NextGIGlobalSDF::BASE_VOXEL_SIZE) {
			return &p_instances[index];
		}
	}
	return nullptr;
}

// An unsigned two-sided shell has no sign, and its gradient vanishes on the
// surface. The bounds center is inside a closed mesh, so the direction away
// from it is the outward normal a back-facing ray agrees with.
Vector3 _two_sided_outward(const Vector3 &p_position, const NextGIGlobalSDF::Instance &p_instance, const Vector3 &p_fallback) {
	if (p_instance.field == nullptr) {
		return p_fallback;
	}
	const Vector3 center = p_instance.transform.xform(p_instance.field->bounds.get_center());
	const Vector3 away = p_position - center;
	if (away.length_squared() <= 1.0e-8f) {
		return p_fallback;
	}
	return away.normalized();
}

Vector3 _field_normal(const NextGIGlobalSDF &p_field, uint32_t p_level, const Vector3i &p_origin, const Vector3 &p_position, const Vector3 &p_fallback) {
	const float voxel = NextGIGlobalSDF::level_voxel_size(p_level);
	const Vector3 axes[3] = { Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f) };
	float taps[6];
	for (int axis = 0; axis < 3; axis++) {
		taps[axis * 2] = p_field.sample(p_level, p_origin, p_position + axes[axis] * voxel);
		taps[axis * 2 + 1] = p_field.sample(p_level, p_origin, p_position - axes[axis] * voxel);
	}
	const Vector3 gradient = NextGIVoxelLighting::gradient_from_taps(taps, voxel);
	if (gradient.length_squared() <= 1.0e-8f) {
		return p_fallback;
	}
	return gradient.normalized();
}

struct MarchCounters {
	float t = 0.0f;
	uint32_t steps = 0;
};

NextGISWRTTrace::Hit _finish_miss(const MarchCounters &p_march) {
	NextGISWRTTrace::Hit hit;
	hit.steps = p_march.steps;
	return hit;
}

bool _march_global(const NextGIGlobalSDF &p_field, const Vector3i *p_origins, uint32_t p_level_count, const Vector3 &p_camera, const Vector3 &p_origin, const Vector3 &p_direction, float p_tmax, const NextGIGlobalSDF::Instance *p_instances, uint32_t p_instance_count, MarchCounters &r_march, NextGISWRTTrace::Hit &r_hit) {
	const uint32_t levels = MIN(p_level_count, NextGIGlobalSDF::MAX_LEVELS);
	if (levels == 0 || p_origins == nullptr) {
		return false;
	}
	while (r_march.steps < NextGISWRTTrace::MAX_STEPS && r_march.t < p_tmax) {
		const Vector3 position = p_origin + p_direction * r_march.t;
		const uint32_t level = NextGIGlobalSDF::level_for_distance(p_camera.distance_to(position), levels);
		const float voxel = NextGIGlobalSDF::level_voxel_size(level);
		const float distance = p_field.sample(level, p_origins[level], position);
		r_march.steps++;
		// Nearest-voxel samples sit off the isosurface, and an unsigned shell's
		// stored minimum can be just over half a voxel. A voxel that contains
		// the surface is still a hit.
		if (!_missing(distance) && Math::abs(distance) <= voxel) {
			const NextGIGlobalSDF::Instance *two_sided = _two_sided_instance(p_instances, p_instance_count, position, distance);
			Vector3 normal = _field_normal(p_field, level, p_origins[level], position, -p_direction);
			if (two_sided != nullptr) {
				normal = _two_sided_outward(position, *two_sided, normal);
				if (p_direction.dot(normal) <= 0.0f) {
					r_march.t += voxel;
					continue;
				}
			}
			r_hit.hit = true;
			r_hit.t = r_march.t;
			r_hit.position = position;
			r_hit.normal = normal;
			r_hit.steps = r_march.steps;
			r_hit.detail = false;
			r_hit.instance_id = NextGISWRTTrace::NO_INSTANCE;
			return true;
		}
		const float advance = _missing(distance) ? voxel : MAX(Math::abs(distance), 0.25f * voxel);
		r_march.t += MIN(advance, MAX(p_tmax - r_march.t, 0.0f));
	}
	return false;
}

struct TraceParamsGPU {
	float inv_view_proj[16];
	float camera[4];
	float screen[4];
	float trace[4];
	float sky[4];
	float wrc[4];
	int32_t origins[24];
	float voxel_size[8];
	int32_t clipmap[16];
	float froxel[4];
	float light_dir[4];
	float grid[4];
	float extra[4];
};

static_assert(sizeof(TraceParamsGPU) == 400, "SWRT params must match the shader struct.");

struct MeshInfoGPU {
	float bounds_voxel[4];
	int32_t dimensions_flags[4];
	int32_t brick_grid[4];
	uint32_t offsets[4];
};
static_assert(sizeof(MeshInfoGPU) == 64, "SWRT mesh info must match the shader.");

struct InstanceGPU {
	float inverse_row0[4];
	float inverse_row1[4];
	float inverse_row2[4];
	float world_min[4];
	float world_max[4];
	uint32_t flags[4];
};
static_assert(sizeof(InstanceGPU) == 96, "SWRT instance must match the shader.");

struct Candidate {
	RID mesh;
	Transform3D transform;
	float distance_squared = 0.0f;
	bool in_front = false;
	bool two_sided = false;
};

struct CandidateOrder {
	bool operator()(const Candidate &p_a, const Candidate &p_b) const {
		return NextGISWRTTrace::instance_before(p_a.in_front, p_a.distance_squared, p_b.in_front, p_b.distance_squared);
	}
};

struct BrickCand {
	Vector3i brick;
	float distance_squared = 0.0f;
	bool in_front = false;
};

void _store_projection(const Projection &p_projection, float r_matrix[16]) {
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			r_matrix[column * 4 + row] = p_projection.columns[column][row];
		}
	}
}

} // namespace

struct NextGISWRTTrace::GpuState {
	NextgiGatherSdfShaderRD gather_shader;
	NextgiSrcRelightSdfShaderRD shadow_shader;
	RID gather_version;
	RID shadow_version;
	RID gather_pipeline;
	RID reflect_pipeline;
	RID probe_pipeline;
	RID shadow_pipeline;
	RID sampler;
	RID dummy_texture;
	RID dummy_buffer;
	RID counter_buffer;
	RID params_buffer;
	RID shade_buffer;
	RID froxel_buffer;
	bool ready = false;
	bool compile_reported = false;
};

Vector3 NextGISWRTTrace::offset_origin(const Vector3 &p_origin, const Vector3 &p_normal, float p_voxel_size) {
	Vector3 normal = p_normal;
	if (normal.length_squared() <= 1.0e-8f) {
		normal = Vector3(0.0f, 1.0f, 0.0f);
	} else {
		normal = normal.normalized();
	}
	return p_origin + normal * (SELF_OFFSET_VOXELS * MAX(p_voxel_size, 0.0f));
}

float NextGISWRTTrace::wrc_handoff(float p_probe_spacing) {
	return WRC_HANDOFF_SCALE * MAX(p_probe_spacing, 0.0f);
}

float NextGISWRTTrace::trace_limit(float p_distance_from_camera, float p_requested, bool p_reflect) {
	const float requested = MAX(p_requested, 0.0f);
	if (p_reflect) {
		return requested;
	}
	return MIN(requested, nextgi_wrc_handoff_distance(MAX(p_distance_from_camera, 0.0f)));
}

bool NextGISWRTTrace::instance_in_front(const Vector3 &p_camera, const Vector3 &p_view_forward, const Vector3 &p_origin) {
	return (p_origin - p_camera).dot(p_view_forward) > -1.0f;
}

bool NextGISWRTTrace::instance_before(bool p_a_in_front, float p_a_distance_squared, bool p_b_in_front, float p_b_distance_squared) {
	if (p_a_in_front != p_b_in_front) {
		return p_a_in_front;
	}
	return p_a_distance_squared < p_b_distance_squared;
}

uint32_t NextGISWRTTrace::froxel_index(const FroxelGrid &p_grid, const Vector3 &p_position) {
	const float cell = p_grid.cell > 0.0f ? p_grid.cell : (DETAIL_METERS / float(FROXEL_AXIS));
	const Vector3 local = (p_position - p_grid.origin) / cell;
	if (local.x < 0.0f || local.y < 0.0f || local.z < 0.0f || local.x >= float(FROXEL_AXIS) || local.y >= float(FROXEL_AXIS) || local.z >= float(FROXEL_AXIS)) {
		return NO_INSTANCE;
	}
	const uint32_t x = uint32_t(local.x);
	const uint32_t y = uint32_t(local.y);
	const uint32_t z = uint32_t(local.z);
	return x + FROXEL_AXIS * (y + FROXEL_AXIS * z);
}

void NextGISWRTTrace::build_froxel(const DetailInstance *p_instances, uint32_t p_count, const Vector3 &p_volume_origin, FroxelGrid &r_grid) {
	r_grid = FroxelGrid();
	r_grid.origin = p_volume_origin;
	r_grid.cell = DETAIL_METERS / float(FROXEL_AXIS);
	if (p_instances == nullptr) {
		return;
	}
	for (uint32_t index = 0; index < p_count; index++) {
		const NextGIGlobalSDF::Instance &instance = p_instances[index].instance;
		if (instance.field == nullptr || !instance.field->is_valid()) {
			continue;
		}
		const AABB world = instance.transform.xform(instance.field->bounds);
		const Vector3 min_cell = (world.position - r_grid.origin) / r_grid.cell;
		const Vector3 max_cell = (world.position + world.size - r_grid.origin) / r_grid.cell;
		const int x0 = CLAMP(int(Math::floor(min_cell.x)), 0, int(FROXEL_AXIS) - 1);
		const int y0 = CLAMP(int(Math::floor(min_cell.y)), 0, int(FROXEL_AXIS) - 1);
		const int z0 = CLAMP(int(Math::floor(min_cell.z)), 0, int(FROXEL_AXIS) - 1);
		const int x1 = CLAMP(int(Math::floor(max_cell.x)), 0, int(FROXEL_AXIS) - 1);
		const int y1 = CLAMP(int(Math::floor(max_cell.y)), 0, int(FROXEL_AXIS) - 1);
		const int z1 = CLAMP(int(Math::floor(max_cell.z)), 0, int(FROXEL_AXIS) - 1);
		if (x1 < x0 || y1 < y0 || z1 < z0) {
			continue;
		}
		for (int z = z0; z <= z1; z++) {
			for (int y = y0; y <= y1; y++) {
				for (int x = x0; x <= x1; x++) {
					const uint32_t cell = uint32_t(x) + FROXEL_AXIS * (uint32_t(y) + FROXEL_AXIS * uint32_t(z));
					if (r_grid.counts[cell] >= FROXEL_CAP) {
						continue;
					}
					r_grid.indices[cell * FROXEL_CAP + r_grid.counts[cell]] = index;
					r_grid.counts[cell]++;
				}
			}
		}
	}
}

NextGISWRTTrace::Hit NextGISWRTTrace::trace_global(const NextGIGlobalSDF &p_field, const Vector3i *p_origins, uint32_t p_level_count, const Vector3 &p_camera, const Vector3 &p_origin, const Vector3 &p_direction, float p_tmax, const NextGIGlobalSDF::Instance *p_instances, uint32_t p_instance_count) {
	Hit hit;
	if (p_direction.length_squared() <= 1.0e-8f || !(p_tmax > 0.0f)) {
		return hit;
	}
	MarchCounters march;
	const Vector3 direction = p_direction.normalized();
	if (_march_global(p_field, p_origins, p_level_count, p_camera, p_origin, direction, p_tmax, p_instances, p_instance_count, march, hit)) {
		return hit;
	}
	return _finish_miss(march);
}

uint64_t NextGISWRTTrace::probe_usec() {
	NextGIGlobalSDF field;
	Vector3i origins[NextGIGlobalSDF::BASE_LEVELS];
	const Vector3 camera;
	for (uint32_t level = 0; level < NextGIGlobalSDF::BASE_LEVELS; level++) {
		origins[level] = NextGIGlobalSDF::scrolled_origin(camera, level);
	}
	const uint64_t started = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	trace_global(field, origins, NextGIGlobalSDF::BASE_LEVELS, camera, Vector3(), Vector3(0.0f, 0.0f, -1.0f), 8.0f, nullptr, 0);
	if (OS::get_singleton() == nullptr) {
		return 1;
	}
	const uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - started;
	return elapsed == 0 ? 1 : elapsed;
}

NextGISWRTTrace::Hit NextGISWRTTrace::trace_detail(const NextGIGlobalSDF &p_field, const Vector3i *p_origins, uint32_t p_level_count, const Vector3 &p_camera, const Vector3 &p_origin, const Vector3 &p_direction, float p_tmax, const DetailInstance *p_instances, uint32_t p_instance_count, const FroxelGrid *p_froxel, const NextGIGlobalSDF::Instance *p_global_instances, uint32_t p_global_count) {
	Hit hit;
	if (p_direction.length_squared() <= 1.0e-8f || !(p_tmax > 0.0f)) {
		return hit;
	}
	const Vector3 direction = p_direction.normalized();
	MarchCounters march;
	const uint32_t instance_count = p_instances == nullptr ? 0 : p_instance_count;
	while (march.steps < MAX_STEPS && march.t < p_tmax && march.t < DETAIL_METERS) {
		const Vector3 position = p_origin + direction * march.t;
		const uint32_t *cell_indices = nullptr;
		uint32_t cell_count = 0;
		if (p_froxel == nullptr) {
			cell_count = instance_count;
		} else {
			const uint32_t cell = froxel_index(*p_froxel, position);
			if (cell < FROXEL_CELLS) {
				cell_indices = &p_froxel->indices[cell * FROXEL_CAP];
				cell_count = p_froxel->counts[cell];
			}
		}
		float best = MISSING_DISTANCE;
		uint32_t best_index = NO_INSTANCE;
		float best_voxel = 0.1f;
		for (uint32_t slot = 0; slot < cell_count; slot++) {
			const uint32_t index = p_froxel == nullptr ? slot : cell_indices[slot];
			if (index >= instance_count || p_instances[index].instance.field == nullptr) {
				continue;
			}
			const NextGIGlobalSDF::Instance &instance = p_instances[index].instance;
			const int mip = _detail_mip(instance.field, DETAIL_METERS - march.t);
			const Vector3 local = instance.transform.affine_inverse().xform(position);
			const Vector3 scale = instance.transform.basis.get_scale();
			const float world_scale = MAX(scale.x, MAX(scale.y, scale.z));
			const float sampled = instance.field->sample_distance(local, mip) * (world_scale > 0.0f ? world_scale : 1.0f);
			if (sampled < best) {
				best = sampled;
				best_index = index;
				best_voxel = instance.field->get_mip_voxel_size(mip) * (world_scale > 0.0f ? world_scale : 1.0f);
			}
		}
		march.steps++;
		if (!_missing(best) && Math::abs(best) <= 0.5f * best_voxel && best_index != NO_INSTANCE) {
			const bool two_sided = p_instances[best_index].instance.two_sided;
			const Vector3 normal = -direction;
			if (two_sided && direction.dot(normal) <= 0.0f) {
				march.t += best_voxel;
				continue;
			}
			hit.hit = true;
			hit.t = march.t;
			hit.position = position;
			hit.normal = normal;
			hit.steps = march.steps;
			hit.detail = true;
			hit.instance_id = p_instances[best_index].id;
			return hit;
		}
		const float advance = _missing(best) ? MAX(best_voxel, 0.25f) : MAX(Math::abs(best), 0.25f * best_voxel);
		march.t += MIN(advance, MAX(p_tmax - march.t, 0.0f));
	}
	if (march.t < DETAIL_METERS && march.t < p_tmax && march.steps < MAX_STEPS) {
		march.t = MIN(DETAIL_METERS, p_tmax);
	}
	if (_march_global(p_field, p_origins, p_level_count, p_camera, p_origin, direction, p_tmax, p_global_instances, p_global_count, march, hit)) {
		hit.steps = march.steps;
		return hit;
	}
	return _finish_miss(march);
}

NextGISWRTTrace::ShadeResult NextGISWRTTrace::shade_global(float p_weight, const Color &p_voxel, const Color &p_wrc, const Color &p_sky, bool p_allow_wrc) {
	ShadeResult result;
	if (p_weight >= NextGIVoxelLighting::WEIGHT_FALLBACK) {
		result.color = p_voxel;
		result.used_voxel = true;
		return result;
	}
	if (p_allow_wrc && p_wrc.a > 0.5f) {
		result.color = p_wrc;
		result.used_wrc = true;
		return result;
	}
	result.color = p_sky;
	return result;
}

NextGISWRTTrace::ShadeResult NextGISWRTTrace::shade_detail(const NextGIVoxelLighting::CardQuery *p_cards, const Color *p_card_radiance, uint32_t p_card_count, const Vector3 &p_position, const Vector3 &p_normal, float p_voxel_weight, const Color &p_voxel, const Color &p_wrc, const Color &p_sky, bool p_allow_wrc) {
	const NextGIVoxelLighting::CardChoice choice = NextGIVoxelLighting::choose_card(p_cards, p_card_count, p_position, p_normal);
	if (!choice.missing && p_card_radiance != nullptr && choice.card < p_card_count) {
		const float weight = NextGIVoxelLighting::shade_weight(false, choice.projection, choice.depth_error);
		if (weight > 0.0f) {
			ShadeResult result;
			result.color = p_card_radiance[choice.card];
			result.used_card = true;
			result.card = choice.card;
			return result;
		}
	}
	return shade_global(p_voxel_weight, p_voxel, p_wrc, p_sky, p_allow_wrc);
}

NextGISWRTTrace::NextGISWRTTrace() {
}

NextGISWRTTrace::~NextGISWRTTrace() {
	if (gpu != nullptr) {
		RenderingDevice *rd = RenderingDevice::get_singleton();
		if (rd != nullptr) {
			const RID rids[] = { gpu->gather_pipeline, gpu->reflect_pipeline, gpu->probe_pipeline, gpu->shadow_pipeline, gpu->sampler, gpu->dummy_texture, gpu->dummy_buffer, gpu->counter_buffer, gpu->params_buffer, gpu->shade_buffer, gpu->froxel_buffer };
			for (const RID &rid : rids) {
				if (rid.is_valid()) {
					rd->free_rid(rid);
				}
			}
			if (gpu->gather_version.is_valid()) {
				gpu->gather_shader.version_free(gpu->gather_version);
			}
			if (gpu->shadow_version.is_valid()) {
				gpu->shadow_shader.version_free(gpu->shadow_version);
			}
		}
		memdelete(gpu);
		gpu = nullptr;
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
		gpu->gather_shader.initialize(Vector<String>({ "\n#define NEXTGI_SWRT_GATHER\n", "\n#define NEXTGI_SWRT_REFLECT\n", "\n#define NEXTGI_SWRT_LITE_PROBE\n" }));
		gpu->gather_version = gpu->gather_shader.version_create(false);
		if (gpu->gather_version.is_valid()) {
			gpu->gather_pipeline = rd->compute_pipeline_create(gpu->gather_shader.version_get_shader(gpu->gather_version, 0));
			gpu->reflect_pipeline = rd->compute_pipeline_create(gpu->gather_shader.version_get_shader(gpu->gather_version, 1));
			const RID probe_shader = gpu->gather_shader.version_get_shader(gpu->gather_version, 2);
			if (probe_shader.is_valid()) {
				gpu->probe_pipeline = rd->compute_pipeline_create(probe_shader);
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
	if (!gpu->gather_pipeline.is_valid() || !gpu->reflect_pipeline.is_valid() || !gpu->shadow_pipeline.is_valid()) {
		pipelines_failed = true;
		if (!gpu->compile_reported) {
			gpu->compile_reported = true;
			ERR_PRINT("NextGI software ray tracing shaders failed to compile.");
		}
		return false;
	}
	if (!gpu->sampler.is_valid()) {
		RD::SamplerState state;
		state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		state.min_filter = RD::SAMPLER_FILTER_NEAREST;
		gpu->sampler = rd->sampler_create(state);
	}
	if (!gpu->dummy_texture.is_valid()) {
		RD::TextureFormat format;
		format.width = 1;
		format.height = 1;
		format.format = RD::DATA_FORMAT_R32_SFLOAT;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		gpu->dummy_texture = rd->texture_create(format, RD::TextureView());
		if (gpu->dummy_texture.is_valid()) {
			rd->texture_clear(gpu->dummy_texture, Color(0, 0, 0, 1), 0, 1, 0, 1);
		}
	}
	if (!gpu->dummy_buffer.is_valid()) {
		gpu->dummy_buffer = rd->storage_buffer_create(256);
	}
	if (!gpu->counter_buffer.is_valid()) {
		gpu->counter_buffer = rd->storage_buffer_create(sizeof(uint32_t) * 4);
	}
	if (!gpu->params_buffer.is_valid()) {
		gpu->params_buffer = rd->storage_buffer_create(sizeof(TraceParamsGPU));
	}
	if (!gpu->shade_buffer.is_valid()) {
		gpu->shade_buffer = rd->storage_buffer_create(RAY_GRID * RAY_GRID * sizeof(uint32_t));
	}
	if (!gpu->froxel_buffer.is_valid()) {
		gpu->froxel_buffer = rd->storage_buffer_create(NextGISWRTTrace::FROXEL_CELLS * sizeof(uint32_t) + NextGISWRTTrace::FROXEL_CELLS * NextGISWRTTrace::FROXEL_CAP * sizeof(uint32_t));
	}
	gpu->ready = gpu->sampler.is_valid() && gpu->dummy_texture.is_valid() && gpu->dummy_buffer.is_valid() && gpu->counter_buffer.is_valid() && gpu->params_buffer.is_valid() && gpu->shade_buffer.is_valid() && gpu->froxel_buffer.is_valid();
	if (!gpu->ready) {
		pipelines_failed = true;
		ERR_PRINT("NextGI software ray tracing failed to allocate buffers.");
	}
	return gpu->ready;
}

static RID _buffer_or_dummy(const RID &p_buffer, const RID &p_dummy) {
	return p_buffer.is_valid() ? p_buffer : p_dummy;
}

void NextGISWRTTrace::update_frame(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting) {
	trace_steps = 0;
	trace_hits = 0;
	trace_misses = 0;
	shadow_rays = 0;
	mode = MODE_GLOBAL;
	if (p_render_data == nullptr || p_render_data->scene_data == nullptr) {
		return;
	}
	if (ProjectSettings::get_singleton() != nullptr && ProjectSettings::get_singleton()->has_setting("rendering/global_illumination/nextgi/software_ray_tracing_mode")) {
		mode = int(GLOBAL_GET("rendering/global_illumination/nextgi/software_ray_tracing_mode")) == MODE_DETAIL ? MODE_DETAIL : MODE_GLOBAL;
	}
	if (!_ensure_gpu()) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return;
	}
	const Vector3 camera = p_render_data->scene_data->cam_transform.origin;
	const Vector3 view_forward = -p_render_data->scene_data->cam_transform.basis.get_column(2).normalized();
	const uint64_t frame = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0;
	Vector<Candidate> candidates;
	const RendererSceneRender::RenderSDFGIUpdateData *update = p_render_data->sdfgi_update_data;
	const LocalVector<RendererSceneRender::NextGIRTInstance> *feed = update != nullptr ? update->nextgi_rt_instances : nullptr;
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (feed != nullptr && mesh_storage != nullptr) {
		for (uint32_t index = 0; index < feed->size(); index++) {
			const RendererSceneRender::NextGIRTInstance &fed = (*feed)[index];
			if (fed.gpu_source.is_valid() || fed.geometry_instance == nullptr) {
				continue;
			}
			RenderGeometryInstanceBase *geometry = static_cast<RenderGeometryInstanceBase *>(fed.geometry_instance);
			if (geometry->data == nullptr) {
				continue;
			}
			const RID mesh = fed.mesh_override.is_valid() ? fed.mesh_override : geometry->data->base;
			if (!mesh.is_valid()) {
				continue;
			}
			const Transform3D transform = fed.has_rt_transform ? fed.rt_transform : geometry->transform;
			if (!(Math::abs(transform.basis.determinant()) > 1.0e-8f)) {
				continue;
			}
			const NextGISDFAtlas::MeshPages *pages = mesh_storage->get_sdf_atlas().get_pages(mesh);
			if (pages == nullptr || pages->field.is_null() || !pages->field->get_data().is_valid()) {
				continue;
			}
			Candidate candidate;
			candidate.mesh = mesh;
			candidate.transform = transform;
			candidate.distance_squared = camera.distance_squared_to(transform.origin);
			candidate.in_front = instance_in_front(camera, view_forward, transform.origin);
			candidate.two_sided = pages->field->is_two_sided();
			candidates.push_back(candidate);
		}
	}
	if (!candidates.is_empty()) {
		candidates.sort_custom<CandidateOrder>();
	}
	const uint32_t kept = MIN(uint32_t(candidates.size()), INSTANCE_CAP);
	LocalVector<MeshSDFData> fields;
	fields.reserve(kept);
	Vector<NextGIGlobalSDF::Instance> instances;
	for (uint32_t index = 0; index < kept; index++) {
		const NextGISDFAtlas::MeshPages *pages = mesh_storage->get_sdf_atlas().get_pages(candidates[index].mesh);
		if (pages == nullptr || pages->field.is_null()) {
			continue;
		}
		fields.push_back(pages->field->get_data());
		NextGIGlobalSDF::Instance instance;
		instance.transform = candidates[index].transform;
		instance.two_sided = candidates[index].two_sided;
		instances.push_back(instance);
	}
	for (uint32_t index = 0; index < uint32_t(instances.size()); index++) {
		instances.write[index].field = &fields[index];
	}

	field.ensure_buffers();
	const bool pool_ok = field.get_pool_buffer().is_valid();
	// The live allocator does not report a short pool. An invalid pool is the degrade signal.
	const NextGI::SWRTLevelBudget budget = pool_ok ? NextGI::resolve_swrt_levels(NextGIGlobalSDF::BASE_LEVELS, 0, 0) : NextGI::resolve_swrt_levels(NextGIGlobalSDF::BASE_LEVELS, 1, 0);
	active_levels = budget.levels;
	if (budget.reduced) {
		NextGI *next_gi = NextGI::get_singleton();
		if (next_gi != nullptr) {
			next_gi->note_swrt_reduced(budget.levels);
		}
	}
	for (uint32_t level = active_levels; level < NextGIGlobalSDF::BASE_LEVELS; level++) {
		if (level_origin_valid[level]) {
			field.release_level(level);
			level_origin_valid[level] = false;
		}
	}
	const uint32_t levels = active_levels;
	for (uint32_t level = 0; level < levels; level++) {
		if (level_origin_valid[level] && !NextGIGlobalSDF::level_updates_on_frame(level, frame)) {
			continue;
		}
		const Vector3i origin = NextGIGlobalSDF::scrolled_origin(camera, level);
		if (level_origin_valid[level] && level_origins[level] != origin) {
			field.release_level(level);
		}
		level_origins[level] = origin;
		level_origin_valid[level] = true;
		const float voxel = NextGIGlobalSDF::level_voxel_size(level);
		HashSet<uint32_t> seen;
		Vector<BrickCand> bricks;
		for (uint32_t index = 0; index < uint32_t(instances.size()); index++) {
			const NextGIGlobalSDF::Instance &instance = instances[index];
			if (instance.field == nullptr || !instance.field->is_valid()) {
				continue;
			}
			AABB world = instance.transform.xform(instance.field->bounds);
			world = world.grow(voxel);
			const Vector3i min_voxel = NextGIGlobalSDF::world_to_voxel(level, origin, world.position);
			const Vector3i max_voxel = NextGIGlobalSDF::world_to_voxel(level, origin, world.position + world.size);
			const int x0 = CLAMP(min_voxel.x / int(NextGIGlobalSDF::BRICK_SIZE), 0, int(NextGIGlobalSDF::PAGE_SIZE) - 1);
			const int y0 = CLAMP(min_voxel.y / int(NextGIGlobalSDF::BRICK_SIZE), 0, int(NextGIGlobalSDF::PAGE_SIZE) - 1);
			const int z0 = CLAMP(min_voxel.z / int(NextGIGlobalSDF::BRICK_SIZE), 0, int(NextGIGlobalSDF::PAGE_SIZE) - 1);
			const int x1 = CLAMP(max_voxel.x / int(NextGIGlobalSDF::BRICK_SIZE), 0, int(NextGIGlobalSDF::PAGE_SIZE) - 1);
			const int y1 = CLAMP(max_voxel.y / int(NextGIGlobalSDF::BRICK_SIZE), 0, int(NextGIGlobalSDF::PAGE_SIZE) - 1);
			const int z1 = CLAMP(max_voxel.z / int(NextGIGlobalSDF::BRICK_SIZE), 0, int(NextGIGlobalSDF::PAGE_SIZE) - 1);
			if (x1 < x0 || y1 < y0 || z1 < z0) {
				continue;
			}
			for (int z = z0; z <= z1; z++) {
				for (int y = y0; y <= y1; y++) {
					for (int x = x0; x <= x1; x++) {
						const uint32_t packed = uint32_t(x) + NextGIGlobalSDF::PAGE_SIZE * (uint32_t(y) + NextGIGlobalSDF::PAGE_SIZE * uint32_t(z));
						if (seen.has(packed)) {
							continue;
						}
						seen.insert(packed);
						BrickCand cand;
						cand.brick = Vector3i(x, y, z);
						const Vector3 center = NextGIGlobalSDF::voxel_center(level, origin, cand.brick * int(NextGIGlobalSDF::BRICK_SIZE) + Vector3i(4, 4, 4));
						cand.distance_squared = camera.distance_squared_to(center);
						cand.in_front = instance_in_front(camera, view_forward, center);
						bricks.push_back(cand);
					}
				}
			}
		}
		const uint32_t budget = MIN(uint32_t(bricks.size()), BRICK_BUDGET);
		for (uint32_t index = 0; index < budget; index++) {
			uint32_t best = index;
			for (uint32_t other = index + 1; other < uint32_t(bricks.size()); other++) {
				if (instance_before(bricks[other].in_front, bricks[other].distance_squared, bricks[best].in_front, bricks[best].distance_squared)) {
					best = other;
				}
			}
			if (best != index) {
				const BrickCand swap = bricks[index];
				bricks.write[index] = bricks[best];
				bricks.write[best] = swap;
			}
		}
		Vector<Vector3i> selected;
		selected.resize(budget);
		for (uint32_t index = 0; index < budget; index++) {
			selected.write[index] = bricks[index].brick;
		}
		if (budget > 0) {
			field.update_bricks(instances, level, origin, selected.ptr(), budget);
		}
	}

	Ref<RenderSceneBuffersRD> buffers = p_render_data->render_buffers;
	RID depth = gpu->dummy_texture;
	uint32_t screen_w = 1;
	uint32_t screen_h = 1;
	float depth_valid = 0.0f;
	if (buffers.is_valid() && buffers->get_depth_texture().is_valid()) {
		depth = buffers->get_depth_texture();
		const Size2i internal = buffers->get_internal_size();
		screen_w = MAX(internal.x, 1);
		screen_h = MAX(internal.y, 1);
		depth_valid = 1.0f;
	}
	const Projection view_proj = p_render_data->scene_data->cam_projection * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
	TraceParamsGPU params = {};
	_store_projection(view_proj.inverse(), params.inv_view_proj);
	params.camera[0] = camera.x;
	params.camera[1] = camera.y;
	params.camera[2] = camera.z;
	params.screen[0] = float(screen_w);
	params.screen[1] = float(screen_h);
	params.screen[2] = depth_valid;
	params.trace[0] = 32.0f;
	params.trace[1] = SELF_OFFSET_VOXELS * NextGIGlobalSDF::level_voxel_size(0);
	params.trace[2] = mode == MODE_DETAIL ? 1.0f : 0.0f;
	params.trace[3] = float(MAX_STEPS);
	Color sky(0.05f, 0.07f, 0.10f, 1.0f);
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	if (scene != nullptr && p_render_data->environment.is_valid()) {
		sky = scene->environment_get_ambient_light(p_render_data->environment);
	}
	params.sky[0] = sky.r;
	params.sky[1] = sky.g;
	params.sky[2] = sky.b;
	params.sky[3] = 1.0f;
	for (uint32_t level = 0; level < 6; level++) {
		const Vector3i origin = level_origin_valid[level] ? level_origins[level] : NextGIGlobalSDF::scrolled_origin(camera, level);
		params.origins[level * 4] = origin.x;
		params.origins[level * 4 + 1] = origin.y;
		params.origins[level * 4 + 2] = origin.z;
		params.voxel_size[level] = NextGIGlobalSDF::level_voxel_size(level);
	}
	if (p_lighting != nullptr) {
		NextGIVoxelLighting::ClipmapLevelGPU clipmap[NextGIVoxelLighting::LEVELS];
		p_lighting->copy_clipmap(clipmap, NextGIVoxelLighting::LEVELS);
		for (uint32_t level = 0; level < NextGIVoxelLighting::LEVELS; level++) {
			params.clipmap[level * 4] = clipmap[level].origin_x;
			params.clipmap[level * 4 + 1] = clipmap[level].origin_y;
			params.clipmap[level * 4 + 2] = clipmap[level].origin_z;
			params.clipmap[level * 4 + 3] = int32_t(clipmap[level].voxel_size_bits);
		}
	}
	params.froxel[0] = camera.x + view_forward.x * 0.05f - 1.0f;
	params.froxel[1] = camera.y + view_forward.y * 0.05f - 1.0f;
	params.froxel[2] = camera.z + view_forward.z * 0.05f - 1.0f;
	params.froxel[3] = DETAIL_METERS / float(FROXEL_AXIS);
	params.light_dir[0] = view_forward.x;
	params.light_dir[1] = view_forward.y;
	params.light_dir[2] = view_forward.z;
	params.light_dir[3] = 1.0f;
	params.grid[0] = float(RAY_GRID);
	params.grid[1] = float(RAY_GRID);
	params.grid[2] = float(screen_w);
	params.grid[3] = float(screen_h);
	const bool voxel_valid = p_lighting != nullptr && p_lighting->get_radiance_buffer().is_valid() && p_lighting->get_weight_buffer().is_valid();
	params.extra[0] = voxel_valid ? 1.0f : 0.0f;
	params.extra[1] = 0.0f;
	params.extra[2] = 0.0f;
	params.extra[3] = float(instances.size());
	rd->buffer_update(gpu->params_buffer, 0, sizeof(params), &params);
	const uint32_t zeroes[4] = { 0, 0, 0, 0 };
	rd->buffer_update(gpu->counter_buffer, 0, sizeof(zeroes), zeroes);
	Vector<uint32_t> froxel_words;
	froxel_words.resize(FROXEL_CELLS + FROXEL_CELLS * FROXEL_CAP);
	froxel_words.fill(0);
	if (mode == MODE_DETAIL && !instances.is_empty()) {
		Vector<DetailInstance> details;
		details.resize(instances.size());
		for (uint32_t index = 0; index < uint32_t(instances.size()); index++) {
			details.write[index].instance = instances[index];
			details.write[index].id = index;
		}
		FroxelGrid grid;
		build_froxel(details.ptr(), details.size(), Vector3(params.froxel[0], params.froxel[1], params.froxel[2]), grid);
		for (uint32_t cell = 0; cell < FROXEL_CELLS; cell++) {
			froxel_words.write[cell] = grid.counts[cell];
			for (uint32_t slot = 0; slot < FROXEL_CAP; slot++) {
				froxel_words.write[FROXEL_CELLS + cell * FROXEL_CAP + slot] = grid.indices[cell * FROXEL_CAP + slot];
			}
		}
	}
	rd->buffer_update(gpu->froxel_buffer, 0, froxel_words.size() * sizeof(uint32_t), froxel_words.ptr());

	RID mesh_instance_buffer = gpu->dummy_buffer;
	RID mesh_info_buffer = gpu->dummy_buffer;
	RID mesh_page_buffer = gpu->dummy_buffer;
	RID mesh_brick_buffer = gpu->dummy_buffer;
	// Global traces need the instance bounds so a two-sided shell can tell a
	// back face from the front. Detail traces also need the mesh pages.
	if (!instances.is_empty()) {
		Vector<InstanceGPU> gpu_instances;
		Vector<MeshInfoGPU> gpu_meshes;
		PackedInt32Array pages;
		PackedByteArray bricks;
		gpu_instances.resize(instances.size());
		gpu_meshes.resize(instances.size());
		for (uint32_t index = 0; index < uint32_t(instances.size()); index++) {
			const NextGIGlobalSDF::Instance &instance = instances[index];
			const MeshSDFData *data = instance.field;
			InstanceGPU gpu_instance = {};
			const Transform3D inverse = instance.transform.affine_inverse();
			for (int row = 0; row < 3; row++) {
				float *dest = row == 0 ? gpu_instance.inverse_row0 : (row == 1 ? gpu_instance.inverse_row1 : gpu_instance.inverse_row2);
				dest[0] = inverse.basis.rows[row].x;
				dest[1] = inverse.basis.rows[row].y;
				dest[2] = inverse.basis.rows[row].z;
				dest[3] = inverse.origin[row];
			}
			const AABB world = instance.transform.xform(data->bounds);
			const Vector3 scale = instance.transform.basis.get_scale();
			gpu_instance.world_min[0] = world.position.x;
			gpu_instance.world_min[1] = world.position.y;
			gpu_instance.world_min[2] = world.position.z;
			gpu_instance.world_min[3] = MAX(scale.x, MAX(scale.y, scale.z));
			gpu_instance.world_max[0] = world.position.x + world.size.x;
			gpu_instance.world_max[1] = world.position.y + world.size.y;
			gpu_instance.world_max[2] = world.position.z + world.size.z;
			gpu_instance.flags[0] = instance.two_sided ? 2u : 0u;
			gpu_instances.write[index] = gpu_instance;
			if (mode != MODE_DETAIL) {
				continue;
			}

			MeshInfoGPU info = {};
			info.bounds_voxel[0] = data->bounds.position.x;
			info.bounds_voxel[1] = data->bounds.position.y;
			info.bounds_voxel[2] = data->bounds.position.z;
			info.bounds_voxel[3] = data->voxel_size;
			if (!data->mip_dimensions.is_empty()) {
				info.dimensions_flags[0] = data->mip_dimensions[0].x;
				info.dimensions_flags[1] = data->mip_dimensions[0].y;
				info.dimensions_flags[2] = data->mip_dimensions[0].z;
			}
			if (!data->mip_brick_grid_dimensions.is_empty()) {
				info.brick_grid[0] = data->mip_brick_grid_dimensions[0].x;
				info.brick_grid[1] = data->mip_brick_grid_dimensions[0].y;
				info.brick_grid[2] = data->mip_brick_grid_dimensions[0].z;
			}
			info.offsets[0] = uint32_t(pages.size());
			while (bricks.size() % 4 != 0) {
				bricks.push_back(0);
			}
			info.offsets[1] = uint32_t(bricks.size() / 4);
			float band = data->mip_bands.is_empty() ? 1.0f : data->mip_bands[0];
			uint32_t band_bits = 0;
			memcpy(&band_bits, &band, sizeof(band_bits));
			info.offsets[3] = band_bits;
			if (!data->page_tables.is_empty()) {
				pages.append_array(data->page_tables[0]);
			}
			if (!data->brick_data.is_empty()) {
				bricks.append_array(data->brick_data[0]);
			}
			gpu_meshes.write[index] = info;
		}
		if (!gpu_instances.is_empty()) {
			mesh_instance_buffer = rd->storage_buffer_create(gpu_instances.size() * sizeof(InstanceGPU), Span<uint8_t>((const uint8_t *)gpu_instances.ptr(), gpu_instances.size() * sizeof(InstanceGPU)));
		}
		if (mode == MODE_DETAIL && !pages.is_empty() && !bricks.is_empty()) {
			mesh_info_buffer = rd->storage_buffer_create(gpu_meshes.size() * sizeof(MeshInfoGPU), Span<uint8_t>((const uint8_t *)gpu_meshes.ptr(), gpu_meshes.size() * sizeof(MeshInfoGPU)));
			mesh_page_buffer = rd->storage_buffer_create(pages.size() * sizeof(int32_t), Span<uint8_t>((const uint8_t *)pages.ptr(), pages.size() * sizeof(int32_t)));
			mesh_brick_buffer = rd->storage_buffer_create(bricks.size(), Span<uint8_t>(bricks.ptr(), bricks.size()));
		}
	}

	const RID radiance = voxel_valid ? p_lighting->get_radiance_buffer() : gpu->dummy_buffer;
	const RID weights = voxel_valid ? p_lighting->get_weight_buffer() : gpu->dummy_buffer;
	Vector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, gpu->counter_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, gpu->params_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, _buffer_or_dummy(field.get_pool_buffer(), gpu->dummy_buffer)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, _buffer_or_dummy(field.get_dynamic_pool_buffer(), gpu->dummy_buffer)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, _buffer_or_dummy(field.get_coverage_buffer(), gpu->dummy_buffer)));
	for (uint32_t level = 0; level < 6; level++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5 + level, _buffer_or_dummy(field.get_page_buffer(level), gpu->dummy_buffer)));
	}
	for (uint32_t level = 0; level < 6; level++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11 + level, _buffer_or_dummy(field.get_coarse_buffer(level), gpu->dummy_buffer)));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, radiance));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, weights));
	{
		RD::Uniform depth_uniform;
		depth_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		depth_uniform.binding = 19;
		depth_uniform.append_id(gpu->sampler);
		depth_uniform.append_id(depth);
		uniforms.push_back(depth_uniform);
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, mesh_instance_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, mesh_info_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, mesh_page_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, mesh_brick_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, gpu->froxel_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, gpu->dummy_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 26, gpu->shade_buffer));

	const uint32_t groups = (RAY_GRID + 7u) / 8u;
	const RID gather_shader = gpu->gather_shader.version_get_shader(gpu->gather_version, 0);
	const RID reflect_shader = gpu->gather_shader.version_get_shader(gpu->gather_version, 1);
	const RID shadow_shader = gpu->shadow_shader.version_get_shader(gpu->shadow_version, 0);
	const bool lite = NextGI::is_lumen_lite();
	RID probe_buffer;
	uint32_t probe_base = 0;
	if (lite) {
		NextGI *next_gi = NextGI::get_singleton();
		NextGIIrradianceField *lite_field = next_gi != nullptr ? next_gi->ensure_irradiance_field() : nullptr;
		if (lite_field != nullptr && lite_field->ensure_probe_buffer()) {
			probe_buffer = lite_field->get_probe_buffer();
			probe_base = lite_field->consume_swrt_probe_base();
		}
	}
	const RID gather_set = lite ? RID() : rd->uniform_set_create(uniforms, gather_shader, 0);
	const RID reflect_set = lite ? RID() : rd->uniform_set_create(uniforms, reflect_shader, 0);
	RID probe_set;
	if (lite && gpu->probe_pipeline.is_valid() && probe_buffer.is_valid()) {
		Vector<RD::Uniform> probe_uniforms = uniforms;
		probe_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, probe_buffer));
		const RID probe_shader = gpu->gather_shader.version_get_shader(gpu->gather_version, 2);
		probe_set = rd->uniform_set_create(probe_uniforms, probe_shader, 0);
	}
	const RID shadow_set = rd->uniform_set_create(uniforms, shadow_shader, 0);
	if (shadow_set.is_valid() && (lite || (gather_set.is_valid() && reflect_set.is_valid()))) {
		if (lite && probe_set.is_valid()) {
			RENDER_TIMESTAMP("NextGI SDF Lite Probes");
			const RD::ComputeListID probe_list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(probe_list, gpu->probe_pipeline);
			rd->compute_list_bind_uniform_set(probe_list, probe_set, 0);
			struct ProbePush {
				uint32_t probe_base;
				uint32_t pad0;
				uint32_t pad1;
				uint32_t pad2;
			} push = { probe_base, 0u, 0u, 0u };
			rd->compute_list_set_push_constant(probe_list, &push, sizeof(push));
			rd->compute_list_dispatch(probe_list, NextGI::LITE_PROBES_PER_FRAME, 1, 1);
			rd->compute_list_end();
			if (NextGI::get_singleton() != nullptr) {
				NextGI::get_singleton()->note_lite_probe_traces(NextGI::LITE_PROBES_PER_FRAME);
			}
		} else if (!lite) {
			RENDER_TIMESTAMP("NextGI SDF Trace");
			const RD::ComputeListID gather_list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(gather_list, gpu->gather_pipeline);
			rd->compute_list_bind_uniform_set(gather_list, gather_set, 0);
			rd->compute_list_dispatch(gather_list, groups, groups, 1);
			rd->compute_list_end();
			if (NextGI::is_reflections_enabled()) {
				const RD::ComputeListID reflect_list = rd->compute_list_begin();
				rd->compute_list_bind_compute_pipeline(reflect_list, gpu->reflect_pipeline);
				rd->compute_list_bind_uniform_set(reflect_list, reflect_set, 0);
				rd->compute_list_dispatch(reflect_list, groups, groups, 1);
				rd->compute_list_end();
			}
		}
		RENDER_TIMESTAMP("NextGI SDF Shadow");
		const RD::ComputeListID shadow_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(shadow_list, gpu->shadow_pipeline);
		rd->compute_list_bind_uniform_set(shadow_list, shadow_set, 0);
		rd->compute_list_dispatch(shadow_list, groups, groups, 1);
		rd->compute_list_end();
		const Vector<uint8_t> readback = rd->buffer_get_data(gpu->counter_buffer);
		if (readback.size() >= int(sizeof(uint32_t) * 4)) {
			uint32_t values[4] = {};
			memcpy(values, readback.ptr(), sizeof(values));
			trace_steps = values[0];
			trace_hits = values[1];
			trace_misses = values[2];
			shadow_rays = values[3];
		}
	}
	for (const RID &set : { gather_set, reflect_set, probe_set, shadow_set }) {
		if (set.is_valid()) {
			rd->free_rid(set);
		}
	}
	const RID created_buffers[] = { mesh_instance_buffer, mesh_info_buffer, mesh_page_buffer, mesh_brick_buffer };
	for (const RID &created : created_buffers) {
		if (created.is_valid() && created != gpu->dummy_buffer) {
			rd->free_rid(created);
		}
	}
}
