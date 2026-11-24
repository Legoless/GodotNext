/**************************************************************************/
/*  nextgeo_mesh_raster.h                                                 */
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

#include "core/string/ustring.h"
#include "core/typedefs.h"
#include "core/variant/variant.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_driver.h"

// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9, GN-10).
//
// Pure CPU constants, sizing math, and path selection for the NextGeo task/mesh hardware rasterizer
// (both the main visibility pass and the VSM hardware pass). Kept device-free so doctests check every
// driver/capability/setting combination (including mocked Metal Apple7/8 vs Apple9+ and missing
// SUPPORTS_MESH_SHADER) without a GPU.
namespace RendererRD {

// Task stage: 1 workgroup per 32 compacted HW-bin clusters; payload carries their 32 visible-cluster indices.
static constexpr uint32_t NEXTGEO_MESH_TASK_GROUP_SIZE = 32u;
static constexpr uint32_t NEXTGEO_MESH_TASK_PAYLOAD_BYTES = NEXTGEO_MESH_TASK_GROUP_SIZE * sizeof(uint32_t); // 128 B

// Mesh stage: 1 128-thread workgroup per cluster, capped at §7.1's 128 vertices and 128 primitives.
static constexpr uint32_t NEXTGEO_MESH_WORKGROUP_SIZE = 128u;
static constexpr uint32_t NEXTGEO_MESH_MAX_VERTICES = 128u;
static constexpr uint32_t NEXTGEO_MESH_MAX_PRIMITIVES = 128u;

// Number of task workgroups launched for p_hw_cluster_count compacted HW-bin clusters.
inline uint32_t nextgeo_mesh_task_groups(uint32_t p_hw_cluster_count) {
	if (p_hw_cluster_count == 0u) {
		return 0u;
	}
	return (p_hw_cluster_count - 1u) / NEXTGEO_MESH_TASK_GROUP_SIZE + 1u;
}

// Number of mesh workgroups emitted by task workgroup p_task_group_index for p_hw_cluster_count clusters.
inline uint32_t nextgeo_mesh_task_batch_count(uint32_t p_hw_cluster_count, uint32_t p_task_group_index) {
	const uint64_t base = uint64_t(p_task_group_index) * uint64_t(NEXTGEO_MESH_TASK_GROUP_SIZE);
	if (base >= uint64_t(p_hw_cluster_count)) {
		return 0u;
	}
	const uint64_t rem = uint64_t(p_hw_cluster_count) - base;
	return uint32_t(MIN(rem, uint64_t(NEXTGEO_MESH_TASK_GROUP_SIZE)));
}

// True when the device's reported mesh shader limits accommodate NextGeo's task and mesh stages
// (32-thread task, 128-thread mesh, 128 vertices, 128 primitives, 128-byte payload, and 32 mesh groups
// emitted per task workgroup).
inline bool nextgeo_mesh_shader_fits_limits(const RenderingDeviceDriver::MeshShaderLimits &p_limits) {
	return p_limits.max_task_workgroup_count[0] >= 1u &&
			p_limits.max_task_workgroup_total_count >= 1u &&
			p_limits.max_mesh_workgroup_count[0] >= NEXTGEO_MESH_TASK_GROUP_SIZE &&
			p_limits.max_mesh_workgroup_total_count >= NEXTGEO_MESH_TASK_GROUP_SIZE &&
			p_limits.max_task_workgroup_invocations >= NEXTGEO_MESH_TASK_GROUP_SIZE &&
			p_limits.max_task_workgroup_size[0] >= NEXTGEO_MESH_TASK_GROUP_SIZE &&
			p_limits.max_task_workgroup_size[1] >= 1u &&
			p_limits.max_task_workgroup_size[2] >= 1u &&
			p_limits.max_mesh_workgroup_invocations >= NEXTGEO_MESH_WORKGROUP_SIZE &&
			p_limits.max_mesh_workgroup_size[0] >= NEXTGEO_MESH_WORKGROUP_SIZE &&
			p_limits.max_mesh_workgroup_size[1] >= 1u &&
			p_limits.max_mesh_workgroup_size[2] >= 1u &&
			p_limits.max_mesh_output_vertices >= NEXTGEO_MESH_MAX_VERTICES &&
			p_limits.max_mesh_output_primitives >= NEXTGEO_MESH_MAX_PRIMITIVES &&
			p_limits.max_task_payload_size >= NEXTGEO_MESH_TASK_PAYLOAD_BYTES;
}

// rendering/nextgeo/hw_raster_path = auto | vertex | mesh (enum 0 | 1 | 2).
// auto keeps vertex until GN-10 passes on a driver in PQ (§22).
enum NextGeoHWRasterPath : uint32_t {
	NEXTGEO_HW_RASTER_PATH_AUTO = 0,
	NEXTGEO_HW_RASTER_PATH_VERTEX = 1,
	NEXTGEO_HW_RASTER_PATH_MESH = 2,
	NEXTGEO_HW_RASTER_PATH_MAX,
};

inline NextGeoHWRasterPath nextgeo_hw_raster_path_from_variant(const Variant &p_value) {
	if (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME) {
		const String s = String(p_value).strip_edges().to_lower();
		if (s == "vertex" || s == "vs") {
			return NEXTGEO_HW_RASTER_PATH_VERTEX;
		}
		if (s == "mesh") {
			return NEXTGEO_HW_RASTER_PATH_MESH;
		}
		return NEXTGEO_HW_RASTER_PATH_AUTO;
	}
	const int v = int(p_value);
	if (v == int(NEXTGEO_HW_RASTER_PATH_VERTEX)) {
		return NEXTGEO_HW_RASTER_PATH_VERTEX;
	}
	if (v == int(NEXTGEO_HW_RASTER_PATH_MESH)) {
		return NEXTGEO_HW_RASTER_PATH_MESH;
	}
	return NEXTGEO_HW_RASTER_PATH_AUTO;
}

inline const char *nextgeo_hw_raster_path_name(bool p_use_mesh) {
	return p_use_mesh ? "mesh" : "vertex";
}

// Why the HW rasterizer stayed on the VS vertex-pulling path (or NONE when using mesh shaders).
enum NextGeoHWRasterReason : uint32_t {
	NEXTGEO_HW_RASTER_REASON_NONE = 0,
	NEXTGEO_HW_RASTER_REASON_SETTING_VERTEX,
	NEXTGEO_HW_RASTER_REASON_AUTO_PENDING_PQ,
	NEXTGEO_HW_RASTER_REASON_NO_MESH_SHADER,
	NEXTGEO_HW_RASTER_REASON_NO_MESH_INDIRECT,
	NEXTGEO_HW_RASTER_REASON_NO_INDIRECT_COUNT,
	NEXTGEO_HW_RASTER_REASON_LIMITS_EXCEEDED,
	NEXTGEO_HW_RASTER_REASON_MULTIVIEW,
	NEXTGEO_HW_RASTER_REASON_SHADER_UNAVAILABLE,
	NEXTGEO_HW_RASTER_REASON_MAX,
};

inline const char *nextgeo_hw_raster_reason_name(NextGeoHWRasterReason p_reason) {
	switch (p_reason) {
		case NEXTGEO_HW_RASTER_REASON_NONE:
			return "NONE";
		case NEXTGEO_HW_RASTER_REASON_SETTING_VERTEX:
			return "SETTING_VERTEX";
		case NEXTGEO_HW_RASTER_REASON_AUTO_PENDING_PQ:
			return "AUTO_PENDING_PQ";
		case NEXTGEO_HW_RASTER_REASON_NO_MESH_SHADER:
			return "NO_MESH_SHADER";
		case NEXTGEO_HW_RASTER_REASON_NO_MESH_INDIRECT:
			return "NO_MESH_INDIRECT";
		case NEXTGEO_HW_RASTER_REASON_NO_INDIRECT_COUNT:
			return "NO_INDIRECT_COUNT";
		case NEXTGEO_HW_RASTER_REASON_LIMITS_EXCEEDED:
			return "LIMITS_EXCEEDED";
		case NEXTGEO_HW_RASTER_REASON_MULTIVIEW:
			return "MULTIVIEW";
		case NEXTGEO_HW_RASTER_REASON_SHADER_UNAVAILABLE:
			return "SHADER_UNAVAILABLE";
		default:
			return "UNKNOWN";
	}
}

struct NextGeoHWRasterCapabilities {
	bool supports_mesh_shader = false;
	bool supports_mesh_shader_indirect = false;
	bool supports_draw_indirect_count = false;
	RenderingDeviceDriver::MeshShaderLimits limits;
	// Flipped per driver only after GN-10 passes on that driver in PQ (§22).
	bool gn10_passed = false;
};

struct NextGeoHWRasterSelection {
	bool use_mesh = false;
	NextGeoHWRasterReason reason = NEXTGEO_HW_RASTER_REASON_AUTO_PENDING_PQ;
};

// Pure path selection for NextGeo HW rasterization (main visibility pass and VSM HW pass).
// Metal requires Apple9+ (captured via SUPPORTS_MESH_SHADER_INDIRECT); M2 (Apple8) and any device
// without mesh-shader or indirect-count support stay on VS. Multiview stays on VS/legacy (#178
// refuses mesh pipelines on multiview framebuffer formats).
inline NextGeoHWRasterSelection nextgeo_select_hw_raster_path(NextGeoHWRasterPath p_setting, const NextGeoHWRasterCapabilities &p_caps, bool p_multiview = false) {
	NextGeoHWRasterSelection sel;
	if (p_setting == NEXTGEO_HW_RASTER_PATH_VERTEX) {
		sel.reason = NEXTGEO_HW_RASTER_REASON_SETTING_VERTEX;
		return sel;
	}
	if (p_multiview) {
		sel.reason = NEXTGEO_HW_RASTER_REASON_MULTIVIEW;
		return sel;
	}
	if (!p_caps.supports_mesh_shader) {
		sel.reason = NEXTGEO_HW_RASTER_REASON_NO_MESH_SHADER;
		return sel;
	}
	if (!p_caps.supports_mesh_shader_indirect) {
		sel.reason = NEXTGEO_HW_RASTER_REASON_NO_MESH_INDIRECT;
		return sel;
	}
	if (!p_caps.supports_draw_indirect_count) {
		sel.reason = NEXTGEO_HW_RASTER_REASON_NO_INDIRECT_COUNT;
		return sel;
	}
	if (!nextgeo_mesh_shader_fits_limits(p_caps.limits)) {
		sel.reason = NEXTGEO_HW_RASTER_REASON_LIMITS_EXCEEDED;
		return sel;
	}
	if (p_setting == NEXTGEO_HW_RASTER_PATH_AUTO && !p_caps.gn10_passed) {
		sel.reason = NEXTGEO_HW_RASTER_REASON_AUTO_PENDING_PQ;
		return sel;
	}
	sel.use_mesh = true;
	sel.reason = NEXTGEO_HW_RASTER_REASON_NONE;
	return sel;
}

inline NextGeoHWRasterCapabilities nextgeo_hw_raster_capabilities_from_device(const RenderingDevice *p_device, bool p_gn10_passed = false) {
	NextGeoHWRasterCapabilities caps;
	if (p_device == nullptr) {
		return caps;
	}
	caps.supports_mesh_shader = p_device->has_feature(RenderingDevice::SUPPORTS_MESH_SHADER);
	caps.supports_mesh_shader_indirect = p_device->has_feature(RenderingDevice::SUPPORTS_MESH_SHADER_INDIRECT);
	caps.supports_draw_indirect_count = p_device->has_feature(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT);
	caps.limits = p_device->get_mesh_shader_limits();
	caps.gn10_passed = p_gn10_passed;
	return caps;
}

inline NextGeoHWRasterCapabilities nextgeo_hw_raster_capabilities_from_metal(uint32_t p_apple_family, uint32_t p_msl_version, bool p_argument_buffers, bool p_draw_indirect_count, bool p_gn10_passed = false) {
	NextGeoHWRasterCapabilities caps;
	const RenderingDeviceDriver::MeshShaderCaps mc = RenderingDeviceDriver::metal_mesh_shader_caps(p_apple_family, p_msl_version, p_argument_buffers);
	caps.supports_mesh_shader = mc.mesh;
	caps.supports_mesh_shader_indirect = mc.indirect;
	caps.supports_draw_indirect_count = p_draw_indirect_count;
	caps.limits = RenderingDeviceDriver::metal_mesh_shader_limits(p_apple_family, mc.mesh);
	caps.gn10_passed = p_gn10_passed;
	return caps;
}

inline NextGeoHWRasterCapabilities nextgeo_hw_raster_capabilities_from_d3d12(uint32_t p_mesh_shader_tier, bool p_dxil_mesh_stages, bool p_draw_indirect_count, bool p_gn10_passed = false) {
	NextGeoHWRasterCapabilities caps;
	String unused_log;
	const RenderingDeviceDriver::MeshShaderCaps mc = RenderingDeviceDriver::d3d12_mesh_shader_caps(p_mesh_shader_tier, p_dxil_mesh_stages, unused_log);
	caps.supports_mesh_shader = mc.mesh;
	caps.supports_mesh_shader_indirect = mc.indirect;
	caps.supports_draw_indirect_count = p_draw_indirect_count;
	caps.limits = RenderingDeviceDriver::d3d12_mesh_shader_limits(mc.mesh);
	caps.gn10_passed = p_gn10_passed;
	return caps;
}

} // namespace RendererRD
