/**************************************************************************/
/*  virtual_shadow_map_storage.h                                          */
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

#include "core/math/aabb.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/shaders/vsm_allocate_pages.glsl.gen.h"
// GodotNext: vsm_caching (#212).
#include "servers/rendering/renderer_rd/shaders/vsm_invalidate.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/vsm_mark_pages.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/vsm_page_table.glsl.gen.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

namespace RendererRD {

// GodotNext: vsm_page_pool (#220). The RenderingDevice side of the Virtual Shadow Map core: the
// physical pool texture, the page-table buffer, the clipmap scroll pass and the page-table
// self-check. It exists with the Forward+ renderer only, and allocates nothing until VSM is active.
//
// GodotNext: vsm_settings_debug (#221). rendering/lights_and_shadows/shadow_method = virtual (read at startup)
// or a light's LIGHT_SHADOW_METHOD_VIRTUAL turns the core on when the tier rule allows it; then every
// shadow-casting light whose method resolves to VIRTUAL gets maps. GODOT_DEV_VSM in the environment
// (builds with DEBUG_ENABLED) stands for shadow_method = virtual and picks the pool: 1 the High pool, or
// the pool quality (low, medium, high, epic), or a percentage of the default High pool ("25%"). The
// maps fill LightData's vsm_map_base and vsm_flags, and the Forward+ light loop samples them (#222).
//
// GodotNext: vsm_page_marking (#219). With VSM on, every view with Forward+ render buffers forces the
// depth prepass and, at the end of RenderForwardClustered::_pre_opaque_render() (once the light
// buffers and the cluster are built), runs mark_and_allocate(): vsm_mark_pages.glsl flags the pages
// the view samples, plus every light's always-resident pages, and vsm_allocate_pages.glsl gives them
// physical pages (the resident ones first) and emits the dirty-page list and its indirect arguments
// for the raster passes (#216, #217, #218).
class VirtualShadowMapStorage : public RendererVirtualShadowMapStorage {
	static VirtualShadowMapStorage *singleton;

	enum ShaderMode {
		SHADER_MODE_SCROLL,
		SHADER_MODE_LOOKUP_CHECK,
		SHADER_MODE_MAX,
	};

	struct ScrollPushConstant {
		uint32_t map = 0;
		// Per mip: column start in bits 0-7, column count in 8-15, row start in 16-23, row count in 24-31.
		uint32_t mip_ranges[MIP_COUNT] = {};
	};
	static_assert(sizeof(ScrollPushConstant) % 16 == 0, "Push constants are a multiple of 16 bytes.");
	// A scroll count can be a whole mip side; each start and count gets 8 bits.
	static_assert(PAGES_PER_SIDE < 256, "Scroll ranges are packed in 8 bits: widen ScrollPushConstant::mip_ranges and vsm_page_table.glsl for 256 or more pages per side.");

	struct CheckPushConstant {
		uint32_t count = 0;
		uint32_t pad[3] = {};
	};

	VsmPageTableShaderRD shader;
	RID shader_version;
	RID pipelines[SHADER_MODE_MAX];
	bool shader_ready = false;
	bool shader_failed = false; // ShaderRD initializes once: a failed build is not retried.

