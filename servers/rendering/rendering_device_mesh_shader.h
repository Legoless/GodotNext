/**************************************************************************/
/*  rendering_device_mesh_shader.h                                        */
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
#include "servers/rendering/rendering_device_driver.h"

// GodotNext: nextgeo_mesh_raster (#154, plan §21.1 C2, §21.4 N9).
// Inspects task and mesh shader SPIR-V for the shader-side mesh limits left by #178:
// workgroup local size and total invocations, mesh output vertex and primitive caps,
// and task payload byte size. RenderingDevice checks every task and mesh shader against
// the device's MeshShaderLimits and rejects violations with one named error.
namespace RDMeshShader {

// Returned for task_payload_size when a TaskPayloadWorkgroupEXT variable's size cannot be
// determined statically (for example, an array sized by a specialization constant).
static constexpr uint64_t PAYLOAD_SIZE_UNKNOWN = UINT64_MAX;

struct StageProperties {
	bool valid_spirv = false;
	uint32_t local_size[3] = { 1, 1, 1 };
	uint32_t output_vertices = 0;
	uint32_t output_primitives = 0;
	uint64_t task_payload_size = 0;
};

// Scans one little-endian SPIR-V module for task/mesh execution modes and task payload variables.
// Returns valid_spirv = false for anything that is not a valid little-endian SPIR-V module.
StageProperties inspect_spirv(const uint8_t *p_spirv, uint64_t p_size);

// Validates every SHADER_STAGE_TASK and SHADER_STAGE_MESH stage in p_spirv against p_limits.
// Returns true when within limits; otherwise returns false and writes one named error to r_error.
bool validate_shader(const String &p_shader_name, Span<RenderingDeviceCommons::ShaderStageSPIRVData> p_spirv, const RenderingDeviceDriver::MeshShaderLimits &p_limits, String &r_error);

// Validates the reflected task and mesh workgroup sizes of a parsed shader container against p_limits.
bool validate_reflection_local_sizes(const String &p_shader_name, const RenderingDeviceCommons::ShaderReflection &p_reflection, const RenderingDeviceDriver::MeshShaderLimits &p_limits, String &r_error);

} // namespace RDMeshShader
