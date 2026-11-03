/**************************************************************************/
/*  next_gi_relight.h                                                     */
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

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/variant/variant.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src_relight.h"

class RenderDataRD;
class RenderGeometryInstance;

namespace RendererRD {

// GPU half of D14. Pages are allocated from the RT scene's TLAS phase and shaded in the
// "SRC relight" slot. The pure rules live in next_gi_src_relight.h.
class NextGIRelight {
	static NextGIRelight *singleton;

	struct ShaderState;
	ShaderState *shader = nullptr;

	struct CardPage {
		uint32_t card_index = 0;
		uint32_t radiance_x = 0;
		uint32_t radiance_y = 0;
		uint32_t radiance_w = 0;
		uint32_t radiance_h = 0;
		uint32_t mask_x = 0;
		uint32_t mask_y = 0;
		uint32_t mask_w = 0;
		uint32_t mask_h = 0;
		uint32_t stable_id = 0;
	};

	struct InstancePages {
		RenderGeometryInstance *instance = nullptr;
		uint32_t page_table = UINT32_MAX;
		uint32_t record_index = 0;
		NextGIInstanceRecord record;
		LocalVector<CardPage> cards;
	};

	RID radiance;
	RID mask;
	RID dummy_depth;
	RID material_sampler;
	RID shadow_sampler;
	bool radiance_is_packed = true;
	uint64_t radiance_bytes = 0;
	bool atlases_ready = false;
	bool logged_atlas = false;

	NextGIShelfPacker radiance_packer{ NEXTGI_SRC_RADIANCE_SIZE, NEXTGI_SRC_RADIANCE_SIZE };
	NextGIShelfPacker mask_packer{ NEXTGI_SRC_MASK_SIZE, NEXTGI_SRC_MASK_SIZE };
	LocalVector<InstancePages> instances;
	HashMap<RenderGeometryInstance *, uint32_t> instance_index;
	uint32_t page_table_count = 0;
	uint32_t alloc_failures = 0;

	RID light_buffer;
	RID reuse_buffer;
	RID page_buffer;
	RID index_buffer;
	RID counter_buffer;
	RID probe_buffer;
	RID params_buffer;
	RID uniform_set;
	uint32_t light_capacity = 0;
	uint32_t reuse_capacity = 0;
	uint32_t page_capacity = 0;
	uint32_t index_capacity = 0;
	uint32_t probe_capacity = 0;

	uint32_t schedule_cursor = 0;
	uint32_t frame = 0;
	uint32_t last_texels = 0;
	uint32_t last_rays = 0;
	uint32_t last_rays_skipped = 0;
	uint32_t max_texels = 0;
	uint32_t max_rays = 0;
	uint32_t max_shadowed = 0;
	uint32_t max_mask_evals = 0;
	uint32_t last_unshadowed = 0;
	uint32_t last_light_count = 0;
	uint32_t last_directional = 0;
	uint32_t last_dynamic = 0;
	uint32_t last_static = 0;
	uint32_t flat_page_count = 0;

	PackedFloat32Array probe_rgb;
	Array mask_pages;

public:
	struct ScheduledPage {
		uint32_t indirect_x = 0;
		uint32_t indirect_y = 0;
		uint32_t record_index = 0;
		uint32_t card_index = 0;
		uint32_t block_count = 0;
	};

private:
	LocalVector<ScheduledPage> scheduled_pages;
	RID page_rect_buffer;
	uint32_t page_rect_capacity = 0;
	RID indirect_texture;

	bool _ensure_atlases();
	bool _ensure_shader();
	RID _storage_buffer(RID &r_buffer, uint32_t &r_capacity, uint32_t p_count, uint32_t p_stride);
	void _reset_uniform_set();
	void _upload_page_rects();

public:
	static NextGIRelight *get_singleton() { return singleton; }

	void init();
	void free_resources();

	// Allocates radiance and mask rects for the instance's cards. Writes page_table on the record.
	// Later parts of the same instance share the allocation. Returns UINT32_MAX when the mesh has no cards.
	uint32_t assign_pages(RenderGeometryInstance *p_instance, NextGIInstanceRecord &r_record);

	// Line 2b. Builds the scenario light set, schedules pages under the High caps and dispatches the relight shader.
	void relight(RenderDataRD *p_render_data);

	RID get_page_rect_buffer() const { return page_rect_buffer; }
	uint32_t get_page_rect_count() const { return page_table_count; }
	RID get_indirect_texture() const { return indirect_texture; }
	RID get_radiance_texture() const { return radiance; }
	const LocalVector<ScheduledPage> &get_scheduled_pages() const { return scheduled_pages; }
	float get_max_origin_offset() const;

	void fill_info(Dictionary &r_info) const;

	NextGIRelight();
	~NextGIRelight();
};

} // namespace RendererRD
