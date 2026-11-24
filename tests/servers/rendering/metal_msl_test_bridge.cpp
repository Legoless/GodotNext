/**************************************************************************/
/*  metal_msl_test_bridge.cpp                                             */
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

#include "metal_msl_test_bridge.h"

#include <spirv.hpp>
#include <spirv_msl.hpp>
#include <spirv_parser.hpp>

#include <exception>

namespace MetalMSLTestBridge {

static spv::ExecutionModel _map_stage(RenderingDeviceCommons::ShaderStage p_stage) {
	switch (p_stage) {
		case RenderingDeviceCommons::SHADER_STAGE_VERTEX:
			return spv::ExecutionModelVertex;
		case RenderingDeviceCommons::SHADER_STAGE_FRAGMENT:
			return spv::ExecutionModelFragment;
		case RenderingDeviceCommons::SHADER_STAGE_COMPUTE:
			return spv::ExecutionModelGLCompute;
		case RenderingDeviceCommons::SHADER_STAGE_RAYGEN:
			return spv::ExecutionModelRayGenerationKHR;
		case RenderingDeviceCommons::SHADER_STAGE_ANY_HIT:
			return spv::ExecutionModelAnyHitKHR;
		case RenderingDeviceCommons::SHADER_STAGE_CLOSEST_HIT:
			return spv::ExecutionModelClosestHitKHR;
		case RenderingDeviceCommons::SHADER_STAGE_MISS:
			return spv::ExecutionModelMissKHR;
		case RenderingDeviceCommons::SHADER_STAGE_INTERSECTION:
			return spv::ExecutionModelIntersectionKHR;
		default:
			return spv::ExecutionModelMax;
	}
}

static spirv_cross::SPIRType::BaseType _map_basetype(BaseType p_type) {
	switch (p_type) {
		case BASE_TYPE_IMAGE:
			return spirv_cross::SPIRType::Image;
		case BASE_TYPE_SAMPLER:
			return spirv_cross::SPIRType::Sampler;
		case BASE_TYPE_SAMPLED_IMAGE:
			return spirv_cross::SPIRType::SampledImage;
		case BASE_TYPE_ACCELERATION_STRUCTURE:
			return spirv_cross::SPIRType::AccelerationStructure;
		case BASE_TYPE_VOID:
		default:
			return spirv_cross::SPIRType::Void;
	}
}

bool compile_spirv_to_msl(
		const uint32_t *p_spirv_words,
		size_t p_word_count,
		RenderingDeviceCommons::ShaderStage p_stage,
		const CompileOptions &p_options,
		const Vector<ResourceBinding> &p_bindings,
		String &r_msl_source,
		String &r_error) {
	r_msl_source = String();
	r_error = String();

	try {
		spirv_cross::Parser parser(p_spirv_words, p_word_count);
		parser.parse();

		spirv_cross::CompilerMSL compiler(std::move(parser.get_parsed_ir()));
		spirv_cross::CompilerMSL::Options msl_options{};
		msl_options.platform = spirv_cross::CompilerMSL::Options::macOS;
		msl_options.set_msl_version(p_options.msl_major, p_options.msl_minor);
		msl_options.argument_buffers = p_options.argument_buffers;
		msl_options.argument_buffers_tier = p_options.argument_buffers
				? spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2
				: spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier1;
		msl_options.force_active_argument_buffer_resources = true;
		msl_options.pad_argument_buffer_resources = true;
		msl_options.texture_buffer_native = true;
		msl_options.rt_visible_function_table_index = p_options.rt_visible_function_table_index;
		msl_options.rt_intersection_function_table_index = p_options.rt_intersection_function_table_index;
		msl_options.rt_sbt_params_buffer_index = p_options.rt_sbt_params_buffer_index;
		if (p_options.msl_major > 3 || (p_options.msl_major == 3 && p_options.msl_minor >= 2)) {
			msl_options.readwrite_texture_fences = false;
		}
		compiler.set_msl_options(msl_options);

		for (uint32_t set_idx = 0; set_idx < 8; set_idx++) {
			if (p_options.device_address_space_sets & (1u << set_idx)) {
				compiler.set_argument_buffer_device_address_space(set_idx, true);
			}
		}

		const spv::ExecutionModel model = _map_stage(p_stage);
		for (const ResourceBinding &b : p_bindings) {
			spirv_cross::MSLResourceBinding rb{};
			rb.stage = model;
			rb.basetype = _map_basetype(b.basetype);
			rb.desc_set = b.desc_set;
			rb.binding = b.binding;
			rb.count = b.count;
			rb.msl_buffer = b.msl_buffer;
			rb.msl_texture = b.msl_texture;
			rb.msl_sampler = b.msl_sampler;
			compiler.add_msl_resource_binding(rb);
		}

		if (p_options.has_push_constants) {
			spirv_cross::MSLResourceBinding pc{};
			pc.stage = model;
			pc.desc_set = spirv_cross::ResourceBindingPushConstantDescriptorSet;
			pc.binding = spirv_cross::ResourceBindingPushConstantBinding;
			pc.basetype = spirv_cross::SPIRType::Void;
			pc.msl_buffer = p_options.push_constant_buffer_index;
			compiler.add_msl_resource_binding(pc);
		}

		const std::string source = compiler.compile();
		r_msl_source = String::utf8(source.c_str(), (int)source.size());
		return true;
	} catch (const spirv_cross::CompilerError &e) {
		r_error = String::utf8(e.what());
		return false;
	} catch (const std::exception &e) {
		r_error = String::utf8(e.what());
		return false;
	}
}

} // namespace MetalMSLTestBridge
