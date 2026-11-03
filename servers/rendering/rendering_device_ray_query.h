/**************************************************************************/
/*  rendering_device_ray_query.h                                          */
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

#include "servers/rendering/rendering_device_driver.h"

// GodotNext: validation rules of the ray-query-only RenderingDevice API (plan D5 and §9 RT API
// items 1, 4 and 5). RenderingDevice applies them, and the unit tests check them directly.
namespace RDRayQuery {

inline constexpr const char *ERROR_NO_RAYTRACING_PIPELINE = "The current rendering device has no raytracing pipeline support.";
inline constexpr const char *ERROR_AS_INVALIDATED = "Acceleration structure either has not been built yet, or has been invalidated by an operation and needs to be rebuilt.";
inline constexpr const char *ERROR_AS_RAY_QUERY_ONLY = "TLAS built with hit_sbt_range 0 is ray-query only. Bind it to a compute list, or rebuild it with hit shader binding table ranges.";

// A TLAS instance with a BLAS and a zero hit shader binding table range has no hit shaders. Range 0
// decodes to SBT offset 0, so such an instance can be traced by ray queries but not by an RT pipeline.
inline bool is_ray_query_only_instance(bool p_has_blas, int64_t p_hit_sbt_range) {
	return p_has_blas && p_hit_sbt_range == 0;
}

// Acceleration structure build inputs (vertex and index data) are read through their device address.
inline bool has_build_input_usage(BitField<RDD::BufferUsageBits> p_usage) {
	return p_usage.has_flag(RDD::BUFFER_USAGE_DEVICE_ADDRESS_BIT) && p_usage.has_flag(RDD::BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
}

inline String get_build_input_usage_error(const String &p_buffer_kind, uint32_t p_geometry_index) {
	return "The " + p_buffer_kind + " buffer of geometry " + itos(p_geometry_index) + " was not created with BUFFER_CREATION_DEVICE_ADDRESS_BIT and BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT, which acceleration structure build inputs need.";
}

inline bool is_valid_index_format(RDD::IndexBufferFormat p_format) {
	return p_format == RDD::INDEX_BUFFER_FORMAT_UINT16 || p_format == RDD::INDEX_BUFFER_FORMAT_UINT32;
}

enum BindStatus {
	BIND_OK,
	BIND_INVALIDATED,
	BIND_RAY_QUERY_ONLY,
};

// Checks an acceleration structure in a uniform set that is bound to a compute list or, with
// p_raytracing_list, to a raytracing list.
inline BindStatus get_bind_status(bool p_invalidated, bool p_has_ray_query_only_instances, bool p_raytracing_list) {
	if (p_invalidated) {
		return BIND_INVALIDATED;
	}
	if (p_raytracing_list && p_has_ray_query_only_instances) {
		return BIND_RAY_QUERY_ONLY;
	}
	return BIND_OK;
}

inline const char *get_bind_error(BindStatus p_status) {
	switch (p_status) {
		case BIND_INVALIDATED:
			return ERROR_AS_INVALIDATED;
		case BIND_RAY_QUERY_ONLY:
			return ERROR_AS_RAY_QUERY_ONLY;
		case BIND_OK:
		default:
			return "";
	}
}

// GodotNext: as_build_modes
// UPDATE is rejected before any driver command. Check order: an unknown mode, then a missing
// ALLOW_UPDATE flag, then a structure that has not been built, then a top-level instance-count change.
enum UpdateStatus {
	UPDATE_OK,
	UPDATE_INVALID_MODE,
	UPDATE_MISSING_ALLOW_UPDATE,
	UPDATE_NOT_BUILT,
	UPDATE_INSTANCE_COUNT_CHANGED,
};

inline UpdateStatus get_update_status(RDD::AccelerationStructureBuildMode p_mode, bool p_allow_update, bool p_invalidated, bool p_is_tlas, uint32_t p_last_instance_count, uint32_t p_instance_count) {
	if (p_mode == RDD::ACCELERATION_STRUCTURE_BUILD_MODE_BUILD) {
		return UPDATE_OK;
	}
	if (p_mode != RDD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE) {
		return UPDATE_INVALID_MODE;
	}
	if (!p_allow_update) {
		return UPDATE_MISSING_ALLOW_UPDATE;
	}
	if (p_invalidated) {
		return UPDATE_NOT_BUILT;
	}
	if (p_is_tlas && p_instance_count != p_last_instance_count) {
		return UPDATE_INSTANCE_COUNT_CHANGED;
	}
	return UPDATE_OK;
}

inline const char *get_update_error(UpdateStatus p_status) {
	switch (p_status) {
		case UPDATE_MISSING_ALLOW_UPDATE:
			return "Acceleration structure UPDATE requires ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT.";
		case UPDATE_NOT_BUILT:
			return "Acceleration structure UPDATE requires a previous BUILD. The structure has not been built, or it was invalidated.";
		case UPDATE_INSTANCE_COUNT_CHANGED:
			return "Top-level acceleration structure UPDATE cannot change the instance count. Rebuild it instead.";
		case UPDATE_INVALID_MODE:
			return "Acceleration structure build mode is not BUILD or UPDATE.";
		case UPDATE_OK:
		default:
			return "";
	}
}

} // namespace RDRayQuery
