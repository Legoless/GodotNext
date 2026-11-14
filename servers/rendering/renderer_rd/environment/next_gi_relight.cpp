/**************************************************************************/
/*  next_gi_relight.cpp                                                   */
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

#include "next_gi_relight.h"

#include "core/config/project_settings.h"
#include "core/math/projection.h"
#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_far_field.h"
#include "servers/rendering/renderer_rd/environment/next_gi_swrt_trace.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_src_relight.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_src_relight_swrt.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"

#include <algorithm>
#include <cstdint>

using namespace RendererRD;

namespace RendererRD {

uint32_t nextgi_src_schedule_streams(const NextGISrcSchedulePage *p_pages, uint32_t p_count, uint32_t p_texel_cap, uint32_t p_ray_cap, uint32_t *r_indices, uint32_t p_capacity) {
	if (p_pages == nullptr || r_indices == nullptr || p_count == 0 || p_capacity == 0) {
		return 0;
	}
	LocalVector<uint32_t> dirty;
	LocalVector<uint32_t> near;
	LocalVector<uint32_t> rest;
	LocalVector<uint32_t> far_rest;
	for (uint32_t i = 0; i < p_count; i++) {
		if (p_pages[i].far_field) {
			far_rest.push_back(i);
		} else if (p_pages[i].dirty_direct) {
			dirty.push_back(i);
		} else if (p_pages[i].near_dirty) {
			near.push_back(i);
		} else {
			rest.push_back(i);
		}
	}
	struct ByPriority {
		const NextGISrcSchedulePage *pages = nullptr;
		bool operator()(uint32_t p_a, uint32_t p_b) const { return pages[p_a].priority > pages[p_b].priority; }
	} by_priority;
	by_priority.pages = p_pages;
	if (dirty.size() > 1) {
		std::sort(dirty.ptr(), dirty.ptr() + dirty.size(), by_priority);
	}
	struct ByAge {
		const NextGISrcSchedulePage *pages = nullptr;
		bool operator()(uint32_t p_a, uint32_t p_b) const { return pages[p_a].last_relit_frame < pages[p_b].last_relit_frame; }
	} by_age;
	by_age.pages = p_pages;
	if (near.size() > 1) {
		std::sort(near.ptr(), near.ptr() + near.size(), by_age);
	}
	if (rest.size() > 1) {
		std::sort(rest.ptr(), rest.ptr() + rest.size(), by_age);
	}
	if (far_rest.size() > 1) {
		std::sort(far_rest.ptr(), far_rest.ptr() + far_rest.size(), by_age);
	}
	for (const uint32_t index : far_rest) {
		rest.push_back(index);
	}
	const uint32_t near_limit = MAX(p_count / 16u, 1u);
	uint32_t scheduled = 0;
	uint32_t texels = 0;
	uint32_t rays = 0;
	uint32_t near_taken = 0;
	auto take = [&](uint32_t p_index, bool p_counts_as_near) {
		if (scheduled >= p_capacity) {
			return false;
		}
		if (p_counts_as_near && near_taken >= near_limit) {
			return false;
		}
		const NextGISrcSchedulePage &page = p_pages[p_index];
		if (page.texels == 0) {
			return true;
		}
		if (scheduled > 0 && (texels + page.texels > p_texel_cap || rays + page.ray_estimate > p_ray_cap)) {
			return false;
		}
		r_indices[scheduled++] = p_index;
		texels += page.texels;
		rays += page.ray_estimate;
		if (p_counts_as_near) {
			near_taken++;
		}
		return texels < p_texel_cap && rays < p_ray_cap;
	};
	for (uint32_t index : dirty) {
		if (!take(index, false)) {
			return scheduled;
		}
	}
	for (uint32_t index : near) {
		if (near_taken >= near_limit) {
			break;
		}
		if (!take(index, true)) {
			return scheduled;
		}
	}
	for (uint32_t index : rest) {
		if (!take(index, false)) {
			return scheduled;
		}
	}
	return scheduled;
}

} // namespace RendererRD

NextGIRelight *NextGIRelight::singleton = nullptr;

struct NextGIRelightPageGPU {
	uint32_t record_index = 0;
	uint32_t card_index = 0;
	uint32_t radiance_origin = 0;
	uint32_t radiance_size = 0;
	uint32_t mask_origin = 0;
	uint32_t mask_size = 0;
	uint32_t light_offset = 0;
	uint32_t shadowed_count = 0;
	uint32_t light_count = 0;
	uint32_t stable_id = 0;
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(NextGIRelightPageGPU) == 48, "The relight page must match nextgi_src_relight.glsl.");

struct NextGIRelightParamsGPU {
	uint32_t frame = 0;
	uint32_t ray_budget = 0;
	uint32_t page_count = 0;
	float rt_radius = 0.0f;
	uint32_t page_offset = 0;
};
static_assert(sizeof(NextGIRelightParamsGPU) == 20, "The relight params must match nextgi_src_relight.glsl.");

struct NextGIRelightCountersGPU {
	uint32_t traced = 0;
	uint32_t skipped = 0;
	uint32_t reserved = 0;
	uint32_t pad = 0;
};

// GodotNext: nextgi_async_counters (#186)
static Mutex nextgi_relight_counter_mutex;
static HashMap<uint64_t, NextGIRelight *> nextgi_relight_counter_registry;
static uint64_t nextgi_relight_counter_next_token = 1;

struct NextGIRelight::ShaderState {
	NextgiSrcRelightShaderRD shader;
	RID version;
	RID pipeline;
};

// GodotNext: ps_swrt_trace (#186, #188)
struct NextGIRelight::SWRTShaderState {
	NextgiSrcRelightSwrtShaderRD shader;
	RID version;
	RID pipeline;
};

static uint32_t _pack_xy(uint32_t p_x, uint32_t p_y) {
	return p_x | (p_y << 16);
}

static void _projection_to_columns(const Projection &p_projection, float r_matrix[16]) {
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			r_matrix[column * 4 + row] = p_projection.columns[column][row];
		}
	}
}

static Projection _world_shadow_matrix(const Projection &p_camera, const Transform3D &p_light_transform, const Rect2 &p_atlas) {
	Projection correction;
	correction.set_depth_correction(false, true, false);
	const Projection camera = correction * p_camera;
	Projection bias;
	bias.set_light_bias();
	Projection rect;
	rect.set_light_atlas_rect(p_atlas);
	// World space, where the forward renderer uses view space (D14).
	return rect * bias * camera * Projection(p_light_transform.inverse());
}

static Basis _record_basis(const NextGIInstanceRecord &p_record) {
	Basis basis;
	basis.rows[0] = Vector3(p_record.object_to_world[0], p_record.object_to_world[1], p_record.object_to_world[2]);
	basis.rows[1] = Vector3(p_record.object_to_world[4], p_record.object_to_world[5], p_record.object_to_world[6]);
	basis.rows[2] = Vector3(p_record.object_to_world[8], p_record.object_to_world[9], p_record.object_to_world[10]);
	return basis;
}

static Vector3 _record_origin(const NextGIInstanceRecord &p_record) {
	return Vector3(p_record.object_to_world[3], p_record.object_to_world[7], p_record.object_to_world[11]);
}

static Vector3 _card_world(const NextGICard &p_card, const NextGIInstanceRecord &p_record, const Vector3 &p_object) {
	return _record_basis(p_record).xform(p_object) + _record_origin(p_record);
}

static AABB _card_world_aabb(const NextGICard &p_card, const NextGIInstanceRecord &p_record) {
	const Vector3 origin(p_card.origin[0], p_card.origin[1], p_card.origin[2]);
	const Vector3 axis_u(p_card.axis_u[0], p_card.axis_u[1], p_card.axis_u[2]);
	const Vector3 axis_v(p_card.axis_v[0], p_card.axis_v[1], p_card.axis_v[2]);
	const Vector3 axis_n(p_card.axis_n[0], p_card.axis_n[1], p_card.axis_n[2]);
	AABB bounds;
	bool first = true;
	for (int su = -1; su <= 1; su += 2) {
		for (int sv = -1; sv <= 1; sv += 2) {
			for (int sz = 0; sz <= 1; sz++) {
				const Vector3 object = origin + axis_u * (float(su) * p_card.axis_u[3]) + axis_v * (float(sv) * p_card.axis_v[3]) + axis_n * (-p_card.axis_n[3] * float(sz));
				const Vector3 world = _card_world(p_card, p_record, object);
				if (first) {
					bounds.position = world;
					bounds.size = Vector3();
					first = false;
				} else {
					bounds.expand_to(world);
				}
			}
		}
	}
	return bounds;
}

NextGIRelight::NextGIRelight() {
	if (singleton == nullptr) {
		singleton = this;
	}
}

NextGIRelight::~NextGIRelight() {
	{
		MutexLock lock(nextgi_relight_counter_mutex);
		if (counter_token != 0) {
			nextgi_relight_counter_registry.erase(counter_token);
			counter_token = 0;
		}
		counters_pending = false;
	}
	free_resources();
	if (singleton == this) {
		singleton = nullptr;
	}
}

void NextGIRelight::_reset_uniform_set() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr) {
		if (uniform_set.is_valid()) {
			rd->free_rid(uniform_set);
		}
		if (far_uniform_set.is_valid()) {
			rd->free_rid(far_uniform_set);
		}
	}
	uniform_set = RID();
	far_uniform_set = RID();
}

void NextGIRelight::free_resources() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	_reset_uniform_set();
	{
		// GodotNext: nextgi_async_counters (#186). A late callback must not reach freed buffers.
		MutexLock lock(nextgi_relight_counter_mutex);
		if (counter_token != 0) {
			nextgi_relight_counter_registry.erase(counter_token);
			counter_token = 0;
		}
		counters_pending = false;
		pending_schedule.clear();
	}
	if (rd != nullptr && readback_buffer.is_valid()) {
		rd->free_rid(readback_buffer);
	}
	readback_buffer = RID();
	if (shader != nullptr) {
		if (rd != nullptr && shader->pipeline.is_valid()) {
			rd->free_rid(shader->pipeline);
		}
		if (shader->version.is_valid()) {
			shader->shader.version_free(shader->version);
		}
		memdelete(shader);
		shader = nullptr;
	}
	// GodotNext: ps_swrt_trace (#186, #188)
	if (swrt_shader != nullptr) {
		if (rd != nullptr && swrt_shader->pipeline.is_valid()) {
			rd->free_rid(swrt_shader->pipeline);
		}
		if (swrt_shader->version.is_valid()) {
			swrt_shader->shader.version_free(swrt_shader->version);
		}
		memdelete(swrt_shader);
		swrt_shader = nullptr;
	}
	if (rd == nullptr) {
		return;
	}
	RID buffers[8] = { light_buffer, reuse_buffer, page_buffer, index_buffer, counter_buffer, probe_buffer, params_buffer, candidate_buffer };
	for (int i = 0; i < 8; i++) {
		if (buffers[i].is_valid()) {
			rd->free_rid(buffers[i]);
		}
	}
	light_buffer = RID();
	candidate_buffer = RID();
	candidate_capacity = 0;
	candidate_key = 0;
	reuse_buffer = RID();
	page_buffer = RID();
	index_buffer = RID();
	counter_buffer = RID();
	probe_buffer = RID();
	params_buffer = RID();
	if (radiance.is_valid()) {
		rd->free_rid(radiance);
	}
	if (mask.is_valid()) {
		rd->free_rid(mask);
	}
	if (dummy_depth.is_valid()) {
		rd->free_rid(dummy_depth);
	}
	if (material_sampler.is_valid()) {
		rd->free_rid(material_sampler);
	}
	if (shadow_sampler.is_valid()) {
		rd->free_rid(shadow_sampler);
	}
	if (indirect_texture.is_valid()) {
		rd->free_rid(indirect_texture);
	}
	if (page_rect_buffer.is_valid()) {
		rd->free_rid(page_rect_buffer);
	}
	indirect_texture = RID();
	page_rect_buffer = RID();
	page_rect_uploaded = 0;
	radiance = RID();
	mask = RID();
	dummy_depth = RID();
	material_sampler = RID();
	shadow_sampler = RID();
	atlases_ready = false;
}