	// GodotNext: vsm_page_marking (#219).
	enum MarkMode {
		MARK_MODE_PIXEL,
		MARK_MODE_FROXEL,
		MARK_MODE_BOX,
		MARK_MODE_RESIDENT,
		MARK_MODE_MAX,
	};
	enum AllocateMode {
		ALLOCATE_MODE_UPDATE,
		ALLOCATE_MODE_SCAN,
		ALLOCATE_MODE_SCATTER,
		ALLOCATE_MODE_ASSIGN,
		ALLOCATE_MODE_MAPS,
		ALLOCATE_MODE_EMIT,
		ALLOCATE_MODE_CLEAR,
		// GodotNext: vsm_caching (#212).
		ALLOCATE_MODE_DEFER_SCAN,
		ALLOCATE_MODE_DEFER_APPLY,
		ALLOCATE_MODE_MAX,
	};
	// GodotNext: vsm_caching (#212).
	enum InvalidateMode {
		INVALIDATE_MODE_MAPS,
		INVALIDATE_MODE_BOXES,
		INVALIDATE_MODE_MAX,
	};
	struct InvalidatePushConstant {
		uint32_t count = 0;
		uint32_t table_maps = 0;
		uint32_t pool_pages = 0;
		uint32_t grid_width = 0;
	};
	static_assert(sizeof(InvalidatePushConstant) == 16, "InvalidatePushConstant matches vsm_invalidate.glsl.");
	struct PendingMapInvalidation {
		uint32_t map = 0;
		uint32_t dirty_flags = 0;
		uint32_t dynamic_if_used = 0;
		uint32_t pad = 0;
	};
	static_assert(sizeof(PendingMapInvalidation) == 16, "PendingMapInvalidation matches uvec4 in vsm_invalidate.glsl.");
	// VSMViewParams in vsm_state_inc.glsl (std430).
	struct ViewParams {
		float inv_projection[4][4] = {};
		float view_to_world[3][4] = {};
		uint32_t screen[4] = {};
		float footprint[4] = {};
		uint32_t cluster[4] = {};
		uint32_t counts[4] = {};
		uint32_t misc[4] = {};
		int32_t bias[4] = {};
		uint32_t fog_size[4] = {};
		float fog_a[4] = {};
		float fog_b[4] = {};
		uint32_t directional[MAX_DIRECTIONAL_LIGHTS][4] = {};
	};
	static_assert(sizeof(ViewParams) == 384, "ViewParams must match VSMViewParams in vsm_state_inc.glsl.");
	struct AllocatePushConstant {
		uint32_t frame = 0;
		uint32_t cache_frames = 0;
		uint32_t table_maps = 0;
		uint32_t pool_pages = 0;
		uint32_t grid_width = 0;
		uint32_t request_capacity = 0;
		uint32_t phase = 0;
		uint32_t deferred_budget = 0; // GodotNext: vsm_caching (#212).
	};
	static_assert(sizeof(AllocatePushConstant) % 16 == 0, "Push constants are a multiple of 16 bytes.");
	// A directional light of the current view (VSMViewParams::directional).
	struct DirectionalEntry {
		uint32_t map_base = MAP_NONE;
		uint32_t flags = 0;
		float max_distance = 0.0f;
		float fog_energy = 0.0f;
	};

	VsmMarkPagesShaderRD mark_shader;
	RID mark_version;
	RID mark_pipelines[MARK_MODE_MAX];
	VsmAllocatePagesShaderRD allocate_shader;
	RID allocate_version;
	RID allocate_pipelines[ALLOCATE_MODE_MAX];
	// GodotNext: vsm_caching (#212).
	VsmInvalidateShaderRD invalidate_shader;
	RID invalidate_version;
	RID invalidate_pipelines[INVALIDATE_MODE_MAX];
	bool marking_ready = false;
	bool marking_failed = false;

