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
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_far_field.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_src_relight.glsl.gen.h"
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

struct NextGIRelight::ShaderState {
	NextgiSrcRelightShaderRD shader;
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
	if (rd == nullptr) {
		return;
	}
	RID buffers[7] = { light_buffer, reuse_buffer, page_buffer, index_buffer, counter_buffer, probe_buffer, params_buffer };
	for (int i = 0; i < 7; i++) {
		if (buffers[i].is_valid()) {
			rd->free_rid(buffers[i]);
		}
	}
	light_buffer = RID();
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
	// GodotNext: nextgi_quality_presets (#55). The radiance atlas doubles in height at Epic.
	const bool epic = NextGI::get_singleton() != nullptr && NextGI::get_quality_params(NextGI::get_singleton()->get_quality_setting()).src_radiance_mib > 64;
	const NextGIAtlasBudget atlas_budget = nextgi_src_atlas_budget(epic);
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

	tf.width = NEXTGI_SRC_MASK_SIZE;
	tf.height = NEXTGI_SRC_MASK_SIZE;
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
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	dummy_depth = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(dummy_depth.is_null(), false);

	atlases_ready = true;
	if (!logged_atlas) {
		logged_atlas = true;
		const double mask_bytes = double(NEXTGI_SRC_MASK_SIZE) * double(NEXTGI_SRC_MASK_SIZE);
		print_line(vformat("NextGI SRC: radiance atlas %dx%d %s, %.1f MiB; shadow mask %dx%d R8, %.1f MiB.", atlas_budget.radiance_width, atlas_budget.radiance_height, radiance_is_packed ? "B10G11R11" : "RGBA16F", double(radiance_bytes) / (1024.0 * 1024.0), NEXTGI_SRC_MASK_SIZE, NEXTGI_SRC_MASK_SIZE, mask_bytes / (1024.0 * 1024.0)));
	}
	return true;
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
		Vector<String> defines;
		defines.push_back(radiance_is_packed ? "" : "#define NEXTGI_RADIANCE_RGBA16F\n");
		shader->shader.initialize(defines);
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

bool NextGIRelight::_evict_oldest_pages() {
	if (instances.is_empty()) {
		return false;
	}
	uint32_t oldest = 0;
	for (uint32_t i = 1; i < instances.size(); i++) {
		if (instances[i].last_used < instances[oldest].last_used) {
			oldest = i;
		}
	}
	for (const CardPage &page : instances[oldest].cards) {
		radiance_buddy.free_block(Rect2i(int(page.radiance_x), int(page.radiance_y), int(page.radiance_w), int(page.radiance_h)));
		mask_packer.free(Rect2i(int(page.mask_x), int(page.mask_y), int(page.mask_w), int(page.mask_h)));
	}
	instance_index.erase(instances[oldest].page_key);
	instances.remove_at(oldest);
	for (uint32_t i = oldest; i < instances.size(); i++) {
		instance_index[instances[i].page_key] = i;
	}
	page_evictions++;
	return true;
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
	const uint32_t index = *existing;
	for (const CardPage &page : instances[index].cards) {
		radiance_buddy.free_block(Rect2i(int(page.radiance_x), int(page.radiance_y), int(page.radiance_w), int(page.radiance_h)));
		mask_packer.free(Rect2i(int(page.mask_x), int(page.mask_y), int(page.mask_w), int(page.mask_h)));
	}
	instance_index.erase(page_key);
	instances.remove_at(index);
	for (uint32_t i = index; i < instances.size(); i++) {
		instance_index[instances[i].page_key] = i;
	}
}

uint32_t NextGIRelight::assign_pages(RenderGeometryInstance *p_instance, NextGIInstanceRecord &r_record, uint32_t p_element) {
	if (p_instance == nullptr || r_record.card_set == UINT32_MAX) {
		return UINT32_MAX;
	}
	const uint64_t page_key = _nextgi_page_key(p_instance, p_element);
	if (!_ensure_atlases()) {
		return UINT32_MAX;
	}
	NextGISRC *src = NextGISRC::get_singleton();
	if (src == nullptr) {
		return UINT32_MAX;
	}
	const uint32_t *existing = instance_index.getptr(page_key);
	if (existing != nullptr) {
		InstancePages &pages = instances[*existing];
		pages.record = r_record;
		pages.record_index = r_record.tlas_index;
		pages.last_used = page_clock++;
		r_record.page_table = pages.page_table;
		return pages.page_table;
	}
	NextGICardSet card_set;
	if (!src->copy_card_set(r_record.card_set, card_set) || card_set.card_count == 0) {
		return UINT32_MAX;
	}

	InstancePages pages;
	pages.instance = p_instance;
	pages.element = p_element;
	pages.page_key = page_key;
	pages.record = r_record;
	pages.record_index = r_record.tlas_index;
	pages.page_table = page_table_count;
	bool fitted = true;
	for (uint32_t i = 0; i < card_set.card_count; i++) {
		NextGICard card;
		if (!src->copy_card(card_set.first_card + i, card)) {
			fitted = false;
			break;
		}
		const uint32_t card_w = MAX(card.atlas_rect[2], 1u);
		const uint32_t card_h = MAX(card.atlas_rect[3], 1u);
		Rect2i radiance_rect;
		Rect2i mask_rect;
		if (!radiance_buddy.allocate(card_w, card_h, radiance_rect)) {
			if (!_evict_oldest_pages() || !radiance_buddy.allocate(card_w, card_h, radiance_rect)) {
				fitted = false;
				break;
			}
		}
		if (!mask_packer.allocate(MAX(card_w / 2, 1u), MAX(card_h / 2, 1u), mask_rect)) {
			radiance_buddy.free_block(radiance_rect);
			fitted = false;
			break;
		}
		CardPage page;
		page.card_index = card_set.first_card + i;
		page.radiance_x = radiance_rect.position.x;
		page.radiance_y = radiance_rect.position.y;
		page.radiance_w = radiance_rect.size.x;
		page.radiance_h = radiance_rect.size.y;
		page.mask_x = mask_rect.position.x;
		page.mask_y = mask_rect.position.y;
		page.mask_w = mask_rect.size.x;
		page.mask_h = mask_rect.size.y;
		page.stable_id = page_table_count + pages.cards.size();
		pages.cards.push_back(page);
	}
	if (!fitted || pages.cards.is_empty()) {
		for (const CardPage &page : pages.cards) {
			radiance_buddy.free_block(Rect2i(int(page.radiance_x), int(page.radiance_y), int(page.radiance_w), int(page.radiance_h)));
			mask_packer.free(Rect2i(int(page.mask_x), int(page.mask_y), int(page.mask_w), int(page.mask_h)));
		}
		alloc_failures++;
		return 0x80000000u;
	}
	pages.last_used = page_clock++;
	page_table_count += pages.cards.size();
	r_record.page_table = pages.page_table;
	instance_index.insert(page_key, instances.size());
	instances.push_back(pages);
	return pages.page_table;
}

void NextGIRelight::relight(RenderDataRD *p_render_data) {
	ERR_FAIL_NULL(p_render_data);
	last_texels = 0;
	last_rays = 0;
	last_rays_skipped = 0;
	last_unshadowed = 0;
	last_light_count = 0;
	last_directional = 0;
	last_dynamic = 0;
	last_static = 0;
	scheduled_pages.clear();
	probe_rgb.clear();
	mask_pages.clear();
	if (p_render_data->sdfgi_update_data == nullptr || !_ensure_atlases() || !_ensure_shader()) {
		return;
	}
	NextGI *next_gi = NextGI::get_singleton();
	NextGISRC *src = NextGISRC::get_singleton();
	if (next_gi == nullptr || next_gi->get_rt_scene() == nullptr || src == nullptr || src->get_card_buffer().is_null()) {
		return;
	}
	const RID tlas = next_gi->get_rt_scene()->get_tlas();
	const NextGIFarField *far_field = next_gi->get_rt_scene()->get_far_field();
	const bool near_ready = tlas.is_valid() && next_gi->get_rt_scene()->is_tlas_traceable();
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
		cpu.gpu.flags = (NEXTGI_LIGHT_REUSE_NONE << NEXTGI_LIGHT_REUSE_SHIFT);
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
	for (uint32_t i = 0; i < instances.size(); i++) {
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

	const RID shader_rid = shader->shader.version_get_shader(shader->version, 0);
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
	if (cached_directional != directional_tex || cached_positional != positional_tex || cached_cards != src->get_card_buffer() || cached_records != next_gi->get_rt_scene()->get_instance_record_buffer() || cached_near_tlas != tlas || cached_far_tlas != far_tlas) {
		_reset_uniform_set();
		cached_directional = directional_tex;
		cached_positional = positional_tex;
		cached_cards = src->get_card_buffer();
		cached_records = next_gi->get_rt_scene()->get_instance_record_buffer();
		cached_near_tlas = tlas;
		cached_far_tlas = far_tlas;
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
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, next_gi->get_rt_scene()->get_instance_record_buffer()),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, light_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, reuse_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, page_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, index_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, counter_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, probe_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, params_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 19, Vector<RID>({ material_sampler, indirect_texture })),
		};
		uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 20), shader_rid, 0);
		uniforms[0] = RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, far_tlas);
		far_uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 20), shader_rid, 0);
	}
	const uint32_t total_pages = gpu_pages.size() + far_gpu_pages.size();
	const uint32_t near_budget = total_pages > 0 ? uint32_t(uint64_t(params.ray_budget) * gpu_pages.size() / total_pages) : 0;
	const uint32_t far_budget = params.ray_budget - near_budget;
	params.ray_budget = near_budget;
	params.page_offset = 0;
	rd->buffer_update(params_buffer, 0, sizeof(params), &params);
	if (near_ready && uniform_set.is_valid() && !gpu_pages.is_empty()) {
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, shader->pipeline);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		rd->compute_list_dispatch(list, (max_w + 7) / 8, (max_h + 7) / 8, gpu_pages.size());
		rd->compute_list_end();
	}
	params.ray_budget = far_budget;
	params.page_offset = gpu_pages.size();
	rd->buffer_update(params_buffer, 0, sizeof(params), &params);
	if (far_ready && far_uniform_set.is_valid() && !far_gpu_pages.is_empty()) {
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, shader->pipeline);
		rd->compute_list_bind_uniform_set(list, far_uniform_set, 0);
		rd->compute_list_dispatch(list, (max_w + 7) / 8, (max_h + 7) / 8, far_gpu_pages.size());
		rd->compute_list_end();
	}
	const Vector<uint8_t> counters = rd->buffer_get_data(counter_buffer);
	if (counters.size() >= int(sizeof(NextGIRelightCountersGPU))) {
		NextGIRelightCountersGPU got;
		memcpy(&got, counters.ptr(), sizeof(got));
		last_rays = got.traced;
		last_rays_skipped = got.skipped;
		max_rays = MAX(max_rays, last_rays);
		uint32_t estimated = 0;
		for (uint32_t s = 0; s < scheduled_count && s < work.size(); s++) {
			estimated += work[scheduled[s]].ray_estimate;
		}
		if (estimated > 0) {
			const float fraction = CLAMP(float(last_rays) / float(estimated), 0.0f, 1.0f);
			for (uint32_t s = 0; s < scheduled_count; s++) {
				const FlatPage &flat_page = flat[scheduled[s]];
				instances[flat_page.instance].cards[flat_page.card].ray_fraction = fraction;
			}
		}
	}
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
		if (data.size() >= NEXTGI_SRC_MASK_SIZE * NEXTGI_SRC_MASK_SIZE) {
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
							bytes.write[y * card_page.mask_w + x] = data[(card_page.mask_y + y) * NEXTGI_SRC_MASK_SIZE + card_page.mask_x + x];
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

void NextGIRelight::_upload_page_rects() {
	if (page_table_count == 0) {
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
	words.resize(page_table_count * 2);
	for (uint32_t i = 0; i < words.size(); i++) {
		words[i] = 0;
	}
	for (const InstancePages &instance_pages : instances) {
		for (const CardPage &card : instance_pages.cards) {
			if (card.stable_id >= page_table_count) {
				continue;
			}
			words[card.stable_id * 2] = _pack_xy(card.radiance_x, card.radiance_y);
			words[card.stable_id * 2 + 1] = _pack_xy(card.radiance_w, card.radiance_h);
		}
	}
	rd->buffer_update(page_rect_buffer, 0, words.size() * sizeof(uint32_t), words.ptr());
}

float NextGIRelight::get_max_origin_offset() const {
	float max_offset = 0.0f;
	for (const InstancePages &instance_pages : instances) {
		max_offset = MAX(max_offset, instance_pages.record.origin_offset);
	}
	return max_offset;
}

void NextGIRelight::fill_info(Dictionary &r_info) const {
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
	r_info["src_radiance_bytes"] = int64_t(radiance_bytes);
	r_info["src_radiance_format"] = radiance_is_packed ? String("B10G11R11") : String("RGBA16F");
	if (!probe_rgb.is_empty()) {
		r_info["src_page_probes"] = probe_rgb;
	}
	if (!mask_pages.is_empty()) {
		r_info["src_mask_pages"] = mask_pages;
	}
}