bool NextGIRelight::_ensure_atlases() {
	if (atlases_ready) {
		return radiance.is_valid() && mask.is_valid();
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);

	RD::SamplerState material_state;
	material_state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
	material_state.min_filter = RD::SAMPLER_FILTER_NEAREST;
	material_sampler = rd->sampler_create(material_state);

	RD::SamplerState shadow_state;
	shadow_state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
	shadow_state.min_filter = RD::SAMPLER_FILTER_NEAREST;
	shadow_state.enable_compare = true;
	shadow_state.compare_op = RD::COMPARE_OP_GREATER;
	shadow_sampler = rd->sampler_create(shadow_state);

	BitField<RD::TextureUsageBits> storage = RD::TEXTURE_USAGE_STORAGE_BIT;
	storage.set_flag(RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	storage.set_flag(RD::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	radiance_is_packed = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32, storage);
	// GodotNext: nextgi_quality_presets (#55). The radiance atlas doubles in height at Epic.
	// GodotNext: ps_lumen_lite (#183). Lite halves it. The page allocator takes the same extent.
	const NextGIAtlasBudget atlas_budget = nextgi_src_current_atlas_budget();
	set_page_extent(atlas_budget);
	RD::TextureFormat tf;
	tf.width = atlas_budget.radiance_width;
	tf.height = atlas_budget.radiance_height;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.usage_bits = storage | RD::TEXTURE_USAGE_SAMPLING_BIT;
	tf.format = radiance_is_packed ? RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32 : RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	radiance = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(radiance.is_null(), false);
	rd->set_resource_name(radiance, "NextGI SRC Radiance");
	rd->texture_clear(radiance, Color(0, 0, 0, 1), 0, 1, 0, 1);
	radiance_bytes = uint64_t(atlas_budget.radiance_width) * atlas_budget.radiance_height * (radiance_is_packed ? 4 : 8);

	tf.width = mask_extent.x;
	tf.height = mask_extent.y;
	tf.format = RD::DATA_FORMAT_R8_UINT;
	mask = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(mask.is_null(), false);
	rd->set_resource_name(mask, "NextGI SRC Shadow Mask");
	rd->texture_clear(mask, Color(0, 0, 0, 0), 0, 1, 0, 1);

	tf.width = atlas_budget.radiance_width / 8;
	tf.height = atlas_budget.radiance_height / 8;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	indirect_texture = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(indirect_texture.is_null(), false);
	rd->set_resource_name(indirect_texture, "NextGI SRC Indirect");
	rd->texture_clear(indirect_texture, Color(0, 0, 0, 0), 0, 1, 0, 1);

	tf.width = 1;
	tf.height = 1;
	tf.format = RD::DATA_FORMAT_D32_SFLOAT;
	// The attachment bit gives the view a depth aspect; without it Vulkan makes a colour view of a depth format.
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	dummy_depth = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(dummy_depth.is_null(), false);
	{
		// Reverse-Z far (0), so the GREATER shadow compare against the fallback reads as lit.
		const float far_depth = 0.0f;
		Vector<uint8_t> far_data;
		far_data.resize(sizeof(float));
		memcpy(far_data.ptrw(), &far_depth, sizeof(float));
		rd->texture_update(dummy_depth, 0, far_data);
	}

	atlases_ready = true;
	if (!logged_atlas) {
		logged_atlas = true;
		const double mask_bytes = double(mask_extent.x) * double(mask_extent.y);
		print_line(vformat("NextGI SRC: radiance atlas %dx%d %s, %.1f MiB; shadow mask %dx%d R8, %.1f MiB.", atlas_budget.radiance_width, atlas_budget.radiance_height, radiance_is_packed ? "B10G11R11" : "RGBA16F", double(radiance_bytes) / (1024.0 * 1024.0), mask_extent.x, mask_extent.y, mask_bytes / (1024.0 * 1024.0)));
	}
	return true;
}

Vector<String> NextGIRelight::get_shader_variant_defines(bool p_radiance_packed) {
	Vector<String> defines;
	defines.push_back(p_radiance_packed ? "" : "#define NEXTGI_RADIANCE_RGBA16F\n");
	return defines;
}

bool NextGIRelight::_ensure_shader() {
	if (shader != nullptr && shader->pipeline.is_valid()) {
		return true;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	NextGI *next_gi = NextGI::get_singleton();
	ERR_FAIL_NULL_V(next_gi, false);
	ERR_FAIL_COND_V(!next_gi->can_initialize_ray_query_shaders(), false);

	if (shader == nullptr) {
		shader = memnew(ShaderState);
		shader->shader.initialize(get_shader_variant_defines(radiance_is_packed));
		// GodotNext: shader_baker_ray_query
		// Embedded so the shader baker bakes or skips the ray-query version per target (#40).
		shader->version = shader->shader.version_create(true);
		if (!shader->version.is_valid()) {
			memdelete(shader);
			shader = nullptr;
			ERR_FAIL_V_MSG(false, "The NextGI SRC relight shader failed to compile.");
		}
		next_gi->notify_ray_query_shader_initialized();
	}
	const RID shader_rid = shader->shader.version_get_shader(shader->version, 0);
	shader->pipeline = rd->compute_pipeline_create(shader_rid);
	ERR_FAIL_COND_V(shader->pipeline.is_null(), false);
	return true;
}

// GodotNext: ps_swrt_trace (#186, #188). No can_initialize_ray_query_shaders() check and no
// notify: the sibling has no ray query.
bool NextGIRelight::_ensure_swrt_shader() {
	if (swrt_shader != nullptr && swrt_shader->pipeline.is_valid()) {
		return true;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (swrt_shader == nullptr) {
		swrt_shader = memnew(SWRTShaderState);
		// GodotNext: nextgi_bake_swrt (#261). The same defines as the hardware relight.
		swrt_shader->shader.initialize(get_shader_variant_defines(radiance_is_packed));
		swrt_shader->version = swrt_shader->shader.version_create(false);
		if (!swrt_shader->version.is_valid()) {
			memdelete(swrt_shader);
			swrt_shader = nullptr;
			ERR_FAIL_V_MSG(false, "The NextGI SRC relight software shader failed to compile.");
		}
	}
	swrt_shader->pipeline = rd->compute_pipeline_create(swrt_shader->shader.version_get_shader(swrt_shader->version, 0));
	ERR_FAIL_COND_V(swrt_shader->pipeline.is_null(), false);
	return true;
}

// GodotNext: ps_swrt_trace (#186 step 5)
void NextGIRelight::build_light_candidates(const AABB *p_bounds, uint32_t p_bound_count, const NextGILight *p_lights, uint32_t p_light_count, uint32_t p_cap, LocalVector<uint32_t> &r_data) {
	r_data.resize(2 * p_light_count);
	LocalVector<uint32_t> list;
	for (uint32_t light = 0; light < p_light_count; light++) {
		const NextGILight &source = p_lights[light];
		const int type = int(source.direction_type[3] + 0.5f);
		const float inv_range = source.position_inv_range[3];
		r_data[2 * light] = r_data.size();
		r_data[2 * light + 1] = SWRT_LIGHT_CANDIDATES_OVERFLOW;
		if (type == 0 || !(inv_range > 0.0f)) {
			continue;
		}
		const Vector3 center(source.position_inv_range[0], source.position_inv_range[1], source.position_inv_range[2]);
		const float range = 1.0f / inv_range;
		const float range_sq = range * range;
		list.clear();
		bool overflow = false;
		for (uint32_t index = 0; index < p_bound_count; index++) {
			const AABB &box = p_bounds[index];
			if (box.size == Vector3()) {
				continue;
			}
			const Vector3 closest = center.clamp(box.position, box.position + box.size);
			if ((closest - center).length_squared() > range_sq) {
				continue;
			}
			if (list.size() == p_cap) {
				overflow = true;
				break;
			}
			list.push_back(index);
		}
		if (overflow) {
			continue;
		}
		r_data[2 * light + 1] = list.size();
		for (const uint32_t index : list) {
			r_data.push_back(index);
		}
	}
	if (r_data.is_empty()) {
		r_data.push_back(0);
	}
}

void NextGIRelight::init() {
	_ensure_atlases();
}

RID NextGIRelight::_storage_buffer(RID &r_buffer, uint32_t &r_capacity, uint32_t p_count, uint32_t p_stride) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const uint32_t count = MAX(p_count, 1u);
	if (r_buffer.is_valid() && r_capacity >= count) {
		return r_buffer;
	}
	if (r_buffer.is_valid()) {
		_reset_uniform_set();
		rd->free_rid(r_buffer);
	}
	r_capacity = MAX(count, r_capacity * 2);
	r_buffer = rd->storage_buffer_create(r_capacity * p_stride);
	_reset_uniform_set();
	return r_buffer;
}

// p_free_blocks is false when the caller already freed the set's blocks (a committed placement).
void NextGIRelight::_free_page_set(uint32_t p_index, bool p_free_blocks) {
	ERR_FAIL_UNSIGNED_INDEX(p_index, instances.size());
	const InstancePages &pages = instances[p_index];
	if (p_free_blocks) {
		_free_set_blocks(radiance_buddy, pages);
	}
	alloc_generation++;
	// GodotNext: nextgi_src_page_levels (#247). The page-table run goes back for reuse.
	if (pages.page_table != UINT32_MAX && !pages.cards.is_empty()) {
		free_page_runs[pages.cards.size()].push_back(pages.page_table);
	}
	instance_index.erase(pages.page_key);
	instances.remove_at(p_index);
	for (uint32_t i = p_index; i < instances.size(); i++) {
		instance_index[instances[i].page_key] = i;
	}
	eviction_order_valid = false;
}

// GodotNext: nextgi_src_page_levels (#247)
void NextGIRelight::_free_set_blocks(NextGIBuddyAllocator &r_buddy, const InstancePages &p_pages) const {
	for (const CardPage &page : p_pages.cards) {
		r_buddy.free_block(Rect2i(int(page.radiance_x), int(page.radiance_y), int(page.radiance_w), int(page.radiance_h)));
	}
}

uint32_t NextGIRelight::_take_page_run(uint32_t p_count) {
	LocalVector<uint32_t> *runs = free_page_runs.getptr(p_count);
	if (runs != nullptr && !runs->is_empty()) {
		const uint32_t start = (*runs)[runs->size() - 1];
		runs->resize(runs->size() - 1);
		// The run's probe entries still hold the last owner's radiance.
		probe_resets.push_back(Vector2i(int(start), int(p_count)));
		return start;
	}
	const uint32_t start = page_table_count;
	page_table_count += p_count;
	return start;
}

// The mask rect is the radiance block halved: the relight writes one mask texel per 2x2 radiance texels.
void NextGIRelight::_set_card_rects(InstancePages &r_pages, const LocalVector<Rect2i> &p_rects) {
	ERR_FAIL_COND(p_rects.size() != r_pages.cards.size());
	for (uint32_t i = 0; i < p_rects.size(); i++) {
		CardPage &page = r_pages.cards[i];
		const Rect2i &rect = p_rects[i];
		page.radiance_x = rect.position.x;
		page.radiance_y = rect.position.y;
		page.radiance_w = rect.size.x;
		page.radiance_h = rect.size.y;
		page.mask_x = rect.position.x / 2;
		page.mask_y = rect.position.y / 2;
		page.mask_w = MAX(rect.size.x / 2, 1);
		page.mask_h = MAX(rect.size.y / 2, 1);
		page.dirty_direct = true;
		page.relit = false;
	}
	alloc_generation++;
}

// Stale sets first, then by solid angle, then least recently claimed.
struct NextGIRelightEvictionKey {
	bool stale = false;
	float solid_angle = 0.0f;
	uint64_t last_used = 0;
	uint64_t page_key = 0;
	bool operator()(const NextGIRelightEvictionKey &p_a, const NextGIRelightEvictionKey &p_b) const {
		if (p_a.stale != p_b.stale) {
			return p_a.stale;
		}
		if (p_a.solid_angle != p_b.solid_angle) {
			return p_a.solid_angle < p_b.solid_angle;
		}
		return p_a.last_used < p_b.last_used;
	}
};

void NextGIRelight::_build_eviction_order() {
	LocalVector<NextGIRelightEvictionKey> keys;
	keys.resize(instances.size());
	for (uint32_t i = 0; i < instances.size(); i++) {
		const InstancePages &pages = instances[i];
		NextGIRelightEvictionKey &key = keys[i];
		key.stale = pages.record_pass != record_pass;
		key.solid_angle = key.stale ? 0.0f : pages.solid_angle;
		key.last_used = pages.last_used;
		key.page_key = pages.page_key;
	}
	keys.sort_custom<NextGIRelightEvictionKey>();
	eviction_order.resize(keys.size());
	for (uint32_t i = 0; i < keys.size(); i++) {
		eviction_order[i] = keys[i].page_key;
	}
	eviction_order_valid = true;
}

static bool _nextgi_allocate_sides(NextGIBuddyAllocator &r_buddy, const LocalVector<uint32_t> &p_sides, LocalVector<Rect2i> &r_rects) {
	r_rects.clear();
	for (uint32_t side : p_sides) {
		Rect2i rect;
		if (!r_buddy.allocate(side, side, rect)) {
			return false;
		}
		r_rects.push_back(rect);
	}
	return true;
}

// Places blocks of these sides. When they do not fit, the plan runs on a copy of the allocator:
// stale sets and sets of a lower solid angle (never the set itself) are demoted a level in
// eviction order, then evicted, until the blocks fit. The plan is committed only when they fit, so
// a set that cannot fit changes nothing. p_self_key (a set being re-cut) has its own blocks freed
// in the plan; 0 for a new set.
// r_outranked: set when the blocks did not fit and no set was eligible to make room.
bool NextGIRelight::_place_cards(const LocalVector<uint32_t> &p_sides, float p_solid_angle, uint64_t p_self_key, LocalVector<Rect2i> &r_rects, bool *r_outranked) {
	if (r_outranked != nullptr) {
		*r_outranked = false;
	}
	const uint32_t *self_index = p_self_key != 0 ? instance_index.getptr(p_self_key) : nullptr;
	if (self_index == nullptr) {
		// Fast path: the atlas has room.
		if (_nextgi_allocate_sides(radiance_buddy, p_sides, r_rects)) {
			return true;
		}
		for (const Rect2i &rect : r_rects) {
			radiance_buddy.free_block(rect);
		}
	}
	if (self_index != nullptr) {
		NextGIBuddyAllocator attempt = radiance_buddy;
		_free_set_blocks(attempt, instances[*self_index]);
		if (_nextgi_allocate_sides(attempt, p_sides, r_rects)) {
			radiance_buddy = attempt;
			return true;
		}
	}
	if (!eviction_order_valid) {
		_build_eviction_order();
	}
	// Eligible sets are a prefix of the eviction order.
	LocalVector<uint32_t> candidates;
	for (uint64_t key : eviction_order) {
		const uint32_t *index = instance_index.getptr(key);
		if (index == nullptr || key == p_self_key) {
			continue;
		}
		const InstancePages &pages = instances[*index];
		const bool stale = pages.record_pass != record_pass;
		if (!stale && !(pages.solid_angle < p_solid_angle)) {
			break;
		}
		candidates.push_back(*index);
	}
	// Even evicting every candidate cannot make room: the sets holding the atlas outrank this one.
	uint64_t needed = 0;
	for (uint32_t side : p_sides) {
		needed += uint64_t(side) * uint64_t(side);
	}
	uint64_t available = radiance_buddy.get_free_texels();
	if (self_index != nullptr) {
		for (const CardPage &page : instances[*self_index].cards) {
			available += uint64_t(page.radiance_w) * uint64_t(page.radiance_h);
		}
	}
	for (uint32_t candidate : candidates) {
		for (const CardPage &page : instances[candidate].cards) {
			available += uint64_t(page.radiance_w) * uint64_t(page.radiance_h);
		}
	}
	if (candidates.is_empty() || available < needed) {
		if (r_outranked != nullptr) {
			*r_outranked = true;
		}
		return false;
	}
	NextGIBuddyAllocator plan = radiance_buddy;
	if (self_index != nullptr) {
		_free_set_blocks(plan, instances[*self_index]);
	}
	struct Step {
		uint32_t index = 0;
		bool evict = false;
		LocalVector<Rect2i> rects; // The demoted blocks.
	};
	LocalVector<Step> steps;
	LocalVector<int> step_of; // Per candidate, its step, or -1.
	step_of.resize(candidates.size());
	for (uint32_t i = 0; i < candidates.size(); i++) {
		step_of[i] = -1;
	}
	bool fitted = false;
	// Demote: each step frees three quarters of a set's texels.
	for (uint32_t c = 0; c < candidates.size() && !fitted; c++) {
		const InstancePages &pages = instances[candidates[c]];
		if (pages.record_pass != record_pass || pages.level >= NEXTGI_SRC_PAGE_LEVEL_MAX) {
			continue;
		}
		LocalVector<uint32_t> sides;
		bool shrinks = false;
		for (const CardPage &page : pages.cards) {
			const uint32_t side = MAX(page.full_side >> (pages.level + 1), uint32_t(NEXTGI_SRC_PAGE_MIN));
			shrinks = shrinks || side < page.radiance_w;
			sides.push_back(side);
		}
		if (!shrinks) {
			continue;
		}
		_free_set_blocks(plan, pages);
		Step step;
		step.index = candidates[c];
		if (!_nextgi_allocate_sides(plan, sides, step.rects)) {
			return false; // Freed blocks always hold their halves; the plan is not trusted past this.
		}
		step_of[c] = int(steps.size());
		steps.push_back(step);
		NextGIBuddyAllocator attempt = plan;
		if (_nextgi_allocate_sides(attempt, p_sides, r_rects)) {
			plan = attempt;
			fitted = true;
		}
	}
	// Evict, in the same order.
	for (uint32_t c = 0; c < candidates.size() && !fitted; c++) {
		if (step_of[c] >= 0) {
			Step &step = steps[step_of[c]];
			for (const Rect2i &rect : step.rects) {
				plan.free_block(rect);
			}
			step.evict = true;
		} else {
			_free_set_blocks(plan, instances[candidates[c]]);
			Step step;
			step.index = candidates[c];
			step.evict = true;
			step_of[c] = int(steps.size());
			steps.push_back(step);
		}
		NextGIBuddyAllocator attempt = plan;
		if (_nextgi_allocate_sides(attempt, p_sides, r_rects)) {
			plan = attempt;
			fitted = true;
		}
	}
	if (!fitted) {
		return false;
	}
	// Commit: the plan is the allocator; the steps carry its blocks into the page sets.
	radiance_buddy = plan;
	alloc_generation++;
	LocalVector<uint64_t> evicted;
	for (Step &step : steps) {
		InstancePages &pages = instances[step.index];
		if (step.evict) {
			evicted.push_back(pages.page_key);
			continue;
		}
		_set_card_rects(pages, step.rects);
		pages.level++;
		page_demotions++;
	}
	for (uint64_t key : evicted) {
		const uint32_t *index = instance_index.getptr(key);
		if (index != nullptr) {
			_free_page_set(*index, false);
			page_evictions++;
			page_priority_evictions++;
		}
	}
	eviction_order_valid = false;
	return true;
}

// Moves a set to another level. A coarser level always fits in the set's own blocks; a finer one
// goes through _place_cards() and keeps the set as it is when it does not fit (returns false).
bool NextGIRelight::_recut(uint64_t p_key, uint32_t p_level) {
	const uint32_t *index = instance_index.getptr(p_key);
	if (index == nullptr || instances[*index].level == p_level) {
		return false;
	}
	LocalVector<uint32_t> sides;
	for (const CardPage &page : instances[*index].cards) {
		sides.push_back(MAX(page.full_side >> p_level, uint32_t(NEXTGI_SRC_PAGE_MIN)));
	}
	const float solid_angle = instances[*index].solid_angle;
	LocalVector<Rect2i> rects;
	if (!_place_cards(sides, solid_angle, p_key, rects)) {
		return false;
	}
	// Evictions in the plan may have moved the set.
	index = instance_index.getptr(p_key);
	ERR_FAIL_NULL_V(index, false);
	InstancePages &pages = instances[*index];
	_set_card_rects(pages, rects);
	pages.level = p_level;
	page_recuts++;
	eviction_order_valid = false;
	return true;
}

float NextGIRelight::record_solid_angle(const NextGIInstanceRecord &p_record, float p_extent, const LocalVector<Vector3> &p_cameras) {
	if (p_cameras.is_empty() || p_extent <= 0.0f) {
		return float(Math::PI);
	}
	float max_scale = 0.0f;
	for (int column = 0; column < 3; column++) {
		const Vector3 axis(p_record.object_to_world[column], p_record.object_to_world[4 + column], p_record.object_to_world[8 + column]);
		max_scale = MAX(max_scale, axis.length());
	}
	const Vector3 center(p_record.object_to_world[3], p_record.object_to_world[7], p_record.object_to_world[11]);
	const float radius = 0.5f * float(Math::SQRT3) * p_extent * max_scale;
	float distance = float(Math::INF);
	for (const Vector3 &camera : p_cameras) {
		distance = MIN(distance, camera.distance_to(center));
	}
	return distance <= radius ? float(Math::PI) : nextgi_src_solid_angle(radius, distance);
}

float NextGIRelight::card_set_extent(uint32_t p_card_set) {
	NextGISRC *src = NextGISRC::get_singleton();
	NextGICardSet card_set;
	if (src == nullptr || p_card_set == UINT32_MAX || !src->copy_card_set(p_card_set, card_set)) {
		return 0.0f;
	}
	return card_set.texel_size * 128.0f;
}

void NextGIRelight::touch_pages(RenderGeometryInstance *p_instance, uint32_t p_element, uint32_t p_card_set, float p_solid_angle) {
	if (p_instance == nullptr || p_card_set == UINT32_MAX) {
		return;
	}
	const uint32_t *existing = instance_index.getptr(get_page_key(p_instance, p_element));
	if (existing == nullptr || instances[*existing].card_set != p_card_set) {
		return;
	}
	InstancePages &pages = instances[*existing];
	if (pages.record_pass != record_pass || pages.solid_angle != p_solid_angle) {
		pages.record_pass = record_pass;
		pages.solid_angle = p_solid_angle;
		eviction_order_valid = false;
	}
}

void NextGIRelight::begin_record_pass() {
	record_pass++;
	// GodotNext: nextgi_src_page_levels (#247). The pass changes every set's stale flag.
	pass_unplaced = 0;
	eviction_order_valid = false;
	pass_record_keys.clear();
}

void NextGIRelight::finish_record_pass(LocalVector<NextGIInstanceRecord> &r_records) const {
	for (uint32_t i = 0; i < r_records.size() && i < pass_record_keys.size(); i++) {
		NextGIInstanceRecord &record = r_records[i];
		if (pass_record_keys[i] == 0 || record.page_table == UINT32_MAX || (record.page_table & 0x80000000u) != 0) {
			continue;
		}
		const uint32_t *set = instance_index.getptr(pass_record_keys[i]);
		record.page_table = set != nullptr && instances[*set].record_pass == record_pass ? instances[*set].page_table : 0x80000000u;
	}
}

void NextGIRelight::set_page_extent(const NextGIAtlasBudget &p_budget) {
	radiance_buddy.set_extent(p_budget.radiance_width, p_budget.radiance_height);
	// GodotNext: nextgi_src_page_levels (#247). Half the radiance extent: a card's mask is its radiance block halved.
	mask_extent = Size2i(int(p_budget.radiance_width / 2), int(p_budget.radiance_height / 2));
	instances.clear();
	instance_index.clear();
	pass_record_keys.clear();
	free_page_runs.clear();
	probe_resets.clear();
	eviction_order.clear();
	eviction_order_valid = false;
	page_table_count = 0;
	page_rect_uploaded = 0;
}

void NextGIRelight::collect_current_page_sets(LocalVector<uint32_t> &r_sets) const {
	r_sets.clear();
	for (uint32_t i = 0; i < instances.size(); i++) {
		if (instances[i].record_pass == record_pass) {
			r_sets.push_back(i);
		}
	}
}

NextGIRelight::PageSetInfo NextGIRelight::get_page_set_info(uint32_t p_set) const {
	PageSetInfo info;
	ERR_FAIL_UNSIGNED_INDEX_V(p_set, instances.size(), info);
	const InstancePages &pages = instances[p_set];
	info.record_index = pages.record_index;
	info.card_set = pages.card_set;
	info.page_table = pages.page_table;
	info.level = pages.level;
	for (const CardPage &page : pages.cards) {
		info.radiance_rects.push_back(Rect2i(int(page.radiance_x), int(page.radiance_y), int(page.radiance_w), int(page.radiance_h)));
		info.mask_rects.push_back(Rect2i(int(page.mask_x), int(page.mask_y), int(page.mask_w), int(page.mask_h)));
	}
	return info;
}

static uint64_t _nextgi_page_key(RenderGeometryInstance *p_instance, uint32_t p_element) {
	return uint64_t(uintptr_t(p_instance)) ^ (uint64_t(p_element) * 0x9E3779B97F4A7C15ULL);
}

uint32_t NextGIRelight::get_instance_page_table(RenderGeometryInstance *p_instance, uint32_t p_element) const {
	if (p_instance == nullptr) {
		return UINT32_MAX;
	}
	const uint32_t *existing = instance_index.getptr(_nextgi_page_key(p_instance, p_element));
	if (existing == nullptr || *existing >= instances.size()) {
		return UINT32_MAX;
	}
	// GodotNext: nextgi_src_page_levels (#247). A set outside the current records is not relit, so
	// its radiance is frozen: it reads as unpaged (the evicted mark) until a pass claims it again.
	if (instances[*existing].record_pass != record_pass) {
		return 0x80000000u;
	}
	return instances[*existing].page_table;
}

void NextGIRelight::release_instance(RenderGeometryInstance *p_instance) {
	if (p_instance == nullptr) {
		return;
	}
	const uint64_t page_key = _nextgi_page_key(p_instance, UINT32_MAX);
	const uint32_t *existing = instance_index.getptr(page_key);
	if (existing == nullptr) {
		return;
	}
	_free_page_set(*existing);
}

uint32_t NextGIRelight::assign_pages(RenderGeometryInstance *p_instance, NextGIInstanceRecord &r_record, uint32_t p_element, float p_solid_angle) {
	if (p_instance == nullptr || r_record.card_set == UINT32_MAX) {
		return UINT32_MAX;
	}
	if (!_ensure_atlases()) {
		return UINT32_MAX;
	}
	NextGISRC *src = NextGISRC::get_singleton();
	if (src == nullptr) {
		return UINT32_MAX;
	}
	// GodotNext: ps_lumen_lite (#183). A set cut for the same card set is reused without the cards.
	const uint32_t *existing = instance_index.getptr(_nextgi_page_key(p_instance, p_element));
	if (existing != nullptr && instances[*existing].card_set == r_record.card_set) {
		return assign_card_pages(p_instance, r_record, p_element, 0, nullptr, 0, p_solid_angle);
	}
	NextGICardSet card_set;
	if (!src->copy_card_set(r_record.card_set, card_set) || card_set.card_count == 0) {
		return UINT32_MAX;
	}
	LocalVector<NextGICard> cards;
	cards.resize(card_set.card_count);
	for (uint32_t i = 0; i < card_set.card_count; i++) {
		if (!src->copy_card(card_set.first_card + i, cards[i])) {
			alloc_failures++;
			alloc_failures_card++;
			return 0x80000000u;
		}
	}
	return assign_card_pages(p_instance, r_record, p_element, card_set.first_card, cards.ptr(), cards.size(), p_solid_angle);
}

uint32_t NextGIRelight::assign_card_pages(RenderGeometryInstance *p_instance, NextGIInstanceRecord &r_record, uint32_t p_element, uint32_t p_first_card, const NextGICard *p_cards, uint32_t p_card_count, float p_solid_angle) {
	if (p_instance == nullptr || r_record.card_set == UINT32_MAX) {
		return UINT32_MAX;
	}
	const uint64_t page_key = _nextgi_page_key(p_instance, p_element);
	if (r_record.tlas_index >= pass_record_keys.size()) {
		const uint32_t old_size = pass_record_keys.size();
		pass_record_keys.resize(r_record.tlas_index + 1);
		for (uint32_t i = old_size; i < pass_record_keys.size(); i++) {
			pass_record_keys[i] = 0;
		}
	}
	pass_record_keys[r_record.tlas_index] = page_key;
	const uint32_t *existing = instance_index.getptr(page_key);
	if (existing != nullptr) {
		if (instances[*existing].card_set == r_record.card_set) {
			InstancePages &pages = instances[*existing];
			if (pages.record_pass + 1 < record_pass) {
				// Back after a pass outside the records: the pages missed every light change since.
				for (CardPage &card_page : pages.cards) {
					card_page.dirty_direct = true;
				}
			}
			if (pages.record_pass != record_pass || pages.solid_angle != p_solid_angle) {
				eviction_order_valid = false;
			}
			pages.record = r_record;
			pages.record_index = r_record.tlas_index;
			pages.record_pass = record_pass;
			pages.solid_angle = p_solid_angle;
			pages.last_used = page_clock++;
			// GodotNext: nextgi_src_page_levels (#247). A level change re-cuts the set in place: the
			// page table stays, the rects move and the pages are relit.
			const uint32_t wanted = nextgi_src_page_level_hysteresis(p_solid_angle, pages.level);
			const uint32_t page_table = pages.page_table;
			const bool promote = wanted < pages.level;
			const bool retry = !promote || pages.promote_failed_generation != alloc_generation || p_solid_angle > pages.promote_failed_angle;
			if (wanted != pages.level && retry && !_recut(page_key, wanted) && promote) {
				const uint32_t *index = instance_index.getptr(page_key);
				if (index != nullptr) {
					instances[*index].promote_failed_generation = alloc_generation;
					instances[*index].promote_failed_angle = p_solid_angle;
				}
			}
			r_record.page_table = page_table;
			return page_table;
		}
		// The key now names another card set (a new material key, or a freed instance's address
		// reused). Its pages are cut for the old cards, so they go.
		_free_page_set(*existing);
	}
	if (p_cards == nullptr || p_card_count == 0) {
		return UINT32_MAX;
	}

	InstancePages pages;
	pages.instance = p_instance;
	pages.element = p_element;
	pages.page_key = page_key;
	pages.record = r_record;
	pages.record_index = r_record.tlas_index;
	pages.card_set = r_record.card_set;
	pages.record_pass = record_pass;
	pages.solid_angle = p_solid_angle;
	// GodotNext: nextgi_src_page_levels (#247). Every card of the set takes the set's level.
	pages.level = nextgi_src_page_level_hysteresis(p_solid_angle, UINT32_MAX);
	LocalVector<uint32_t> sides;
	for (uint32_t i = 0; i < p_card_count; i++) {
		const NextGICard &card = p_cards[i];
		CardPage page;
		page.card_index = p_first_card + i;
		page.full_side = MAX(Math::next_power_of_2(MAX(MAX(card.atlas_rect[2], card.atlas_rect[3]), 1u)), uint32_t(NEXTGI_SRC_PAGE_MIN));
		pages.cards.push_back(page);
		sides.push_back(MAX(page.full_side >> pages.level, uint32_t(NEXTGI_SRC_PAGE_MIN)));
	}
	LocalVector<Rect2i> rects;
	bool outranked = false;
	if (!_place_cards(sides, p_solid_angle, 0, rects, &outranked)) {
		// No room at its level, even by priority: a coarser level in the space that is free, so a
		// far set holds a small page rather than none. It is promoted once room appears.
		bool placed = false;
		for (uint32_t level = pages.level + 1; level <= uint32_t(NEXTGI_SRC_PAGE_LEVEL_MAX) && !placed; level++) {
			LocalVector<uint32_t> coarser;
			for (const CardPage &page : pages.cards) {
				coarser.push_back(MAX(page.full_side >> level, uint32_t(NEXTGI_SRC_PAGE_MIN)));
			}
			if (_nextgi_allocate_sides(radiance_buddy, coarser, rects)) {
				pages.level = level;
				placed = true;
			} else {
				for (const Rect2i &rect : rects) {
					radiance_buddy.free_block(rect);
				}
			}
		}
		if (!placed) {
			if (outranked) {
				pass_unplaced++;
			} else {
				alloc_failures++;
				alloc_failures_radiance++;
			}
			return 0x80000000u;
		}
	}
	_set_card_rects(pages, rects);
	pages.page_table = _take_page_run(pages.cards.size());
	for (uint32_t i = 0; i < pages.cards.size(); i++) {
		pages.cards[i].stable_id = pages.page_table + i;
	}
	pages.last_used = page_clock++;
	r_record.page_table = pages.page_table;
	instance_index.insert(page_key, instances.size());
	instances.push_back(pages);
	eviction_order_valid = false;
	return pages.page_table;
}

void NextGIRelight::relight(RenderDataRD *p_render_data) {
	ERR_FAIL_NULL(p_render_data);
	last_texels = 0;
	// GodotNext: nextgi_async_counters (#186). last_rays and last_rays_skipped come from the
	// asynchronous readback of an earlier frame, so they are not cleared here.
	last_unshadowed = 0;
	last_light_count = 0;
	last_directional = 0;
	last_dynamic = 0;
	last_static = 0;
	scheduled_pages.clear();
	probe_rgb.clear();
	mask_pages.clear();
	// GodotNext: ps_swrt_trace (#186, #188). The software tier runs the same kernel through its
	// sibling, whose shadow rays march the global SDF; its records come from the SWRT record pass.
	// The tier branch comes first: the ray-query ShaderRD is never created on that tier.
	NextGI *next_gi = NextGI::get_singleton();
	const bool swrt = next_gi != nullptr && next_gi->get_tier_result().tier == NextGI::TIER_SOFTWARE;
	if (p_render_data->sdfgi_update_data == nullptr || !_ensure_atlases() || !(swrt ? _ensure_swrt_shader() : _ensure_shader())) {
		return;
	}
	NextGISRC *src = NextGISRC::get_singleton();
	NextGISWRTTrace *swrt_trace = swrt ? next_gi->get_swrt_trace() : nullptr;
	if (next_gi == nullptr || (swrt ? swrt_trace == nullptr : next_gi->get_rt_scene() == nullptr) || src == nullptr || src->get_card_buffer().is_null()) {
		return;
	}
	const RID tlas = swrt ? RID() : next_gi->get_rt_scene()->get_tlas();
	const NextGIFarField *far_field = swrt ? nullptr : next_gi->get_rt_scene()->get_far_field();
	const RID record_buffer = swrt ? swrt_trace->get_record_buffer() : next_gi->get_rt_scene()->get_instance_record_buffer();
	const bool near_ready = swrt ? swrt_trace->is_ready() && record_buffer.is_valid() : tlas.is_valid() && next_gi->get_rt_scene()->is_tlas_traceable();
	const bool far_ready = far_field != nullptr && far_field->is_trace_ready();
	if (instances.is_empty() || (!near_ready && !far_ready)) {
		return;
	}

	RenderingDevice *rd = RenderingDevice::get_singleton();
	LightStorage *light_storage = LightStorage::get_singleton();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_NULL(light_storage);

	float exposure = 1.0f;
	if (p_render_data->camera_attributes.is_valid() && RSG::camera_attributes != nullptr) {
		exposure = RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
	}
	const bool physical = RendererSceneRenderRD::get_singleton() != nullptr && RendererSceneRenderRD::get_singleton()->is_using_physical_light_units();

	struct CpuLight {
		NextGILight gpu;
		RID instance;
		bool directional = false;
		bool is_static = false;
		bool has_shadow = false;
	};
	LocalVector<CpuLight> cpu_lights;

	auto add_light = [&](RID p_light_instance, bool p_directional, bool p_static) {
		if (!light_storage->owns_light_instance(p_light_instance)) {
			return;
		}
		const RID light = light_storage->light_instance_get_base_light(p_light_instance);
		const RSE::LightBakeMode bake = RSG::light_storage->light_get_bake_mode(light);
		if (bake == RSE::LIGHT_BAKE_DISABLED) {
			return;
		}
		if (p_directional && RSG::light_storage->light_directional_get_sky_mode(light) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
			return;
		}
		const Transform3D transform = light_storage->light_instance_get_base_transform(p_light_instance);
		const Vector3 travel = -transform.basis.get_column(Vector3::AXIS_Z).normalized();
		Color color = RSG::light_storage->light_get_color(light).srgb_to_linear();
		float energy = RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY) * RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
		const RSE::LightType type = RSG::light_storage->light_get_type(light);
		if (physical) {
			energy *= RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_INTENSITY);
			if (type == RSE::LIGHT_OMNI) {
				energy *= 1.0f / (Math::PI * 4.0f);
			} else if (type == RSE::LIGHT_SPOT) {
				energy *= 1.0f / Math::PI;
			}
		}
		energy *= exposure;
		const float range = RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_RANGE);
		const float spot_atten = RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPOT_ATTENUATION);

		CpuLight cpu;
		cpu.instance = p_light_instance;
		cpu.directional = p_directional;
		cpu.is_static = p_static;
		cpu.has_shadow = RSG::light_storage->light_has_shadow(light);
		cpu.gpu.color_energy[0] = color.r;
		cpu.gpu.color_energy[1] = color.g;
		cpu.gpu.color_energy[2] = color.b;
		cpu.gpu.color_energy[3] = energy;
		cpu.gpu.position_inv_range[0] = transform.origin.x;
		cpu.gpu.position_inv_range[1] = transform.origin.y;
		cpu.gpu.position_inv_range[2] = transform.origin.z;
		cpu.gpu.position_inv_range[3] = range > 0.0f ? 1.0f / range : 0.0f;
		cpu.gpu.direction_type[0] = travel.x;
		cpu.gpu.direction_type[1] = travel.y;
		cpu.gpu.direction_type[2] = travel.z;
		cpu.gpu.direction_type[3] = float(type);
		cpu.gpu.attenuation = RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_ATTENUATION);
		cpu.gpu.cos_spot = Math::cos(Math::deg_to_rad(RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPOT_ANGLE)));
		cpu.gpu.inv_spot_atten = spot_atten > 0.0f ? 1.0f / spot_atten : 1.0f;
		cpu.gpu.flags = uint32_t(NEXTGI_LIGHT_REUSE_NONE) << NEXTGI_LIGHT_REUSE_SHIFT;
		if (cpu.has_shadow) {
			cpu.gpu.flags |= NEXTGI_LIGHT_FLAG_SHADOW;
		}
		if (p_static) {
			cpu.gpu.flags |= NEXTGI_LIGHT_FLAG_STATIC;
		}
		cpu_lights.push_back(cpu);
		if (p_directional) {
			last_directional++;
		} else if (p_static) {
			last_static++;
		} else {
			last_dynamic++;
		}
	};

	const RendererSceneRender::RenderSDFGIUpdateData *feed = p_render_data->sdfgi_update_data;
	if (feed->directional_lights != nullptr) {
		for (int i = 0; i < feed->directional_lights->size(); i++) {
			add_light(feed->directional_lights->get(i), true, false);
		}
	}
	for (uint32_t i = 0; i < feed->positional_light_count; i++) {
		add_light(feed->positional_light_instances[i], false, false);
	}
	for (uint32_t i = 0; i < feed->nextgi_static_light_count; i++) {
		add_light(feed->nextgi_static_light_instances[i], false, true);
	}
	last_light_count = cpu_lights.size();

	LocalVector<NextGIShadowReuse> reuses;
	const uint64_t scene_pass = RendererSceneRenderRD::get_singleton()->get_scene_pass();
	for (CpuLight &cpu : cpu_lights) {
		if (!cpu.has_shadow) {
			continue;
		}
		const bool rendered = light_storage->light_instance_get_shadow_pass(cpu.instance) == scene_pass;
		const uint32_t reuse_index = reuses.size();
		uint32_t written = 0;
		auto push_entry = [&](const Projection &p_camera, const Rect2 &p_atlas, uint32_t p_source, int p_split) {
			if (p_atlas.size.x <= 0.0f || p_atlas.size.y <= 0.0f) {
				return;
			}
			NextGIShadowReuse entry;
			_projection_to_columns(_world_shadow_matrix(p_camera, light_storage->light_instance_get_shadow_transform(cpu.instance, p_split), p_atlas), entry.matrix);
			entry.atlas[0] = p_atlas.position.x;
			entry.atlas[1] = p_atlas.position.y;
			entry.atlas[2] = p_atlas.size.x;
			entry.atlas[3] = p_atlas.size.y;
			const float bias_scale = light_storage->light_instance_get_shadow_bias_scale(cpu.instance, p_split);
			const RID base = light_storage->light_instance_get_base_light(cpu.instance);
			entry.bias = RSG::light_storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_BIAS) / 100.0f * bias_scale;
			entry.source = p_source;
			// GodotNext: nextgi_relight_shadow_range (#258)
			if (p_source == NEXTGI_REUSE_DIRECTIONAL && p_render_data->scene_data != nullptr) {
				const Vector3 camera_origin = p_render_data->scene_data->cam_transform.origin;
				entry.range_origin[0] = camera_origin.x;
				entry.range_origin[1] = camera_origin.y;
				entry.range_origin[2] = camera_origin.z;
				entry.range_origin[3] = nextgi_directional_shadow_range(RSG::light_storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_MAX_DISTANCE), p_render_data->scene_data->z_far, p_render_data->scene_data->cam_orthogonal);
			}
			reuses.push_back(entry);
			written++;
		};
		if (rendered && cpu.directional && light_storage->directional_shadow_get_texture().is_valid()) {
			for (int split = 0; split < 4; split++) {
				push_entry(light_storage->light_instance_get_shadow_camera(cpu.instance, split), light_storage->light_instance_get_directional_shadow_atlas_rect(cpu.instance, split), NEXTGI_REUSE_DIRECTIONAL, split);
			}
		} else if (rendered && !cpu.directional && p_render_data->shadow_atlas.is_valid() && light_storage->shadow_atlas_owns_light_instance(p_render_data->shadow_atlas, cpu.instance)) {
			Vector2i omni_offset;
			const Rect2 atlas = light_storage->light_instance_get_shadow_atlas_rect(cpu.instance, p_render_data->shadow_atlas, omni_offset);
			push_entry(light_storage->light_instance_get_shadow_camera(cpu.instance, 0), atlas, NEXTGI_REUSE_POSITIONAL, 0);
		}
		if (written == 0) {
			continue;
		}
		cpu.gpu.flags = (cpu.gpu.flags & 0xFFu) | (reuse_index << NEXTGI_LIGHT_REUSE_SHIFT);
		for (; written < 4; written++) {
			NextGIShadowReuse empty;
			reuses.push_back(empty);
		}
	}

	struct FlatPage {
		uint32_t instance = 0;
		uint32_t card = 0;
		uint32_t texels = 0;
	};
	const RendererSceneRender::RenderSDFGIUpdateData *work_events = p_render_data->sdfgi_update_data;
	const uint32_t shadow_lights = MIN(last_directional + last_dynamic + last_static, 2u);
	const Vector3 camera = p_render_data->scene_data != nullptr ? p_render_data->scene_data->cam_transform.origin : Vector3();
	LocalVector<FlatPage> flat;
	LocalVector<NextGISrcSchedulePage> work;
	last_dirty_pages = 0;
	last_max_age = 0;
	// GodotNext: ps_lumen_lite (#183). Only sets in this frame's records: another set's
	// record_index now names some other instance, which may have no card set at all.
	LocalVector<uint32_t> current_sets;
	collect_current_page_sets(current_sets);
	for (uint32_t i : current_sets) {
		InstancePages &instance_pages = instances[i];
		float max_scale = 0.0f;
		for (int row = 0; row < 3; row++) {
			const Vector3 axis(instance_pages.record.object_to_world[row * 4], instance_pages.record.object_to_world[row * 4 + 1], instance_pages.record.object_to_world[row * 4 + 2]);
			max_scale = MAX(max_scale, axis.length());
		}
		if (nextgi_src_scale_changed(instance_pages.captured_scale, max_scale)) {
			src->invalidate_captures(instance_pages.record.card_set);
		}
		if (max_scale > 0.0f) {
			instance_pages.captured_scale = max_scale;
		}
		for (uint32_t c = 0; c < instance_pages.cards.size(); c++) {
			CardPage &card_page = instance_pages.cards[c];
			NextGICard card;
			AABB bounds;
			Vector3 center = camera;
			if (src->copy_card(card_page.card_index, card)) {
				bounds = _card_world_aabb(card, instance_pages.record);
				center = bounds.get_center();
				const bool directional = work_events != nullptr && work_events->nextgi_directional_light_dirty;
				bool dirty = directional;
				if (work_events != nullptr) {
					for (uint32_t v = 0; v < work_events->nextgi_dirty_volume_count && !dirty; v++) {
						const AABB swept = work_events->nextgi_dirty_volumes[v];
						dirty = bounds.intersects(swept);
						if (!dirty) {
							for (uint32_t light_index = 0; light_index < cpu_lights.size() && !dirty; light_index++) {
								const CpuLight &cpu = cpu_lights[light_index];
								if (cpu.directional) {
									dirty = bounds.intersects(nextgi_src_directional_sweep(swept, Vector3(cpu.gpu.direction_type[0], cpu.gpu.direction_type[1], cpu.gpu.direction_type[2]), float(GLOBAL_GET("rendering/global_illumination/nextgi/rt_scene_radius"))));
								} else {
									const float range = cpu.gpu.position_inv_range[3] > 0.0f ? 1.0f / cpu.gpu.position_inv_range[3] : 0.0f;
									dirty = nextgi_src_local_light_dirties_page(bounds, swept, Vector3(cpu.gpu.position_inv_range[0], cpu.gpu.position_inv_range[1], cpu.gpu.position_inv_range[2]), range);
								}
							}
						}
					}
					for (uint32_t v = 0; v < work_events->nextgi_light_volume_count && !dirty; v++) {
						dirty = bounds.intersects(work_events->nextgi_light_volumes[v]);
					}
				}
				card_page.dirty_direct = card_page.dirty_direct || dirty;
				AABB grown = bounds;
				grown.grow_by(1.0f);
				card_page.near_dirty = false;
				if (work_events != nullptr) {
					for (uint32_t v = 0; v < work_events->nextgi_dirty_volume_count && !card_page.near_dirty; v++) {
						card_page.near_dirty = grown.intersects(work_events->nextgi_dirty_volumes[v]);
					}
					for (uint32_t v = 0; v < work_events->nextgi_light_volume_count && !card_page.near_dirty; v++) {
						card_page.near_dirty = grown.intersects(work_events->nextgi_light_volumes[v]);
					}
				}
			}
			FlatPage page;
			page.instance = i;
			page.card = c;
			page.texels = card_page.radiance_w * card_page.radiance_h;
			flat.push_back(page);
			NextGISrcSchedulePage item;
			item.texels = page.texels;
			item.ray_estimate = nextgi_src_page_ray_estimate(page.texels, shadow_lights, card_page.ray_fraction);
			item.last_relit_frame = card_page.last_relit_frame;
			const float distance = MAX(center.distance_to(camera), 0.5f);
			const bool far_page = (instance_pages.record.rt_flags & NEXTGI_RT_FLAG_FAR) != 0;
			item.far_field = far_page;
			item.priority = far_page ? 0.0f : float(page.texels) / (distance * distance);
			item.dirty_direct = card_page.dirty_direct && !far_page;
			item.near_dirty = card_page.near_dirty && !far_page;
			work.push_back(item);
			if (card_page.dirty_direct) {
				last_dirty_pages++;
			}
			if (card_page.last_relit_frame > 0 && frame >= card_page.last_relit_frame) {
				last_max_age = MAX(last_max_age, frame - card_page.last_relit_frame);
			}
		}
	}
	flat_page_count = flat.size();
	LocalVector<uint32_t> scheduled;
	scheduled.resize(flat.size());
	const uint32_t budget = nextgi_src_budget_for_quality(int(next_gi->get_resolved_quality()));
	const uint32_t scheduled_count = flat.is_empty() ? 0 : nextgi_src_schedule_streams(work.ptr(), flat.size(), budget, budget, scheduled.ptr(), scheduled.size());
	schedule_cursor = flat.is_empty() ? 0 : (schedule_cursor + scheduled_count) % flat.size();

	LocalVector<NextGILight> gpu_lights;
	gpu_lights.resize(MAX(cpu_lights.size(), 1u));
	for (uint32_t i = 0; i < cpu_lights.size(); i++) {
		gpu_lights[i] = cpu_lights[i].gpu;
	}
	LocalVector<NextGIRelightPageGPU> gpu_pages;
	LocalVector<NextGIRelightPageGPU> far_gpu_pages;
	LocalVector<uint32_t> gpu_indices;
	uint32_t max_w = 1;
	uint32_t max_h = 1;
	for (uint32_t s = 0; s < scheduled_count; s++) {
		const FlatPage &flat_page = flat[scheduled[s]];
		InstancePages &instance_pages = instances[flat_page.instance];
		CardPage &card_page = instance_pages.cards[flat_page.card];
		card_page.dirty_direct = false;
		card_page.relit = true; // GodotNext: nextgi_src_page_levels (#247)
		card_page.last_relit_frame = frame;
		NextGICard card;
		if (!src->copy_card(card_page.card_index, card)) {
			continue;
		}
		const AABB bounds = _card_world_aabb(card, instance_pages.record);
		const Vector3 center = _card_world(card, instance_pages.record, Vector3(card.origin[0], card.origin[1], card.origin[2]));
		const Vector3 normal = _record_basis(instance_pages.record).xform(Vector3(card.axis_n[0], card.axis_n[1], card.axis_n[2])).normalized();

		LocalVector<NextGIPageLightCandidate> candidates;
		for (uint32_t light_index = 0; light_index < cpu_lights.size(); light_index++) {
			const CpuLight &cpu = cpu_lights[light_index];
			if (!nextgi_light_reaches_receiver(cpu.is_static, instance_pages.record.rt_flags)) {
				continue;
			}
			if (!cpu.directional) {
				const AABB light_bounds = light_storage->light_instance_get_base_aabb(cpu.instance);
				if (!bounds.intersects(light_bounds)) {
					continue;
				}
			}
			const Vector3 light_position(cpu.gpu.position_inv_range[0], cpu.gpu.position_inv_range[1], cpu.gpu.position_inv_range[2]);
			const Vector3 travel(cpu.gpu.direction_type[0], cpu.gpu.direction_type[1], cpu.gpu.direction_type[2]);
			float atten = 1.0f;
			float ndotl = 0.0f;
			if (cpu.directional) {
				ndotl = normal.dot(-travel);
			} else {
				const Vector3 delta = light_position - center;
				const float distance = delta.length();
				const Vector3 to_light = distance > 0.0f ? delta / distance : Vector3(0, 1, 0);
				ndotl = normal.dot(to_light);
				const int type = int(cpu.gpu.direction_type[3] + 0.5f);
				if (type == RSE::LIGHT_SPOT) {
					atten = nextgi_spot_attenuation(distance, cpu.gpu.position_inv_range[3], cpu.gpu.attenuation, (-to_light).dot(travel), cpu.gpu.cos_spot, cpu.gpu.inv_spot_atten);
				} else {
					atten = nextgi_omni_attenuation(distance, cpu.gpu.position_inv_range[3], cpu.gpu.attenuation);
				}
			}
			const float rank = cpu.gpu.color_energy[3] * atten * MAX(ndotl, 0.0f);
			if (rank <= 0.0f) {
				continue;
			}
			NextGIPageLightCandidate candidate;
			candidate.light_index = light_index;
			candidate.energy = rank;
			candidate.has_shadow = cpu.has_shadow;
			candidates.push_back(candidate);
		}
		LocalVector<uint32_t> shadowed;
		LocalVector<uint32_t> unshadowed;
		nextgi_select_page_lights(candidates.ptr(), candidates.size(), NEXTGI_SRC_MAX_SHADOWED_LIGHTS, shadowed, unshadowed);
		max_shadowed = MAX(max_shadowed, shadowed.size());
		max_mask_evals = MAX(max_mask_evals, MIN(shadowed.size(), uint32_t(NEXTGI_SRC_MASK_EVALS_PER_FRAME)));
		last_unshadowed += unshadowed.size();

		NextGIRelightPageGPU gpu_page;
		gpu_page.record_index = instance_pages.record_index;
		gpu_page.card_index = card_page.card_index;
		gpu_page.radiance_origin = _pack_xy(card_page.radiance_x, card_page.radiance_y);
		gpu_page.radiance_size = _pack_xy(card_page.radiance_w, card_page.radiance_h);
		gpu_page.mask_origin = _pack_xy(card_page.mask_x, card_page.mask_y);
		gpu_page.mask_size = _pack_xy(card_page.mask_w, card_page.mask_h);
		gpu_page.light_offset = gpu_indices.size();
		gpu_page.shadowed_count = shadowed.size();
		gpu_page.light_count = shadowed.size() + unshadowed.size();
		gpu_page.stable_id = card_page.stable_id;
		for (uint32_t index : shadowed) {
			gpu_indices.push_back(index);
		}
		for (uint32_t index : unshadowed) {
			gpu_indices.push_back(index);
		}
		if ((instance_pages.record.rt_flags & NEXTGI_RT_FLAG_FAR) != 0) {
			far_gpu_pages.push_back(gpu_page);
		} else {
			gpu_pages.push_back(gpu_page);
		}
		const uint32_t blocks_x = MAX((card_page.radiance_w + 7u) / 8u, 1u);
		const uint32_t blocks_y = MAX((card_page.radiance_h + 7u) / 8u, 1u);
		for (uint32_t by = 0; by < blocks_y; by++) {
			for (uint32_t bx = 0; bx < blocks_x; bx++) {
				ScheduledPage scheduled_page;
				scheduled_page.indirect_x = (card_page.radiance_x / 8u) + bx;
				scheduled_page.indirect_y = (card_page.radiance_y / 8u) + by;
				scheduled_page.record_index = instance_pages.record_index;
				scheduled_page.card_index = card_page.card_index;
				scheduled_pages.push_back(scheduled_page);
			}
		}
		last_texels += card_page.radiance_w * card_page.radiance_h;
		max_w = MAX(max_w, card_page.radiance_w);
		max_h = MAX(max_h, card_page.radiance_h);
	}
	max_texels = MAX(max_texels, last_texels);

	_storage_buffer(light_buffer, light_capacity, gpu_lights.size(), sizeof(NextGILight));
	_storage_buffer(reuse_buffer, reuse_capacity, MAX(reuses.size(), 1u), sizeof(NextGIShadowReuse));
	_storage_buffer(page_buffer, page_capacity, MAX(gpu_pages.size() + far_gpu_pages.size(), 1u), sizeof(NextGIRelightPageGPU));
	_storage_buffer(index_buffer, index_capacity, MAX(gpu_indices.size(), 1u), sizeof(uint32_t));

	if (!probe_buffer.is_valid() || probe_capacity < MAX(page_table_count, 1u)) {
		if (probe_buffer.is_valid()) {
			_reset_uniform_set();
			rd->free_rid(probe_buffer);
		}
		probe_capacity = MAX(page_table_count, 1u);
		probe_buffer = rd->storage_buffer_create(probe_capacity * sizeof(float) * 4);
		_reset_uniform_set();
	}
	// GodotNext: nextgi_src_page_levels (#247). Reused page-table runs start without the last owner's probes.
	for (const Vector2i &reset : probe_resets) {
		if (uint32_t(reset.x + reset.y) <= probe_capacity) {
			rd->buffer_clear(probe_buffer, uint32_t(reset.x) * sizeof(float) * 4, uint32_t(reset.y) * sizeof(float) * 4);
		}
	}
	probe_resets.clear();
	if (!params_buffer.is_valid()) {
		params_buffer = rd->storage_buffer_create(sizeof(NextGIRelightParamsGPU));
		_reset_uniform_set();
	}
	if (!counter_buffer.is_valid()) {
		counter_buffer = rd->storage_buffer_create(sizeof(NextGIRelightCountersGPU));
		_reset_uniform_set();
	}

	if (!gpu_lights.is_empty()) {
		rd->buffer_update(light_buffer, 0, gpu_lights.size() * sizeof(NextGILight), gpu_lights.ptr());
	}
	if (!reuses.is_empty()) {
		rd->buffer_update(reuse_buffer, 0, reuses.size() * sizeof(NextGIShadowReuse), reuses.ptr());
	} else {
		NextGIShadowReuse empty;
		rd->buffer_update(reuse_buffer, 0, sizeof(empty), &empty);
	}
	if (!gpu_pages.is_empty()) {
		rd->buffer_update(page_buffer, 0, gpu_pages.size() * sizeof(NextGIRelightPageGPU), gpu_pages.ptr());
	}
	if (!far_gpu_pages.is_empty()) {
		rd->buffer_update(page_buffer, gpu_pages.size() * sizeof(NextGIRelightPageGPU), far_gpu_pages.size() * sizeof(NextGIRelightPageGPU), far_gpu_pages.ptr());
	}
	if (!gpu_indices.is_empty()) {
		rd->buffer_update(index_buffer, 0, gpu_indices.size() * sizeof(uint32_t), gpu_indices.ptr());
	}
	NextGIRelightCountersGPU zero_counters;
	rd->buffer_update(counter_buffer, 0, sizeof(zero_counters), &zero_counters);
	const float rt_radius = float(GLOBAL_GET("rendering/global_illumination/nextgi/rt_scene_radius"));
	NextGIRelightParamsGPU params;
	params.frame = frame;
	params.ray_budget = NextGI::get_singleton() != nullptr ? NextGI::get_quality_params(NextGI::get_singleton()->get_quality_setting()).relight_shadow_rays : uint32_t(NEXTGI_SRC_RELIGHT_RAY_CAP);
	params.page_count = gpu_pages.size() + far_gpu_pages.size();
	params.rt_radius = rt_radius > 0.0f ? rt_radius : 1.0f;
	rd->buffer_update(params_buffer, 0, sizeof(params), &params);

	// GodotNext: ps_swrt_trace (#186 step 5). The software relight's per-light candidate lists,
	// rebuilt when the lights or the software instances change (set 0, binding 0).
	if (swrt) {
		const uint32_t generation_hash = hash_murmur3_one_64(swrt_trace->get_instance_generation());
		const uint32_t lights_hash = gpu_lights.is_empty() ? 0u : hash_murmur3_buffer(gpu_lights.ptr(), int(gpu_lights.size() * sizeof(NextGILight)), generation_hash);
		const uint64_t key = (uint64_t(generation_hash) << 32) | lights_hash;
		if (key != candidate_key || !candidate_buffer.is_valid()) {
			LocalVector<AABB> bounds;
			swrt_trace->get_instance_bounds(bounds);
			build_light_candidates(bounds.ptr(), bounds.size(), gpu_lights.ptr(), gpu_lights.size(), SWRT_LIGHT_CANDIDATE_CAP, candidate_data);
			_storage_buffer(candidate_buffer, candidate_capacity, candidate_data.size(), sizeof(uint32_t));
			rd->buffer_update(candidate_buffer, 0, candidate_data.size() * sizeof(uint32_t), candidate_data.ptr());
			candidate_key = key;
		}
	}
	const RID shader_rid = swrt ? swrt_shader->shader.version_get_shader(swrt_shader->version, 0) : shader->shader.version_get_shader(shader->version, 0);
	const RID pipeline = swrt ? swrt_shader->pipeline : shader->pipeline;
	RID directional_tex = light_storage->directional_shadow_get_texture();
	if (!directional_tex.is_valid()) {
		directional_tex = dummy_depth;
	}
	RID positional_tex = dummy_depth;
	if (p_render_data->shadow_atlas.is_valid()) {
		const RID atlas_tex = light_storage->shadow_atlas_get_texture(p_render_data->shadow_atlas);
		if (atlas_tex.is_valid()) {
			positional_tex = atlas_tex;
		}
	}
	const RID far_tlas = far_ready ? far_field->get_tlas() : tlas;
	static RID cached_directional;
	static RID cached_positional;
	static RID cached_cards;
	static RID cached_records;
	static RID cached_near_tlas;
	static RID cached_far_tlas;
	static bool cached_swrt = false;
	if (cached_directional != directional_tex || cached_positional != positional_tex || cached_cards != src->get_card_buffer() || cached_records != record_buffer || cached_near_tlas != tlas || cached_far_tlas != far_tlas || cached_swrt != swrt) {
		_reset_uniform_set();
		cached_directional = directional_tex;
		cached_positional = positional_tex;
		cached_cards = src->get_card_buffer();
		cached_records = record_buffer;
		cached_near_tlas = tlas;
		cached_far_tlas = far_tlas;
		cached_swrt = swrt;
	}
	if (!uniform_set.is_valid()) {
		RD::Uniform uniforms[20] = {
			RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, tlas),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, radiance),
			RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, mask),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ material_sampler, src->get_atlas_texture(NextGISRC::ATLAS_ALBEDO) })),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ material_sampler, src->get_atlas_texture(NextGISRC::ATLAS_NORMAL) })),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ material_sampler, src->get_atlas_texture(NextGISRC::ATLAS_EMISSION) })),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ material_sampler, src->get_atlas_texture(NextGISRC::ATLAS_DEPTH) })),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ shadow_sampler, directional_tex })),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ shadow_sampler, positional_tex })),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, src->get_card_buffer()),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, src->get_card_set_buffer()),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, record_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, light_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, reuse_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, page_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, index_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, counter_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, probe_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, params_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 19, Vector<RID>({ material_sampler, indirect_texture })),
		};
		if (swrt) {
			// GodotNext: ps_swrt_trace (#186, #188). The sibling's set 0 holds the per-light candidate
			// lists in place of the TLAS (#186 step 5), and there is no far field (HWRT only, plan L8).
			uniforms[0] = RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, candidate_buffer);
			uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 20), shader_rid, 0);
		} else {
			uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 20), shader_rid, 0);
			uniforms[0] = RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, far_tlas);
			far_uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 20), shader_rid, 0);
		}
	}
	// GodotNext: ps_swrt_trace (#186, #188). Set 1: the global-SDF trace with this frame's scene
	// params (the AS-updates slot filled them); freed after the dispatch.
	const RID trace_set = swrt ? swrt_trace->create_trace_uniform_set(shader_rid, 1, false, next_gi->get_voxel_lighting()) : RID();
	const uint32_t total_pages = gpu_pages.size() + far_gpu_pages.size();
	const uint32_t near_budget = total_pages > 0 ? uint32_t(uint64_t(params.ray_budget) * gpu_pages.size() / total_pages) : 0;
	const uint32_t far_budget = params.ray_budget - near_budget;
	params.ray_budget = near_budget;
	params.page_offset = 0;
	rd->buffer_update(params_buffer, 0, sizeof(params), &params);
	if (near_ready && uniform_set.is_valid() && (!swrt || trace_set.is_valid()) && !gpu_pages.is_empty()) {
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, pipeline);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		if (swrt) {
			rd->compute_list_bind_uniform_set(list, trace_set, 1);
		}
		rd->compute_list_dispatch(list, (max_w + 7) / 8, (max_h + 7) / 8, gpu_pages.size());
		rd->compute_list_end();
	}
	if (trace_set.is_valid()) {
		rd->free_rid(trace_set);
	}
	params.ray_budget = far_budget;
	params.page_offset = gpu_pages.size();
	rd->buffer_update(params_buffer, 0, sizeof(params), &params);
	if (far_ready && far_uniform_set.is_valid() && !far_gpu_pages.is_empty()) {
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, pipeline);
		rd->compute_list_bind_uniform_set(list, far_uniform_set, 0);
		rd->compute_list_dispatch(list, (max_w + 7) / 8, (max_h + 7) / 8, far_gpu_pages.size());
		rd->compute_list_end();
	}
	// GodotNext: nextgi_async_counters (#186). The traced count arrives one or more frames later;
	// its ray_fraction lands on this frame's scheduled cards, found again by page key.
	LocalVector<NextGIRelightRayRequest> ray_schedule;
	ray_schedule.reserve(scheduled_count);
	for (uint32_t s = 0; s < scheduled_count && s < work.size(); s++) {
		const FlatPage &flat_page = flat[scheduled[s]];
		NextGIRelightRayRequest request;
		request.page_key = instances[flat_page.instance].page_key;
		request.card = flat_page.card;
		request.ray_estimate = work[scheduled[s]].ray_estimate;
		ray_schedule.push_back(request);
	}
	_request_counters(ray_schedule);
	frame++;

	const bool read_probes = bool(GLOBAL_GET("rendering/global_illumination/nextgi/debug/read_page_radiance"));
	const bool read_mask = bool(GLOBAL_GET("rendering/global_illumination/nextgi/debug/read_shadow_mask"));
	if (read_probes && probe_buffer.is_valid() && page_table_count > 0) {
		const Vector<uint8_t> data = rd->buffer_get_data(probe_buffer, 0, page_table_count * sizeof(float) * 4);
		if (data.size() >= int(page_table_count * sizeof(float) * 4)) {
			const float *probes = reinterpret_cast<const float *>(data.ptr());
			probe_rgb.resize(page_table_count * 3);
			for (uint32_t i = 0; i < page_table_count; i++) {
				probe_rgb.write[i * 3 + 0] = probes[i * 4 + 0];
				probe_rgb.write[i * 3 + 1] = probes[i * 4 + 1];
				probe_rgb.write[i * 3 + 2] = probes[i * 4 + 2];
			}
		}
	}
	if (read_mask && mask.is_valid()) {
		const Vector<uint8_t> data = rd->texture_get_data(mask, 0);
		if (data.size() >= mask_extent.x * mask_extent.y) {
			for (const InstancePages &instance_pages : instances) {
				for (const CardPage &card_page : instance_pages.cards) {
					NextGICard card;
					if (!src->copy_card(card_page.card_index, card)) {
						continue;
					}
					PackedByteArray bytes;
					bytes.resize(card_page.mask_w * card_page.mask_h);
					for (uint32_t y = 0; y < card_page.mask_h; y++) {
						for (uint32_t x = 0; x < card_page.mask_w; x++) {
							bytes.write[y * card_page.mask_w + x] = data[(card_page.mask_y + y) * mask_extent.x + card_page.mask_x + x];
						}
					}
					PackedFloat32Array world;
					world.resize(12);
					for (int i = 0; i < 12; i++) {
						world.write[i] = instance_pages.record.object_to_world[i];
					}
					Dictionary page;
					page["stable_id"] = card_page.stable_id;
					page["radiance_w"] = card_page.radiance_w;
					page["radiance_h"] = card_page.radiance_h;
					page["mask_w"] = card_page.mask_w;
					page["mask_h"] = card_page.mask_h;
					page["mask"] = bytes;
					page["origin"] = Vector3(card.origin[0], card.origin[1], card.origin[2]);
					page["axis_u"] = Vector3(card.axis_u[0], card.axis_u[1], card.axis_u[2]);
					page["axis_v"] = Vector3(card.axis_v[0], card.axis_v[1], card.axis_v[2]);
					page["axis_n"] = Vector3(card.axis_n[0], card.axis_n[1], card.axis_n[2]);
					page["half_u"] = card.axis_u[3];
					page["half_v"] = card.axis_v[3];
					page["depth"] = card.axis_n[3];
					page["world"] = world;
					mask_pages.push_back(page);
				}
			}
		}
	}
	_upload_page_rects();
	// Drop the set before the frame ends. A later free of the TLAS or the card buffers would
	// otherwise free it through the dependency map, and shutdown would free it again.
	_reset_uniform_set();
}

