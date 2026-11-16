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

#include "core/math/transform_3d.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/shaders/vsm_page_table.glsl.gen.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

namespace RendererRD {

// GodotNext: vsm_page_pool (#220). The RenderingDevice side of the Virtual Shadow Map core: the
// physical pool texture, the page-table buffer, the clipmap scroll pass and the page-table
// self-check. It exists with the Forward+ renderer only, and allocates nothing until VSM is active.
//
// Nothing turns VSM on for a project yet: the shadow-method settings come with #221. Until then
// GODOT_DEV_VSM in the environment (builds with DEBUG_ENABLED) turns the core on for every
// shadow-casting light: 1 picks the High pool, or name the pool quality (low, medium, high, epic).
// The lights keep their atlas shadows; the maps only fill LightData's vsm_map_base and vsm_flags.
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

	RID pool_texture;
	Vector2i pool_texture_size;
	uint64_t pool_driver_bytes = 0;
	RID table_buffer;
	uint64_t table_buffer_bytes = 0;

	bool active = false;
	PoolQuality active_quality = POOL_QUALITY_HIGH;

	bool _ensure_shader();
	RID _shader(ShaderMode p_mode);

protected:
	bool _table_resize(uint32_t p_old_maps, uint32_t p_new_maps) override;
	void _table_clear(uint32_t p_first_map, uint32_t p_map_count) override;
	void _table_scroll(uint32_t p_map, const ClipmapScroll &p_scroll) override;
	bool _pool_resize(uint32_t p_page_count) override;

public:
	static VirtualShadowMapStorage *get_singleton() { return singleton; }

	// GODOT_DEV_VSM, read once. False in builds without DEBUG_ENABLED.
	static bool is_dev_switch_on(PoolQuality *r_quality = nullptr);

	bool is_active() const { return active; }
	RID get_pool_texture() const { return pool_texture; }
	RID get_page_table_buffer() const { return table_buffer; }

	// LightStorage::update_light_buffers() calls this for every light it writes. It allocates or
	// keeps the light's maps (and moves a directional light's clipmap levels to the camera when
	// p_move_clipmaps is set), marks them used in the renderer's current frame, and returns
	// LightData's vsm_map_base and vsm_flags: MAP_NONE and 0 for a light without maps.
	void update_light(RID p_light_instance, MapKind p_kind, bool p_casts_shadow, const Basis &p_light_basis, const Vector3 &p_camera_position, float p_max_distance, bool p_move_clipmaps, uint32_t &r_map_base, uint32_t &r_flags);
	// LightStorage::light_instance_free() calls this; the light's entries are cleared in this frame.
	void light_instance_freed(RID p_light_instance);

	Dictionary get_debug_info() const;
	// Runs the self-check of RenderingServer.virtual_shadow_maps_run_self_check().
	Dictionary run_self_check(uint32_t p_seed, uint32_t p_query_count, uint32_t p_cycles);

	VirtualShadowMapStorage();
	~VirtualShadowMapStorage() override;
};

} // namespace RendererRD