	// Sized by the page table's map count, and grown with it in _table_resize().
	static constexpr uint32_t MAP_BUFFER_BYTES_PER_MAP = sizeof(MapData) + MAP_STATE_REGIONS * sizeof(uint32_t);
	RID map_data_buffer;
	RID map_state_buffer;
	uint32_t map_buffers_maps = 0;
	LocalVector<MapData> map_data;
	uint32_t map_data_dirty_begin = 0;
	uint32_t map_data_dirty_end = 0;
	// GodotNext: vsm_caching (#212). Per-map shadow caster mask uploaded into MAP_STATE_CULL_MASK.
	LocalVector<uint32_t> map_cull_masks;
	bool map_cull_masks_dirty = false;
	// Sized by the pool.
	RID pages_buffer;
	RID free_list_buffer;
	RID dirty_buffer;
	RID request_buffer;
	RID page_coverage_buffer; // GodotNext: vsm_caching (#212). Screen pixels per physical page.
	uint32_t page_buffers_pages = 0;
	uint32_t request_buffer_capacity = 0;
	// Fixed size / dynamically grown.
	RID state_buffer;
	RID view_buffer;
	RID indirect_buffer;
	RID box_buffer;
	RID invalidation_box_buffer; // GodotNext: vsm_caching (#212).
	uint32_t invalidation_box_capacity = 0;
	RID invalidation_map_buffer;
	uint32_t invalidation_map_capacity = 0;
	LocalVector<PendingMapInvalidation> pending_map_invalidations;
	uint32_t deferred_invalidation_budget = 0;
	uint64_t last_invalidation_epoch = 0;
	uint64_t last_invalidation_boxes = 0;
	uint64_t last_invalidation_maps = 0;
	uint64_t total_invalidation_boxes = 0;
	uint64_t total_invalidation_maps = 0;
	uint64_t static_mode_invalidations = 0;
	RID capture_depth_buffer;
	uint32_t capture_depth_words = 0;
	uint64_t processed_pool_reallocations = 0;
	LocalVector<DirectionalEntry> view_directional;
	int32_t lod_bias_directional = 0;
	int32_t lod_bias_local = 0;
	uint32_t active_cache_frames = cache_frames(POOL_QUALITY_HIGH);
	uint64_t marked_views = 0;
	// GodotNext: vsm_non_nanite (#218).
	float min_caster_texels = DEFAULT_MIN_CASTER_TEXELS;
	bool raster_check_armed = false;
	Dictionary (*raster_check_getter)(void *p_userdata) = nullptr;
	void (*raster_check_arm_callback)(void *p_userdata) = nullptr;
	void *raster_check_userdata = nullptr;
	Dictionary last_raster_stats;
	// GodotNext: vsm_nanite (#217).
	Dictionary last_nanite_stats;

	// The GPU check (RenderingServer.virtual_shadow_maps_request_check()): the next marked view copies
	// its inputs and the state after marking and after allocation into these buffers.
	struct CheckCapture {
		bool armed = false;
		bool captured = false;
		ViewParams view;
		uint32_t table_maps = 0;
		uint32_t pool_pages = 0;
		uint32_t grid_width = 0;
		uint32_t request_capacity = 0;
		uint32_t frame = 0;
		uint32_t cache_frames = 0;
		uint32_t deferred_budget = 0; // GodotNext: vsm_caching (#212).
		uint32_t omni_count = 0;
		uint32_t spot_count = 0;
		uint32_t area_count = 0;
		bool fog = false;
		LocalVector<AABB> boxes;
		LocalVector<MapData> map_data;
		RID cluster;
		uint32_t cluster_bytes = 0;
		RID lights[3];
		RID bias_before;
		RID table_marked;
		RID pages_before;
		RID state_marked;
		RID requests;
		RID map_state_marked;
		RID table_after;
		RID pages_after;
		RID state_after;
		RID map_state_after;
		RID dirty;
		RID indirect;
		RID page_coverage; // GodotNext: vsm_caching (#212).
		// The page clear: the pool is filled with a non-zero value before allocation, copied after the
		// clear pass, then restored from the copy taken before and cleared again, as without the check.
		RID pool_saved;
		RID pool_cleared;
	};
	CheckCapture check;

	bool _ensure_marking_shaders();
	void _resize_map_buffers(uint32_t p_maps);
	void _ensure_page_buffers();
	void _set_map_data(uint32_t p_map, const MapData &p_data);
	// GodotNext: vsm_caching (#212).
	void _set_map_cull_mask(uint32_t p_map, uint32_t p_mask);
	void _upload_map_data();
	void _free_marking_resources();
	void _free_check_buffers();
	RID _snapshot(RID p_buffer, uint32_t p_offset, uint32_t p_bytes, const char *p_name);
	RID _pool_snapshot(const char *p_name);
	void _pool_copy(RID p_from, RID p_to);

	RID pool_texture;
	Vector2i pool_texture_size;
	uint64_t pool_driver_bytes = 0;
	RID table_buffer;
	uint64_t table_buffer_bytes = 0;

