/**************************************************************************/
/*  next_gi_shadow_check.cpp                                              */
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

#include "next_gi_shadow_check.h"

#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "core/templates/hashfuncs.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_shadow_check.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// GodotNext: nextgi_shadow_policy (#145)

namespace {

constexpr uint32_t SHADOW_CHECK_SEED = 145;
constexpr uint32_t SHADOW_CHECK_COUNTER_BYTES = NEXTGI_SHADOW_CHECK_COUNTER_MAX * sizeof(uint32_t);

// One readback in flight at a time. Its results land here, with the allocation and the light words it ran with,
// so a check can tell runs before and after an allocation change apart.
Mutex shadow_check_mutex;
uint64_t shadow_check_next_token = 1;
uint64_t shadow_check_pending_token = 0;
uint64_t shadow_check_pending_generation = 0;
uint32_t shadow_check_pending_modes[NEXTGI_SHADOW_CHECK_LIGHTS] = {};
uint64_t shadow_check_runs = 0;
uint32_t shadow_check_results[NEXTGI_SHADOW_CHECK_COUNTER_MAX] = {};
uint64_t shadow_check_result_generation = 0;
uint32_t shadow_check_result_modes[NEXTGI_SHADOW_CHECK_LIGHTS] = {};
uint32_t shadow_check_result_built = 0;
uint32_t shadow_check_built = 0;

} // namespace

const char *nextgi_shadow_check_counter_name(NextGIShadowCheckCounter p_counter) {
	switch (p_counter) {
		case NEXTGI_SHADOW_CHECK_COUNTER_RAYS:
			return "rays";
		case NEXTGI_SHADOW_CHECK_COUNTER_AGREE:
			return "agree";
		case NEXTGI_SHADOW_CHECK_COUNTER_DISAGREE:
			return "disagree";
		case NEXTGI_SHADOW_CHECK_COUNTER_EXPECTED_OCCLUDED:
			return "expected_occluded";
		case NEXTGI_SHADOW_CHECK_COUNTER_TRACED_OCCLUDED:
			return "traced_occluded";
		case NEXTGI_SHADOW_CHECK_COUNTER_FAST_RAYS:
			return "fast_rays";
		case NEXTGI_SHADOW_CHECK_COUNTER_LOOP_RAYS:
			return "loop_rays";
		case NEXTGI_SHADOW_CHECK_COUNTER_FALLBACK_RAYS:
			return "fallback_rays";
		case NEXTGI_SHADOW_CHECK_COUNTER_FAST_DISAGREE:
			return "fast_disagree";
		case NEXTGI_SHADOW_CHECK_COUNTER_LOOP_DISAGREE:
			return "loop_disagree";
		case NEXTGI_SHADOW_CHECK_COUNTER_FALLBACK_DISAGREE:
			return "fallback_disagree";
		case NEXTGI_SHADOW_CHECK_COUNTER_ALPHA_SAMPLES:
			return "alpha_samples";
		case NEXTGI_SHADOW_CHECK_COUNTER_FIRST_DISAGREEMENT:
			return "first_disagreement";
		default:
			return "";
	}
}

struct NextGIShadowCheck::Internal {
	NextgiShadowCheckShaderRD shader;
	RID version;
	RID pipeline;
	bool failed = false;
	RID ray_buffer;
	RID expected_buffer;
	RID light_buffer;
	RID counter_buffer;
	RID readback_buffer;
	uint32_t ray_count = 0;
	uint64_t scene_key = 0;
	bool has_scene_key = false;
};

bool NextGIShadowCheck::_ensure_shader() {
	Internal &in = *internal;
	if (in.pipeline.is_valid()) {
		return true;
	}
	if (in.failed) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	NextGI *next_gi = NextGI::get_singleton();
	if (rd == nullptr || next_gi == nullptr || !next_gi->can_initialize_ray_query_shaders()) {
		return false;
	}
	in.shader.initialize(Vector<String>({ "" }));
	in.version = in.shader.version_create(false);
	if (!in.version.is_valid()) {
		in.failed = true;
		ERR_FAIL_V_MSG(false, "The NextGI shadow-ray check shader failed to compile.");
	}
	next_gi->notify_ray_query_shader_initialized();
	in.pipeline = rd->compute_pipeline_create(in.shader.version_get_shader(in.version, 0));
	if (in.pipeline.is_null()) {
		in.failed = true;
		return false;
	}
	in.ray_buffer = rd->storage_buffer_create(NEXTGI_SHADOW_CHECK_RAYS * sizeof(NextGIShadowCheckRay));
	in.expected_buffer = rd->storage_buffer_create(NEXTGI_SHADOW_CHECK_RAYS);
	in.light_buffer = rd->storage_buffer_create(NEXTGI_SHADOW_CHECK_LIGHTS * 2 * sizeof(uint32_t));
	in.counter_buffer = rd->storage_buffer_create(SHADOW_CHECK_COUNTER_BYTES);
	in.readback_buffer = rd->storage_buffer_create(SHADOW_CHECK_COUNTER_BYTES);
	return in.ray_buffer.is_valid() && in.expected_buffer.is_valid() && in.light_buffer.is_valid() && in.counter_buffer.is_valid() && in.readback_buffer.is_valid();
}

