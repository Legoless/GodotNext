/**************************************************************************/
/*  nextgeo_visibility.h                                                  */
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

#include "servers/rendering/renderer_rd/forward_clustered/nextgi_velocity.h"
#include "servers/rendering/rendering_device.h"

#include <stdint.h>

namespace RendererRD {

// GodotNext: nextgeo_visibility_raster (#83, plan §6.1, §6.3, §7.4).
// Visibility ID contract: R32_UINT = (25-bit visible-cluster index << 7) | 7-bit triangle.
// Background sentinel is 0xFFFFFFFF; instance-ID legacy sentinel is 0xFFFFFFFF.
static constexpr uint32_t NEXTGEO_VISIBILITY_TRIANGLE_BITS = 7u;
static constexpr uint32_t NEXTGEO_VISIBILITY_TRIANGLE_MASK = (1u << NEXTGEO_VISIBILITY_TRIANGLE_BITS) - 1u;
static constexpr uint32_t NEXTGEO_MAX_CLUSTER_TRIANGLES = 1u << NEXTGEO_VISIBILITY_TRIANGLE_BITS; // 128
static constexpr uint32_t NEXTGEO_RASTER_VERTEX_COUNT = 3u * NEXTGEO_MAX_CLUSTER_TRIANGLES; // 384
static constexpr uint32_t NEXTGEO_VISIBILITY_BACKGROUND = 0xFFFFFFFFu;
static constexpr uint32_t NEXTGEO_INSTANCE_ID_LEGACY = 0xFFFFFFFFu;
// Cluster index 0x1FFFFFF with triangle 127 packs to 0xFFFFFFFF (the background sentinel),
// so valid visible-cluster indices stay strictly below 0x1FFFFFF.
static constexpr uint32_t NEXTGEO_VISIBILITY_MAX_CLUSTERS = (1u << 25u) - 1u;

struct NextGeoDrawIndirectCommand {
	uint32_t vertex_count = NEXTGEO_RASTER_VERTEX_COUNT;
	uint32_t instance_count = 0;
	uint32_t first_vertex = 0;
	uint32_t first_instance = 0;
};
static_assert(sizeof(NextGeoDrawIndirectCommand) == 16, "Indirect draw command must match VkDrawIndirectCommand (16 B).");

inline NextGeoDrawIndirectCommand nextgeo_make_draw_indirect(uint32_t p_instance_count) {
	NextGeoDrawIndirectCommand cmd;
	cmd.vertex_count = NEXTGEO_RASTER_VERTEX_COUNT;
	cmd.instance_count = p_instance_count;
	cmd.first_vertex = 0;
	cmd.first_instance = 0; // D3D12-safe zero base instance (plan §7.4 D2).
	return cmd;
}

inline bool nextgeo_visibility_is_background(uint32_t p_visibility_id) {
	return p_visibility_id == NEXTGEO_VISIBILITY_BACKGROUND;
}

inline uint32_t nextgeo_pack_visibility_id(uint32_t p_visible_cluster, uint32_t p_triangle) {
	if (p_visible_cluster >= NEXTGEO_VISIBILITY_MAX_CLUSTERS || p_triangle >= NEXTGEO_MAX_CLUSTER_TRIANGLES) {
		return NEXTGEO_VISIBILITY_BACKGROUND;
	}
	return (p_visible_cluster << NEXTGEO_VISIBILITY_TRIANGLE_BITS) | (p_triangle & NEXTGEO_VISIBILITY_TRIANGLE_MASK);
}

inline uint32_t nextgeo_unpack_visibility_cluster(uint32_t p_visibility_id) {
	return p_visibility_id >> NEXTGEO_VISIBILITY_TRIANGLE_BITS;
}

inline uint32_t nextgeo_unpack_visibility_triangle(uint32_t p_visibility_id) {
	return p_visibility_id & NEXTGEO_VISIBILITY_TRIANGLE_MASK;
}

// NextGI always writes prepass velocity (for its own temporal passes); NextGeo
// activates the §6.1 velocity plan whenever a temporal consumer (TAA, FSR2,
// MetalFX, or an external DENSE upscaler such as DLSS) is active on the view.
inline NextGIVelocityPlan nextgeo_velocity_plan(NextGIVelocityScale p_scale, NextGIVelocityMode p_mode, bool p_using_taa, bool p_using_nextgi, bool p_using_nextgeo) {
	const bool active = p_using_nextgi || (p_using_nextgeo && p_mode != NEXTGI_VELOCITY_NONE);
	return nextgi_velocity_plan(p_scale, p_mode, p_using_taa, active);
}

// §6.1 PASS_MODE_COLOR load/clear rules. Stage B wrote -> load_color = true
// (DRAW_DEFAULT_ALL); otherwise clear color and specular as today, loading
// velocity on the TAA path. Never returns DRAW_IGNORE_ALL (LOAD_OP_DONT_CARE).
inline bool nextgeo_should_load_color(bool p_existing_load_color, bool p_nextgeo_drew, bool p_stage_b_wrote) {
	return p_existing_load_color || (p_nextgeo_drew && p_stage_b_wrote);
}

inline BitField<RenderingDevice::DrawFlags> nextgeo_color_pass_draw_flags(bool p_load_color, bool p_color_pass_loads_velocity, bool p_using_separate_specular, bool p_depth_pre_pass) {
	BitField<RenderingDevice::DrawFlags> flags = p_load_color ? RenderingDevice::DRAW_DEFAULT_ALL : RenderingDevice::DRAW_CLEAR_COLOR_ALL;
	if (p_color_pass_loads_velocity && !p_load_color) {
		// FramebufferCacheRD compacts out null RIDs: velocity is color attachment 1
		// without separate specular, and color attachment 2 with separate specular.
		flags = p_using_separate_specular
				? BitField<RenderingDevice::DrawFlags>(RenderingDevice::DRAW_CLEAR_COLOR_0 | RenderingDevice::DRAW_CLEAR_COLOR_1)
				: BitField<RenderingDevice::DrawFlags>(RenderingDevice::DRAW_CLEAR_COLOR_0);
	}
	flags = flags | (p_depth_pre_pass ? RenderingDevice::DRAW_DEFAULT_ALL : RenderingDevice::DRAW_CLEAR_DEPTH);
	return flags;
}

} // namespace RendererRD