	bool active = false;
	PoolQuality active_quality = POOL_QUALITY_HIGH;
	// GodotNext: vsm_settings_debug (#221).
	TierReason tier_reason = TIER_REASON_UNSUPPORTED_RENDERER;
	bool activation_failed = false;
	Scalability scalability = default_scalability(DEFAULT_QUALITY);
	uint32_t settings_version = 0;
	static constexpr uint32_t STATS_MAX_VIEWS = 16; // Views of one frame counted in RENDERING_INFO_VSM_PAGES_*.
	RID stats_buffer;
	uint32_t stats_views = 0;
	uint64_t stats_frame = UINT64_MAX;
	void _update_scalability();
	void _record_frame_stats(uint64_t p_frame, bool p_view_marked);

	// GodotNext: vsm_projection (#222).
	bool view_samples = false;
	uint32_t view_sampled_lights = 0;
	struct ProjectionStats {
		uint32_t lights_sampled = 0;
		uint32_t atlas_passes = 0;
		uint32_t atlas_passes_skipped = 0;
		uint32_t rays_local = 0;
		uint32_t rays_directional = 0;
		uint32_t steps = 0;
		bool prefiltered_distant = false;
	};
	ProjectionStats pending_projection;
	ProjectionStats last_projection;
	uint64_t projection_views = 0;

	bool _ensure_shader();
	RID _shader(ShaderMode p_mode);

protected:
	bool _table_resize(uint32_t p_old_maps, uint32_t p_new_maps) override;
	void _table_clear(uint32_t p_first_map, uint32_t p_map_count) override;
	void _table_scroll(uint32_t p_map, const ClipmapScroll &p_scroll) override;
	// GodotNext: vsm_caching (#212).
	void _table_invalidate_map(uint32_t p_map, uint32_t p_dirty_flags, bool p_dynamic_if_used = false) override;
	bool _pool_resize(uint32_t p_page_count) override;

public:
	static VirtualShadowMapStorage *get_singleton() { return singleton; }

	// GodotNext: vsm_page_marking (#219). LightStorage::LightData's layout (light_data_inc.glsl), for the
	// GPU check's read-back of the light buffers; LightStorage::update_light_buffers() asserts it.
	static constexpr uint32_t LIGHT_DATA_BYTES = 240;
	static constexpr uint32_t LIGHT_DATA_POSITION = 0;
	static constexpr uint32_t LIGHT_DATA_INV_RADIUS = 12;
	static constexpr uint32_t LIGHT_DATA_DIRECTION = 16;
	static constexpr uint32_t LIGHT_DATA_AREA_INV_RANGE = 60;
	static constexpr uint32_t LIGHT_DATA_COS_SPOT_ANGLE = 76;
	static constexpr uint32_t LIGHT_DATA_FOG_ENERGY = 200;
	static constexpr uint32_t LIGHT_DATA_VSM_MAP_BASE = 224;
	static constexpr uint32_t LIGHT_DATA_VSM_FLAGS = 228;
	// A LightData record (LIGHT_DATA_BYTES / 4 words) as the mark pass reads it.
	static MarkLight _mark_light(const uint32_t *p_light_words, MapKind p_kind);

	// GODOT_DEV_VSM, read once. False in builds without DEBUG_ENABLED. r_pool_pages is the pool's page
	// count: the quality's, or a percentage of the default High pool's.
	static bool is_dev_switch_on(PoolQuality *r_quality = nullptr, uint32_t *r_pool_pages = nullptr);

	bool is_active() const { return active; }
	// GodotNext: vsm_settings_debug (#221). The tier rule's device half: SUPPORTS_IMAGE_ATOMIC_32_BIT and
	// SUPPORTS_DRAW_INDIRECT_COUNT, which a RenderingDevice without a driver can mock.
	static void collect_device_inputs(const RenderingDevice *p_device, bool p_forward_plus, TierInputs &r_inputs);
	// Creates the pool once, when the tier allows it: at startup for a virtual project, or when a light first
	// asks for LIGHT_SHADOW_METHOD_VIRTUAL. Returns whether VSM is active.
	bool request_activation();
	TierReason get_device_tier_reason() const { return tier_reason; }
	const Scalability &get_scalability() const { return scalability; }
	// The views mark_and_allocate() has marked; it rises once per view whose dirty list it wrote.
	uint64_t get_marked_view_count() const { return marked_views; }