// GodotNext: nextgi_async_counters (#186)
void NextGIRelight::_request_counters(LocalVector<NextGIRelightRayRequest> &r_schedule) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || !counter_buffer.is_valid()) {
		return;
	}
	uint64_t token = 0;
	{
		MutexLock lock(nextgi_relight_counter_mutex);
		if (counters_pending) {
			return;
		}
		counters_pending = true;
		token = nextgi_relight_counter_next_token++;
		counter_token = token;
		nextgi_relight_counter_registry.insert(token, this);
		pending_schedule = r_schedule;
	}
	if (!readback_buffer.is_valid()) {
		readback_buffer = rd->storage_buffer_create(sizeof(NextGIRelightCountersGPU));
	}
	Error err = readback_buffer.is_valid() ? OK : FAILED;
	if (err == OK) {
		// A copy, so the next frame's zeroing cannot race the read.
		rd->buffer_copy(counter_buffer, readback_buffer, 0, 0, sizeof(NextGIRelightCountersGPU));
		err = rd->buffer_get_data_async(readback_buffer, callable_mp_static(&NextGIRelight::_counters_received).bind(token), 0, sizeof(NextGIRelightCountersGPU));
	}
	if (err != OK) {
		MutexLock lock(nextgi_relight_counter_mutex);
		nextgi_relight_counter_registry.erase(token);
		counter_token = 0;
		counters_pending = false;
		pending_schedule.clear();
	}
}

