/**************************************************************************/
/*  rendering_device_rt_pipeline.h                                        */
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
#include "core/templates/span.h"
#include "core/templates/vector.h"
#include "servers/rendering/rendering_device_driver.h"

// GodotNext: rd_rt_pipelines (#179). RT pipelines that evaluate materials in hit shaders (plan §21.1 C5).
// A pipeline has one layout; material data is reached through bindless tables plus a shader record of at
// most MAX_HIT_GROUP_RECORD_SIZE bytes after each hit group's handle. Pipeline libraries let a new material
// compile only its own hit group, off the render thread, and raytracing_pipeline_link() puts them
// together. RenderingDevice applies the rules here, and the unit tests check them without a GPU.
namespace RDRTPipeline {

// The largest shader record a hit group carries after its handle in the hit shader binding table.
static constexpr uint32_t MAX_HIT_GROUP_RECORD_SIZE = 32;
// The ray payload size, in bytes, that library pipelines and the pipelines linked from them declare.
// Linked libraries must all declare the same size, so it is one value for every library; a shader in a
// library or a linked pipeline must not use a larger payload.
static constexpr uint32_t LIBRARY_MAX_PAYLOAD_SIZE = 64;
// The size of an indirect trace command: the width, height and depth of the launch as three uint32.
static constexpr uint32_t INDIRECT_COMMAND_SIZE = 12;
// get_spirv_payload_size() for a payload whose size it cannot measure.
static constexpr uint32_t PAYLOAD_SIZE_UNKNOWN = UINT32_MAX;

// The SPIR-V capabilities of shader execution reordering (SER). The bits match
// RenderingDevice::LIMIT_RAYTRACING_REORDER_EXTENSIONS.
enum ReorderUsage : uint32_t {
	// ShaderInvocationReorderEXT: GLSL GL_EXT_shader_invocation_reorder, Vulkan
	// VK_EXT_ray_tracing_invocation_reorder.
	REORDER_USAGE_EXT = 1 << 0,
	// ShaderInvocationReorderNV: GLSL GL_NV_shader_invocation_reorder, Vulkan
	// VK_NV_ray_tracing_invocation_reorder.
	REORDER_USAGE_NV = 1 << 1,
};

inline constexpr const char *ERROR_NO_LIBRARIES = "This device cannot create or link raytracing pipeline libraries (SUPPORTS_RAYTRACING_PIPELINE_LIBRARY).";
inline constexpr const char *ERROR_NO_REORDER = "RAYTRACING_PIPELINE_FLAG_REORDER needs SUPPORTS_RAYTRACING_REORDER, which this device does not support.";
inline constexpr const char *ERROR_LIBRARY_NOT_BINDABLE = "A raytracing pipeline library cannot be bound or given a hit shader binding table. Link it with raytracing_pipeline_link() first.";
inline constexpr const char *ERROR_NO_INDIRECT = "This device cannot trace rays with indirect arguments (SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT).";

// The stride of the hit shader binding table: the handle, then the record, aligned to the handle
// alignment. Without records it is the aligned handle size, as before records existed.
uint32_t get_hit_group_stride(uint32_t p_handle_size, uint32_t p_handle_alignment, uint32_t p_record_size);

// Writes the records of p_count table entries into r_table, whose entries are p_stride bytes apart and
// already hold their handles in the first p_handle_size bytes. Entry i gets the first p_record_size bytes
// of hit group p_hit_group_indices[i]'s record in p_records, which holds MAX_HIT_GROUP_RECORD_SIZE bytes
// for each of p_hit_group_count hit groups; an index past them gets a zero record. The rest of each entry
// is zeroed.
void write_hit_group_records(uint8_t *r_table, uint32_t p_stride, uint32_t p_handle_size, uint32_t p_record_size, const uint32_t *p_hit_group_indices, uint32_t p_count, const uint8_t *p_records, uint32_t p_hit_group_count);

String get_record_too_large_error(uint32_t p_hit_group_index, uint32_t p_size);

// Scans one SPIR-V module's capabilities for the ReorderUsage bits. 0 for anything that is not a
// little-endian SPIR-V module.
uint32_t get_spirv_reorder_usage(const uint8_t *p_spirv, uint64_t p_size);
// The error shader creation fails with when the device lacks p_missing_usage. It names the shader (when
// it has a name) and the missing extension.
String get_missing_reorder_error(const String &p_shader_name, uint32_t p_missing_usage);

// The largest ray payload one SPIR-V module declares, in bytes: its RayPayloadKHR and
// IncomingRayPayloadKHR variables, measured as Vulkan measures maxPipelineRayPayloadSize (every scalar
// aligned to the largest scalar of the variable). 0 without a payload or for anything that is not a
// little-endian SPIR-V module; PAYLOAD_SIZE_UNKNOWN when a payload holds an array sized by a
// specialization constant, a runtime array or a type without a size.
uint32_t get_spirv_payload_size(const uint8_t *p_spirv, uint64_t p_size);
// The error a library fails with when the payload of p_shader_name's p_stage (p_size bytes, or
// PAYLOAD_SIZE_UNKNOWN) does not fit in LIBRARY_MAX_PAYLOAD_SIZE.
String get_library_payload_error(const String &p_shader_name, RDD::ShaderStage p_stage, uint32_t p_size);

// What raytracing_pipeline_link() checks about the base and each library.
struct LinkPart {
	bool library = false; // Created with RAYTRACING_PIPELINE_FLAG_LIBRARY.
	bool linked = false; // Returned by raytracing_pipeline_link().
	uint32_t raygen_count = 0;
	uint32_t miss_count = 0;
	uint32_t max_trace_recursion_depth = 0;
	uint32_t push_constant_size = 0;
	Vector<uint32_t> set_formats;
	// The immutable samplers of the layout, which set formats leave out (RenderingDevice's
	// Shader::immutable_sampler_hash of the layout-defining shader).
	uint32_t immutable_sampler_hash = 0;
};

enum LinkStatus {
	LINK_OK,
	LINK_BASE_NOT_LIBRARY,
	LINK_BASE_NO_RAYGEN,
	LINK_LIBRARY_NOT_LIBRARY,
	LINK_LIBRARY_HAS_RAYGEN_OR_MISS,
	LINK_RECURSION_MISMATCH,
	LINK_LAYOUT_MISMATCH,
	LINK_DUPLICATE_LIBRARY,
};

// The base is a library with at least one ray generation shader, or a pipeline that
// raytracing_pipeline_link() returned.
LinkStatus get_link_base_status(const LinkPart &p_base);
// A library adds hit groups only, with the base's recursion depth and the base's layout: the same set
// formats, immutable samplers and push constant size.
LinkStatus get_link_library_status(const LinkPart &p_base, const LinkPart &p_library);
const char *get_link_error(LinkStatus p_status);

enum IndirectStatus {
	INDIRECT_OK,
	INDIRECT_NO_INDIRECT_USAGE,
	INDIRECT_NO_DEVICE_ADDRESS,
	INDIRECT_MISALIGNED,
	INDIRECT_OUT_OF_RANGE,
};

// The buffer of an indirect trace is a storage buffer created with STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT
// and BUFFER_CREATION_DEVICE_ADDRESS_BIT (Vulkan reads the command through its address), and holds
// INDIRECT_COMMAND_SIZE bytes at a 4-byte aligned p_offset.
IndirectStatus get_indirect_status(BitField<RDD::BufferUsageBits> p_usage, uint64_t p_offset, uint64_t p_buffer_size);
const char *get_indirect_error(IndirectStatus p_status);

} // namespace RDRTPipeline