void NextGIShadowCheck::run(NextGIRTScene *p_rt_scene) {
	ERR_FAIL_NULL(p_rt_scene);
	RenderingDevice *rd = RenderingDevice::get_singleton();
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (rd == nullptr || mesh_storage == nullptr || !p_rt_scene->is_tlas_traceable() || p_rt_scene->get_tlas().is_null() || p_rt_scene->get_shadow_instance_buffer().is_null() || p_rt_scene->get_shadow_alpha_buffer().is_null()) {
		return;
	}
	{
		MutexLock lock(shadow_check_mutex);
		if (shadow_check_pending_token != 0) {
			return; // The last run's counters are still on their way.
		}
	}
	if (!_ensure_shader()) {
		return;
	}
	Internal &in = *internal;
	uint32_t masks[NEXTGI_SHADOW_CHECK_LIGHTS];
	nextgi_shadow_check_masks(masks);

	// The rays depend on the scene only: rebuild them when a source moved or changed.
	const LocalVector<NextGIRTScene::ShadowCheckSource> &sources = p_rt_scene->get_shadow_check_sources();
	uint32_t key = hash_murmur3_one_32(sources.size());
	for (const NextGIRTScene::ShadowCheckSource &source : sources) {
		for (int row = 0; row < 3; row++) {
			for (int column = 0; column < 3; column++) {
				key = hash_murmur3_one_float(source.transform.basis.rows[row][column], key);
			}
			key = hash_murmur3_one_float(source.transform.origin[row], key);
			key = hash_murmur3_one_float(source.world_bounds.position[row], key);
			key = hash_murmur3_one_float(source.world_bounds.size[row], key);
		}
		key = hash_murmur3_one_32(source.layers, key);
		key = hash_murmur3_one_64(source.mesh.get_id(), key);
		key = hash_murmur3_one_32((source.opaque_caster ? 1u : 0u) | (source.has_geometry ? 2u : 0u), key);
		for (uint32_t surface : source.surfaces) {
			key = hash_murmur3_one_32(surface, key);
		}
	}
	if (!in.has_scene_key || in.scene_key != key) {
		LocalVector<Vector<float>> positions;
		LocalVector<Vector<uint32_t>> indices;
		LocalVector<NextGIShadowCheckInstance> instances;
		for (const NextGIRTScene::ShadowCheckSource &source : sources) {
			NextGIShadowCheckInstance instance;
			instance.transform = source.transform;
			instance.world_bounds = source.world_bounds;
			instance.layers = source.layers;
			instance.opaque_caster = source.opaque_caster && source.has_geometry;
			bool placed = false;
			if (source.has_geometry && source.mesh.is_valid()) {
				for (uint32_t surface : source.surfaces) {
					Vector<float> surface_positions;
					Vector<uint32_t> surface_indices;
					if (!mesh_storage->mesh_nextgi_get_cpu_geometry(source.mesh, surface, surface_positions, surface_indices)) {
						continue;
					}
					positions.push_back(surface_positions);
					indices.push_back(surface_indices);
					instances.push_back(instance);
					placed = true;
				}
			}
			if (!placed) {
				// No triangles on the CPU: rays near it are left out.
				instance.opaque_caster = false;
				instances.push_back(instance);
				positions.push_back(Vector<float>());
				indices.push_back(Vector<uint32_t>());
			}
		}
		for (uint32_t i = 0; i < instances.size(); i++) {
			instances[i].positions = positions[i].is_empty() ? nullptr : positions[i].ptr();
			instances[i].vertex_count = positions[i].size() / 3;
			instances[i].indices = indices[i].is_empty() ? nullptr : indices[i].ptr();
			instances[i].index_count = indices[i].size();
		}
		LocalVector<NextGIShadowCheckRay> rays;
		LocalVector<uint8_t> expected;
		in.ray_count = nextgi_shadow_check_build(instances.ptr(), instances.size(), masks, NEXTGI_SHADOW_CHECK_RAYS, SHADOW_CHECK_SEED, rays, expected);
		if (in.ray_count > 0) {
			rd->buffer_update(in.ray_buffer, 0, in.ray_count * sizeof(NextGIShadowCheckRay), rays.ptr());
			while (expected.size() % 4 != 0) {
				expected.push_back(0);
			}
			rd->buffer_update(in.expected_buffer, 0, expected.size(), expected.ptr());
		}
		in.scene_key = key;
		in.has_scene_key = true;
		MutexLock lock(shadow_check_mutex);
		shadow_check_built++;
	}
	if (in.ray_count == 0) {
		return;
	}

	uint32_t lights[NEXTGI_SHADOW_CHECK_LIGHTS * 2];
	for (int light = 0; light < NEXTGI_SHADOW_CHECK_LIGHTS; light++) {
		lights[light * 2] = p_rt_scene->get_shadow_light_mode(masks[light]);
		lights[light * 2 + 1] = masks[light] & NEXTGI_SHADOW_LAYER_MASK;
	}
	rd->buffer_update(in.light_buffer, 0, sizeof(lights), lights);
	uint32_t counters[NEXTGI_SHADOW_CHECK_COUNTER_MAX] = {};
	counters[NEXTGI_SHADOW_CHECK_COUNTER_FIRST_DISAGREEMENT] = UINT32_MAX;
	rd->buffer_update(in.counter_buffer, 0, SHADOW_CHECK_COUNTER_BYTES, counters);

	RD::Uniform uniforms[7] = {
		RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, p_rt_scene->get_tlas()),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_rt_scene->get_shadow_instance_buffer()),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_rt_scene->get_shadow_alpha_buffer()),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, in.ray_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, in.expected_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, in.light_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, in.counter_buffer),
	};
	const RID uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 7), in.shader.version_get_shader(in.version, 0), 0);
	ERR_FAIL_COND(uniform_set.is_null());
	const uint32_t push_constant[4] = { in.ray_count, 0, 0, 0 };
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, in.pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	rd->compute_list_set_push_constant(list, push_constant, sizeof(push_constant));
	rd->compute_list_dispatch(list, (in.ray_count + 63) / 64, 1, 1);
	rd->compute_list_end();
	rd->free_rid(uniform_set);

	uint64_t token = 0;
	{
		MutexLock lock(shadow_check_mutex);
		token = shadow_check_next_token++;
		shadow_check_pending_token = token;
		shadow_check_pending_generation = p_rt_scene->get_shadow_allocation().generation;
		for (int light = 0; light < NEXTGI_SHADOW_CHECK_LIGHTS; light++) {
			shadow_check_pending_modes[light] = lights[light * 2];
		}
	}
	// A copy, so the next run's zeroing cannot race the read.
	rd->buffer_copy(in.counter_buffer, in.readback_buffer, 0, 0, SHADOW_CHECK_COUNTER_BYTES);
	if (rd->buffer_get_data_async(in.readback_buffer, callable_mp_static(&NextGIShadowCheck::_counters_received).bind(token), 0, SHADOW_CHECK_COUNTER_BYTES) != OK) {
		MutexLock lock(shadow_check_mutex);
		shadow_check_pending_token = 0;
	}
}