void NextGIRelight::_counters_received(const Vector<uint8_t> &p_data, uint64_t p_token) {
	MutexLock lock(nextgi_relight_counter_mutex);
	NextGIRelight **owner = nextgi_relight_counter_registry.getptr(p_token);
	if (owner == nullptr) {
		return;
	}
	NextGIRelight *self = *owner;
	nextgi_relight_counter_registry.erase(p_token);
	self->counter_token = 0;
	self->counters_pending = false;
	LocalVector<NextGIRelightRayRequest> schedule;
	SWAP(schedule, self->pending_schedule);
	if (p_data.size() < int(sizeof(NextGIRelightCountersGPU))) {
		return;
	}
	NextGIRelightCountersGPU got;
	memcpy(&got, p_data.ptr(), sizeof(got));
	self->last_rays = got.traced;
	self->last_rays_skipped = got.skipped;
	self->max_rays = MAX(self->max_rays, self->last_rays);
	self->apply_ray_fraction(schedule, got.traced);
}

uint64_t NextGIRelight::get_page_key(RenderGeometryInstance *p_instance, uint32_t p_element) {
	return _nextgi_page_key(p_instance, p_element);
}

void NextGIRelight::apply_ray_fraction(const LocalVector<NextGIRelightRayRequest> &p_schedule, uint32_t p_traced) {
	const float fraction = nextgi_relight_ray_fraction(p_schedule.ptr(), p_schedule.size(), p_traced);
	if (fraction < 0.0f) {
		return;
	}
	for (const NextGIRelightRayRequest &request : p_schedule) {
		const uint32_t *index = instance_index.getptr(request.page_key);
		if (index == nullptr || *index >= instances.size() || request.card >= instances[*index].cards.size()) {
			continue; // The page set left (freed or evicted) since the schedule.
		}
		instances[*index].cards[request.card].ray_fraction = fraction;
	}
}

