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

} // namespace RDRayQuery