void NextGIShadowCheck::_counters_received(const Vector<uint8_t> &p_data, uint64_t p_token) {
	MutexLock lock(shadow_check_mutex);
	if (p_token != shadow_check_pending_token) {
		return;
	}
	shadow_check_pending_token = 0;
	if (p_data.size() < int(SHADOW_CHECK_COUNTER_BYTES)) {
		return;
	}
	memcpy(shadow_check_results, p_data.ptr(), SHADOW_CHECK_COUNTER_BYTES);
	shadow_check_result_generation = shadow_check_pending_generation;
	for (int light = 0; light < NEXTGI_SHADOW_CHECK_LIGHTS; light++) {
		shadow_check_result_modes[light] = shadow_check_pending_modes[light];
	}
	shadow_check_result_built = shadow_check_built;
	shadow_check_runs++;
}

void NextGIShadowCheck::fill_counters(Dictionary &r_counters) {
	MutexLock lock(shadow_check_mutex);
	r_counters["shadow_check_runs"] = int64_t(shadow_check_runs);
	r_counters["shadow_check_builds"] = int64_t(shadow_check_result_built);
	r_counters["shadow_check_generation"] = int64_t(shadow_check_result_generation);
	for (int i = 0; i < NEXTGI_SHADOW_CHECK_COUNTER_MAX; i++) {
		const char *name = nextgi_shadow_check_counter_name(NextGIShadowCheckCounter(i));
		if (name[0] != '\0') {
			r_counters[String("shadow_check_") + name] = int64_t(shadow_check_results[i]);
		}
	}
	PackedInt64Array modes;
	PackedInt64Array masks;
	uint32_t check_masks[NEXTGI_SHADOW_CHECK_LIGHTS];
	nextgi_shadow_check_masks(check_masks);
	for (int light = 0; light < NEXTGI_SHADOW_CHECK_LIGHTS; light++) {
		modes.push_back(int64_t(shadow_check_result_modes[light]));
		masks.push_back(int64_t(check_masks[light]));
	}
	r_counters["shadow_check_modes"] = modes;
	r_counters["shadow_check_masks"] = masks;
}

void NextGIShadowCheck::free() {
	{
		// A late callback must not count against the next check.
		MutexLock lock(shadow_check_mutex);
		shadow_check_pending_token = 0;
	}
	if (internal == nullptr) {
		return;
	}
	Internal &in = *internal;
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr) {
		RID buffers[5] = { in.ray_buffer, in.expected_buffer, in.light_buffer, in.counter_buffer, in.readback_buffer };
		for (const RID &buffer : buffers) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
		}
		if (in.pipeline.is_valid()) {
			rd->free_rid(in.pipeline);
		}
	}
	if (in.version.is_valid()) {
		in.shader.version_free(in.version);
	}
	memdelete(internal);
	internal = nullptr;
}

NextGIShadowCheck::NextGIShadowCheck() {
	internal = memnew(Internal);
}

NextGIShadowCheck::~NextGIShadowCheck() {
	free();
}

} // namespace RendererRD
