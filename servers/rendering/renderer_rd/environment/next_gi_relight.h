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
	// GodotNext: ps_swrt_trace (#186, #188). The software tier's sibling
	// (nextgi_src_relight_swrt.glsl): the same kernel, whose shadow rays march the global SDF. The
	// software tier never constructs the ray-query ShaderRD.
	struct SWRTShaderState;
	SWRTShaderState *swrt_shader = nullptr;

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
		bool dirty_direct = true;
		bool near_dirty = false;
		uint32_t last_relit_frame = 0;
		float ray_fraction = 1.0f;
	};

	struct InstancePages {
		RenderGeometryInstance *instance = nullptr;
		uint32_t element = UINT32_MAX;
		uint64_t page_key = 0;
		uint32_t page_table = UINT32_MAX;
		uint32_t record_index = 0;
		NextGIInstanceRecord record;
		LocalVector<CardPage> cards;
		uint64_t last_used = 0;
		float captured_scale = 0.0f;
		// GodotNext: ps_lumen_lite (#183). The card set the pages were cut for, and the record
		// pass that last saw the instance. record_index is only valid in that pass.
		uint32_t card_set = UINT32_MAX;
		uint64_t record_pass = 0;
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

	// GodotNext: ps_lumen_lite (#183). set_page_extent() matches it to the radiance texture.
	NextGIBuddyAllocator radiance_buddy{ NEXTGI_SRC_RADIANCE_SIZE, NEXTGI_SRC_PAGE_MIN };
	uint64_t record_pass = 1;
	// Page key per instance-record index of this pass, so finish_record_pass() can repair records
	// whose set was evicted later in the same pass.
	LocalVector<uint64_t> pass_record_keys;
	uint64_t page_clock = 1;
	uint32_t page_evictions = 0;
	NextGIShelfPacker mask_packer{ NEXTGI_SRC_MASK_SIZE, NEXTGI_SRC_MASK_SIZE };
	LocalVector<InstancePages> instances;
	HashMap<uint64_t, uint32_t> instance_index;
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
	RID far_uniform_set;
	uint32_t light_capacity = 0;
	// GodotNext: ps_swrt_trace (#186 step 5). Per-light SWRT candidate lists (build_light_candidates),
	// rebuilt when the lights or the software instances change.
	RID candidate_buffer;
	uint32_t candidate_capacity = 0;
	uint64_t candidate_key = 0;
	LocalVector<uint32_t> candidate_data;
	uint32_t reuse_capacity = 0;
	uint32_t page_capacity = 0;
	uint32_t index_capacity = 0;
	uint32_t probe_capacity = 0;

	uint32_t schedule_cursor = 0;
	uint32_t frame = 0;
	uint32_t last_texels = 0;
	uint32_t last_rays = 0;
	uint32_t last_dirty_pages = 0;
	uint32_t last_max_age = 0;
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
	// Page table entries the page rect buffer holds. The record pass assigns page tables before
	// relight() uploads their rects, and relight() can return before the upload (for example while
	// the scene is not traceable), so page_table_count can run past the buffer.
	uint32_t page_rect_uploaded = 0;
	RID indirect_texture;

	// GodotNext: nextgi_async_counters (#186). The traced-ray counter is read asynchronously: a
	// synchronous read inside the frame block restarts the device frame and drops the block's
	// timestamps. The request keeps the schedule of the frame that issued it.
	RID readback_buffer;
	uint64_t counter_token = 0;
	bool counters_pending = false;
	LocalVector<NextGIRelightRayRequest> pending_schedule;
	void _request_counters(LocalVector<NextGIRelightRayRequest> &r_schedule);
	static void _counters_received(const Vector<uint8_t> &p_data, uint64_t p_token);

	bool _ensure_atlases();
	bool _ensure_shader();
	bool _ensure_swrt_shader(); // GodotNext: ps_swrt_trace (#186, #188)
	RID _storage_buffer(RID &r_buffer, uint32_t &r_capacity, uint32_t p_count, uint32_t p_stride);
	void _reset_uniform_set();
	void _upload_page_rects();
	bool _evict_oldest_pages();
	void _free_page_set(uint32_t p_index);

public:
	static NextGIRelight *get_singleton() { return singleton; }

	void init();
	void free_resources();

	// Allocates radiance and mask rects for the instance's cards. Writes page_table on the record.
	// Later parts of the same instance share the allocation. Returns UINT32_MAX when the mesh has no cards.
	uint32_t assign_pages(RenderGeometryInstance *p_instance, NextGIInstanceRecord &r_record, uint32_t p_element = UINT32_MAX);
	void release_instance(RenderGeometryInstance *p_instance);

	// GodotNext: ps_lumen_lite (#183). The RT scene calls this before it writes a new set of
	// instance records. Page sets that no assign_pages() call claims afterwards keep their
	// pages, but their record_index points into the old records, so relight skips them.
	void begin_record_pass();
	// After the last assign_pages() of the pass: records whose set a later instance of the same
	// pass evicted get the evicted mark instead of a page table that no longer exists.
	void finish_record_pass(LocalVector<NextGIInstanceRecord> &r_records) const;
	// The CPU half of assign_pages(), with the record's cards already copied from the SRC.
	uint32_t assign_card_pages(RenderGeometryInstance *p_instance, NextGIInstanceRecord &r_record, uint32_t p_element, uint32_t p_first_card, const NextGICard *p_cards, uint32_t p_card_count);
	// Sizes the page allocator to the radiance texture of this budget and drops every page.
	void set_page_extent(const NextGIAtlasBudget &p_budget);
	// Page sets claimed in the current record pass. Relight schedules only these.
	void collect_current_page_sets(LocalVector<uint32_t> &r_sets) const;
	struct PageSetInfo {
		uint32_t record_index = 0;
		uint32_t card_set = UINT32_MAX;
		uint32_t page_table = UINT32_MAX;
		LocalVector<Rect2i> radiance_rects;
	};
	PageSetInfo get_page_set_info(uint32_t p_set) const;
	uint32_t get_page_set_count() const { return instances.size(); }
	// The packed page rects the WRC and the gather read, two words per page table entry.
	void build_page_rect_words(LocalVector<uint32_t> &r_words) const;

	// Line 2b. Builds the scenario light set, schedules pages under the High caps and dispatches the relight shader.
	void relight(RenderDataRD *p_render_data);

	RID get_page_rect_buffer() const { return page_rect_buffer; }
	// Entries in get_page_rect_buffer(). A page table at or past this count has no rect yet, and
	// the gather, the WRC and voxel lighting treat it as unpaged.
	uint32_t get_page_rect_count() const { return page_rect_uploaded; }
	// UINT32_MAX when this instance has no relight pages. Bit 31 is the evicted mark.
	uint32_t get_instance_page_table(RenderGeometryInstance *p_instance, uint32_t p_element = UINT32_MAX) const;
	// GodotNext: nextgi_async_counters (#186). The key a page set is found by.
	static uint64_t get_page_key(RenderGeometryInstance *p_instance, uint32_t p_element = UINT32_MAX);
	// Sets ray_fraction = traced / sum(estimates) on each scheduled card, finding its page set by
	// key; cards whose set left since the schedule are skipped.
	void apply_ray_fraction(const LocalVector<NextGIRelightRayRequest> &p_schedule, uint32_t p_traced);
	// A card's ray_fraction, or -1 when the instance has no page set or no such card.
	float get_card_ray_fraction(RenderGeometryInstance *p_instance, uint32_t p_element, uint32_t p_card) const;
	RID get_indirect_texture() const { return indirect_texture; }
	RID get_radiance_texture() const { return radiance; }
	const LocalVector<ScheduledPage> &get_scheduled_pages() const { return scheduled_pages; }
	float get_max_origin_offset() const;
	uint32_t get_last_texels() const { return last_texels; }
	uint32_t get_last_rays() const { return last_rays; }
	uint32_t get_dirty_pages() const { return last_dirty_pages; }
	uint32_t get_max_relight_age() const { return last_max_age; }

	void fill_info(Dictionary &r_info) const;
	// GodotNext: ps_swrt_trace (#186, #188). The compute source of the hardware relight or of its
	// software sibling (nextgi_src_relight_swrt.glsl), for the doctests.
	static String build_shader_source(bool p_software);

	// GodotNext: ps_swrt_trace (#186 step 5, plan §21.2.6). The software relight's shadow rays
	// toward a positional light march the mesh SDFs of the instances whose world bounds overlap
	// that light's range sphere. r_data holds, per light, its list's offset and length
	// (data[2 * light], data[2 * light + 1]) and then the lists. A directional light, a light with
	// no range, or a light whose range overlaps more than p_cap instances gets
	// SWRT_LIGHT_CANDIDATES_OVERFLOW as its length; its rays march the global SDF.
	static constexpr uint32_t SWRT_LIGHT_CANDIDATE_CAP = 64; // Start value.
	static constexpr uint32_t SWRT_LIGHT_CANDIDATES_OVERFLOW = 0xffffffffu;
	static void build_light_candidates(const AABB *p_bounds, uint32_t p_bound_count, const NextGILight *p_lights, uint32_t p_light_count, uint32_t p_cap, LocalVector<uint32_t> &r_data);

	NextGIRelight();
	~NextGIRelight();
};

} // namespace RendererRD
