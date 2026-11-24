/**************************************************************************/
/*  metal_msl_test_bridge.h                                               */
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
#include "core/templates/vector.h"
#include "servers/rendering/rendering_device_commons.h"

namespace MetalMSLTestBridge {

enum BaseType : uint32_t {
	BASE_TYPE_VOID = 0,
	BASE_TYPE_IMAGE = 1,
	BASE_TYPE_SAMPLER = 2,
	BASE_TYPE_SAMPLED_IMAGE = 3,
	BASE_TYPE_ACCELERATION_STRUCTURE = 4,
};

struct ResourceBinding {
	RenderingDeviceCommons::ShaderStage stage = RenderingDeviceCommons::SHADER_STAGE_RAYGEN;
	BaseType basetype = BASE_TYPE_VOID;
	uint32_t desc_set = 0;
	uint32_t binding = 0;
	uint32_t count = 1;
	uint32_t msl_buffer = 0;
	uint32_t msl_texture = 0;
	uint32_t msl_sampler = 0;
};

struct CompileOptions {
	uint32_t msl_major = 3;
	uint32_t msl_minor = 2;
	bool argument_buffers = true;
	uint32_t device_address_space_sets = 0;
	uint32_t rt_visible_function_table_index = 16;
	uint32_t rt_intersection_function_table_index = 17;
	uint32_t rt_sbt_params_buffer_index = 18;
	bool has_push_constants = false;
	uint32_t push_constant_buffer_index = 0;
};

bool compile_spirv_to_msl(
		const uint32_t *p_spirv_words,
		size_t p_word_count,
		RenderingDeviceCommons::ShaderStage p_stage,
		const CompileOptions &p_options,
		const Vector<ResourceBinding> &p_bindings,
		String &r_msl_source,
		String &r_error);

} // namespace MetalMSLTestBridge