	// GodotNext: vsm_projection (#222). The active VSM quality preset (the pool's), its LOD biases, and whether
	// the view whose light buffers update_light() writes samples virtual shadow maps: then every light with
	// maps gets LIGHT_FLAGS_SAMPLE.
	PoolQuality get_quality() const { return active_quality; }
	int32_t get_lod_bias_directional() const { return lod_bias_directional; }
	int32_t get_lod_bias_local() const { return lod_bias_local; }
	void set_view_samples(bool p_samples) {
		view_samples = p_samples;
		if (p_samples) {
			view_sampled_lights = 0;
		}
	}
	bool get_view_samples() const { return view_samples; }
	// The view that samples the maps: its shadow-atlas passes kept and skipped, after update_light() ran for
	// its lights; then its SMRT counts (set_projection_params()). get_debug_info() reports them as "projection".
	void record_projection_view(uint32_t p_atlas_passes, uint32_t p_atlas_passes_skipped) {
		pending_projection = ProjectionStats();
		pending_projection.lights_sampled = view_sampled_lights;
		pending_projection.atlas_passes = p_atlas_passes;
		pending_projection.atlas_passes_skipped = p_atlas_passes_skipped;
	}
	void set_projection_params(uint32_t p_rays_local, uint32_t p_rays_directional, uint32_t p_steps, bool p_prefiltered_distant) {
		pending_projection.rays_local = p_rays_local;
		pending_projection.rays_directional = p_rays_directional;
		pending_projection.steps = p_steps;
		pending_projection.prefiltered_distant = p_prefiltered_distant;
		last_projection = pending_projection;
		projection_views++;
	}
	RID get_pool_texture() const { return pool_texture; }
	RID get_page_table_buffer() const { return table_buffer; }
	// GodotNext: vsm_non_nanite (#218). Read-only access to the marking/allocation outputs for raster passes.
	RID get_map_data_buffer() const { return map_data_buffer; }
	RID get_map_state_buffer() const { return map_state_buffer; }
	RID get_dirty_buffer() const { return dirty_buffer; }
	RID get_state_buffer() const { return state_buffer; }
	// GodotNext: vsm_nanite (#217). Indirect dispatch args written by MODE_MAPS (VSM_INDIRECT_ALL).
	RID get_indirect_buffer() const { return indirect_buffer; }
	void set_last_nanite_stats(const Dictionary &p_stats) { last_nanite_stats = p_stats; }
	uint32_t get_map_buffers_capacity() const { return map_buffers_maps; }
	uint32_t get_page_buffers_capacity() const { return page_buffers_pages; }
	const LocalVector<MapData> &get_map_data() const { return map_data; }
	RID snapshot_buffer(RID p_buffer, uint32_t p_offset, uint32_t p_bytes, const char *p_name) { return _snapshot(p_buffer, p_offset, p_bytes, p_name); }
	RID snapshot_pool(const char *p_name) { return _pool_snapshot(p_name); }
	void set_min_caster_texels(float p_min_texels) { min_caster_texels = MAX(p_min_texels, 0.0f); }
	float get_min_caster_texels() const { return min_caster_texels; }
	void set_raster_check_callbacks(void (*p_arm)(void *), Dictionary (*p_get)(void *), void *p_userdata) {
		raster_check_arm_callback = p_arm;
		raster_check_getter = p_get;
		raster_check_userdata = p_userdata;
	}
	bool consume_raster_check_request() {
		const bool armed = raster_check_armed;
		raster_check_armed = false;
		return armed;
	}
	bool is_raster_check_armed() const { return raster_check_armed; }
	void set_last_raster_stats(const Dictionary &p_stats) { last_raster_stats = p_stats; }
	void request_raster_check();
	Dictionary get_raster_check_result();