float NextGIRelight::get_card_ray_fraction(RenderGeometryInstance *p_instance, uint32_t p_element, uint32_t p_card) const {
	const uint32_t *index = instance_index.getptr(_nextgi_page_key(p_instance, p_element));
	if (index == nullptr || *index >= instances.size() || p_card >= instances[*index].cards.size()) {
		return -1.0f;
	}
	return instances[*index].cards[p_card].ray_fraction;
}

void NextGIRelight::build_page_rect_words(LocalVector<uint32_t> &r_words, bool p_relit_only) const {
	r_words.resize(page_table_count * 2);
	for (uint32_t i = 0; i < r_words.size(); i++) {
		r_words[i] = 0;
	}
	for (const InstancePages &instance_pages : instances) {
		for (const CardPage &card : instance_pages.cards) {
			if (card.stable_id >= page_table_count || (p_relit_only && !card.relit)) {
				continue;
			}
			r_words[card.stable_id * 2] = _pack_xy(card.radiance_x, card.radiance_y);
			r_words[card.stable_id * 2 + 1] = _pack_xy(card.radiance_w, card.radiance_h);
		}
	}
}

void NextGIRelight::_upload_page_rects() {
	if (page_table_count == 0) {
		page_rect_uploaded = 0;
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL(rd);
	if (!page_rect_buffer.is_valid() || page_rect_capacity < page_table_count) {
		if (page_rect_buffer.is_valid()) {
			rd->free_rid(page_rect_buffer);
		}
		page_rect_capacity = MAX(page_table_count, page_rect_capacity * 2);
		page_rect_buffer = rd->storage_buffer_create(page_rect_capacity * sizeof(uint32_t) * 2);
	}
	LocalVector<uint32_t> words;
	build_page_rect_words(words, true);
	rd->buffer_update(page_rect_buffer, 0, words.size() * sizeof(uint32_t), words.ptr());
	page_rect_uploaded = page_table_count;
}

float NextGIRelight::get_max_origin_offset() const {
	float max_offset = 0.0f;
	for (const InstancePages &instance_pages : instances) {
		max_offset = MAX(max_offset, instance_pages.record.origin_offset);
	}
	return max_offset;
}

// GodotNext: ps_swrt_trace (#186, #188)
template <typename T>
static String _nextgi_relight_source() {
	T shader;
	shader.initialize(Vector<String>({ "" }));
	const RID version = shader.version_create(false);
	ERR_FAIL_COND_V(!version.is_valid(), String());
	const Vector<String> sources = shader.version_build_variant_stage_sources(version, 0);
	shader.version_free(version);
	if (sources.size() <= RenderingDevice::SHADER_STAGE_COMPUTE) {
		return String();
	}
	return sources[RenderingDevice::SHADER_STAGE_COMPUTE];
}

String NextGIRelight::build_shader_source(bool p_software) {
	return p_software ? _nextgi_relight_source<NextgiSrcRelightSwrtShaderRD>() : _nextgi_relight_source<NextgiSrcRelightShaderRD>();
}

void NextGIRelight::fill_info(Dictionary &r_info) const {
	// GodotNext: nextgi_async_counters (#186). The readback callback writes the ray counters.
	MutexLock lock(nextgi_relight_counter_mutex);
	r_info["src_resident_pages"] = int64_t(page_table_count);
	r_info["src_page_evictions"] = int64_t(page_evictions);
	r_info["src_radiance_bytes"] = int64_t(radiance_bytes);
	r_info["src_relight_texels"] = int64_t(last_texels);
	r_info["src_shadow_rays"] = int64_t(last_rays);
	r_info["src_dirty_pages"] = int64_t(last_dirty_pages);
	r_info["src_max_relight_age_frames"] = int64_t(last_max_age);
	r_info["src_relight_rays"] = int64_t(last_rays);
	r_info["src_relight_rays_skipped"] = int64_t(last_rays_skipped);
	r_info["src_relight_max_texels"] = int64_t(max_texels);
	r_info["src_relight_max_rays"] = int64_t(max_rays);
	r_info["src_max_shadowed_lights"] = int64_t(max_shadowed);
	r_info["src_max_mask_evals"] = int64_t(max_mask_evals);
	r_info["src_unshadowed_light_count"] = int64_t(last_unshadowed);
	r_info["src_light_count"] = int64_t(last_light_count);
	r_info["src_light_directional_count"] = int64_t(last_directional);
	r_info["src_light_dynamic_count"] = int64_t(last_dynamic);
	r_info["src_light_static_count"] = int64_t(last_static);
	r_info["src_page_count"] = int64_t(flat_page_count);
	r_info["src_page_alloc_failures"] = int64_t(alloc_failures);
	// GodotNext: nextgi_src_page_levels (#247)
	r_info["src_page_failures_radiance"] = int64_t(alloc_failures_radiance);
	r_info["src_page_failures_card"] = int64_t(alloc_failures_card);
	r_info["src_page_unplaced"] = int64_t(pass_unplaced);
	r_info["src_page_demotions"] = int64_t(page_demotions);
	r_info["src_page_priority_evictions"] = int64_t(page_priority_evictions);
	r_info["src_page_recuts"] = int64_t(page_recuts);
	{
		PackedInt32Array levels;
		levels.resize(NEXTGI_SRC_PAGE_LEVEL_MAX + 1);
		levels.fill(0);
		int64_t current = 0;
		for (const InstancePages &pages : instances) {
			if (pages.record_pass == record_pass) {
				levels.set(MIN(pages.level, uint32_t(NEXTGI_SRC_PAGE_LEVEL_MAX)), levels[MIN(pages.level, uint32_t(NEXTGI_SRC_PAGE_LEVEL_MAX))] + 1);
				current++;
			}
		}
		r_info["src_page_sets"] = int64_t(instances.size());
		r_info["src_page_sets_current"] = current;
		r_info["src_page_level_sets"] = levels;
	}
	r_info["src_radiance_bytes"] = int64_t(radiance_bytes);
	r_info["src_radiance_format"] = radiance_is_packed ? String("B10G11R11") : String("RGBA16F");
	if (!probe_rgb.is_empty()) {
		r_info["src_page_probes"] = probe_rgb;
	}
	if (!mask_pages.is_empty()) {
		r_info["src_mask_pages"] = mask_pages;
	}
}