	// GodotNext: vsm_page_marking (#219). What update_light() needs of a light.
	struct LightUpdate {
		MapKind kind = MAP_KIND_SPOT;
		bool casts_shadow = false;
		Transform3D transform; // World space.
		Vector3 camera_position;
		float max_distance = 0.0f; // Directional: the shadow max distance (INF for the far plane).
		bool move_clipmaps = false;
		float range = 0.0f; // Spot, omni and area.
		float spot_angle = 0.0f; // Degrees.
		float fog_energy = 0.0f;
		// GodotNext: vsm_caching (#212).
		Vector2 area_size;
		uint32_t caster_mask = 0xFFFFFFFFu;
		bool reverse_cull = false;
	};
	// LightStorage::update_light_buffers() calls begin_light_update() once, then this for every light it
	// writes. It allocates or keeps the light's maps (and moves a directional light's clipmap levels to
	// the camera when move_clipmaps is set), marks them used in the renderer's current frame, writes
	// each map's MapData, and returns LightData's vsm_map_base and vsm_flags: MAP_NONE and 0 for a
	// light without maps.
	void begin_light_update();
	void update_light(RID p_light_instance, const LightUpdate &p_light, uint32_t &r_map_base, uint32_t &r_flags);
	// LightStorage::light_instance_free() calls this; the light's entries are cleared in this frame.
	void light_instance_freed(RID p_light_instance);

	// GodotNext: vsm_page_marking (#219). One view's page marking and allocation inputs.
	struct MarkInput {
		RID depth_texture; // Resolved prepass depth (reversed Z).
		Size2i size;
		Projection inv_projection; // Depth-texture clip space to view space (NextGIView::inv_projection()).
		Transform3D cam_transform;
		float pixel_scale = 0.0f; // A pixel's footprint in meters: view depth * pixel_scale + pixel_bias.
		float pixel_bias = 0.0f;
		float z_far = 1.0f;
		RID cluster_buffer;
		uint32_t cluster_size = 32;
		uint32_t cluster_max_elements = 0;
		RID omni_buffer;
		RID spot_buffer;
		RID area_buffer;
		uint32_t omni_count = 0;
		uint32_t spot_count = 0;
		uint32_t area_count = 0;
		bool fog = false; // Volumetric fog: the froxel grid requests coarse pages.
		Vector3i fog_size;
		float fog_length = 0.0f;
		float fog_spread = 1.0f;
		Vector2 fog_near_size;
		Vector2 fog_far_size;
		LocalVector<AABB> boxes; // Transparent instances' world boxes: coarse pages.
		// GodotNext: vsm_caching (#212). Swept-bounds invalidation boxes from RendererSceneCull.
		const InvalidationBox *invalidation_boxes = nullptr;
		uint32_t invalidation_box_count = 0;
		uint64_t invalidation_epoch = 0;
		uint64_t static_mode_invalidations = 0;
		bool has_dynamic_casters = false;
	};
	// GodotNext: vsm_caching (#212).
	uint32_t get_deferred_invalidation_budget() const { return deferred_invalidation_budget; }
	// Whether VSM marks pages for views (VSM on, the pool and the marking shaders ready).
	bool is_marking_enabled();
	void mark_and_allocate(const MarkInput &p_input);
	// The configurable resolution LOD bias, in mips (#221 wires the settings).
	void set_lod_bias(int32_t p_directional, int32_t p_local);
	// The GPU check of RenderingServer.virtual_shadow_maps_request_check(): arms the next marked view's
	// capture; get_check_result() compares it with the CPU models once that view has run.
	void request_check();
	Dictionary get_check_result();

	Dictionary get_debug_info() const;
	// Runs the self-check of RenderingServer.virtual_shadow_maps_run_self_check().
	Dictionary run_self_check(uint32_t p_seed, uint32_t p_query_count, uint32_t p_cycles);

	VirtualShadowMapStorage();
	~VirtualShadowMapStorage() override;
};

} // namespace RendererRD
