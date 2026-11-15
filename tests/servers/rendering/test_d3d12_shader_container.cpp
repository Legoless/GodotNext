/**************************************************************************/
/*  test_d3d12_shader_container.cpp                                       */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_d3d12_shader_container)

// GodotNext: d3d12_gpu_based_validation (#272). D3D12 GPU-based validation makes CreateRootSignature
// commit memory in proportion to the highest root constant or root descriptor register, so the
// D3D12 shader container keeps those at small registers in register spaces of their own, and the
// DXIL must declare every resource where the root signature binds it. No case needs a GPU: the
// container compiles through glslang and Mesa, and d3d12.dll only (de)serializes root signatures.

#include "modules/modules_enabled.gen.h"

#if defined(D3D12_ENABLED) && defined(MODULE_GLSLANG_ENABLED)

#include "core/templates/local_vector.h"
#include "drivers/d3d12/rendering_shader_container_d3d12.h"
#include "servers/rendering/renderer_rd/shader_rd.h"

#include "modules/glslang/shader_compile.h"

#include <drivers/d3d12/godot_d3dx12.h>
#include <wrl/client.h>

#if !defined(_MSC_VER)
#include <thirdparty/directx_headers/include/dxguids/dxguids.h>

#include <guiddef.h>
#endif

#if NIR_ENABLED

namespace TestD3D12ShaderContainer {

using Microsoft::WRL::ComPtr;
using RDC = RenderingDeviceCommons;
using ContainerD3D12 = RenderingShaderContainerD3D12;

// Root constants and root descriptors must stay this small (they are 0 to a few dozen today).
constexpr uint32_t SMALL_REGISTER_LIMIT = 64;

struct StageSource {
	RDC::ShaderStage stage = RDC::SHADER_STAGE_MAX;
	String code;
};

// A DXIL resource from the container's PSV0 part (Mesa's dxil_resource_v0).
struct DxilResource {
	uint32_t type = 0; // 1 sampler, 2 CBV, 3-5 SRV, 6-9 UAV.
	uint32_t space = 0;
	uint32_t lower_bound = 0;
	uint32_t upper_bound = 0;
};

uint32_t read_u32(const uint8_t *p_bytes) {
	return uint32_t(p_bytes[0]) | (uint32_t(p_bytes[1]) << 8) | (uint32_t(p_bytes[2]) << 16) | (uint32_t(p_bytes[3]) << 24);
}

// The DXIL container: "DXBC", a 16-byte digest, the version, the total size, the part count and the
// part offsets; each part is a four-character code, a size and the data. PSV0 holds the runtime
// info size and runtime info, then the resource count, the size of one record and the records.
bool read_psv0_resources(const Vector<uint8_t> &p_dxil, LocalVector<DxilResource> &r_resources) {
	if (p_dxil.size() < 32 || read_u32(p_dxil.ptr()) != read_u32((const uint8_t *)"DXBC")) {
		return false;
	}
	const uint32_t part_count = read_u32(p_dxil.ptr() + 28);
	for (uint32_t i = 0; i < part_count; i++) {
		const uint32_t offset = read_u32(p_dxil.ptr() + 32 + i * 4);
		if (offset + 8 > uint32_t(p_dxil.size()) || read_u32(p_dxil.ptr() + offset) != read_u32((const uint8_t *)"PSV0")) {
			continue;
		}
		const uint8_t *psv = p_dxil.ptr() + offset + 8;
		const uint32_t runtime_info_size = read_u32(psv);
		const uint8_t *cursor = psv + 4 + runtime_info_size;
		const uint32_t resource_count = read_u32(cursor);
		cursor += 4;
		if (resource_count == 0) {
			return true;
		}
		const uint32_t record_size = read_u32(cursor);
		cursor += 4;
		for (uint32_t j = 0; j < resource_count; j++) {
			const uint8_t *record = cursor + j * record_size;
			r_resources.push_back({ read_u32(record), read_u32(record + 4), read_u32(record + 8), read_u32(record + 12) });
		}
		return true;
	}
	return false;
}

D3D12_DESCRIPTOR_RANGE_TYPE range_type_for_dxil(uint32_t p_type) {
	if (p_type == 1) {
		return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	} else if (p_type == 2) {
		return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
	} else if (p_type <= 5) {
		return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	}
	return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
}

// Whether a root parameter binds the DXIL resource: a root constant or descriptor of its class at
// its exact register and space, or a descriptor table range of its class that covers it.
bool root_signature_binds(const D3D12_ROOT_SIGNATURE_DESC1 &p_desc, const DxilResource &p_resource) {
	const D3D12_DESCRIPTOR_RANGE_TYPE range_type = range_type_for_dxil(p_resource.type);
	for (uint32_t i = 0; i < p_desc.NumParameters; i++) {
		const D3D12_ROOT_PARAMETER1 &param = p_desc.pParameters[i];
		switch (param.ParameterType) {
			case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
				if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV && param.Constants.ShaderRegister == p_resource.lower_bound && param.Constants.RegisterSpace == p_resource.space) {
					return true;
				}
			} break;
			case D3D12_ROOT_PARAMETER_TYPE_CBV:
			case D3D12_ROOT_PARAMETER_TYPE_SRV:
			case D3D12_ROOT_PARAMETER_TYPE_UAV: {
				const D3D12_DESCRIPTOR_RANGE_TYPE root_type = param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV : (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV);
				if (root_type == range_type && param.Descriptor.ShaderRegister == p_resource.lower_bound && param.Descriptor.RegisterSpace == p_resource.space) {
					return true;
				}
			} break;
			case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
				for (uint32_t j = 0; j < param.DescriptorTable.NumDescriptorRanges; j++) {
					const D3D12_DESCRIPTOR_RANGE1 &range = param.DescriptorTable.pDescriptorRanges[j];
					if (range.RangeType == range_type && range.RegisterSpace == p_resource.space && range.BaseShaderRegister <= p_resource.lower_bound && p_resource.upper_bound < range.BaseShaderRegister + range.NumDescriptors) {
						return true;
					}
				}
			} break;
			default: {
			} break;
		}
	}
	return false;
}

class ShaderContainerFixture {
	HMODULE lib_d3d12 = nullptr;

public:
	Ref<RenderingShaderContainer> container;
	ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
	const D3D12_ROOT_SIGNATURE_DESC1 *root_signature = nullptr;
	ContainerD3D12::ShaderReflectionD3D12 reflection_d3d12;
	LocalVector<LocalVector<DxilResource>> stage_resources;

	// Compiles the stages (dynamic buffers given as ShaderRD::DynamicBuffer keys) into a D3D12
	// container, and reads back its root signature and each stage's DXIL resources.
	ShaderContainerFixture(const Vector<StageSource> &p_stages, const Vector<uint64_t> &p_dynamic_buffers) {
		lib_d3d12 = LoadLibraryW(L"D3D12.dll");
		REQUIRE(lib_d3d12 != nullptr);
		{
			RenderingShaderContainerFormatD3D12 format;
			format.set_lib_d3d12(lib_d3d12);
			Vector<RDC::ShaderStageSPIRVData> spirv_stages;
			for (const StageSource &source : p_stages) {
				String error;
				RDC::ShaderStageSPIRVData spirv_stage;
				spirv_stage.shader_stage = source.stage;
				spirv_stage.spirv = compile_glslang_shader(source.stage, source.code, format.get_shader_language_version(), format.get_shader_spirv_version(source.stage), &error);
				spirv_stage.dynamic_buffers = p_dynamic_buffers;
				REQUIRE_MESSAGE(!spirv_stage.spirv.is_empty(), ("glslang failed: " + error).utf8().get_data());
				spirv_stages.push_back(spirv_stage);
			}
			container = format.create_container();
			REQUIRE(container->set_code_from_spirv("test_d3d12_shader_container", spirv_stages));
		}
		ContainerD3D12 *container_d3d12 = static_cast<ContainerD3D12 *>(container.ptr());
		reflection_d3d12 = container_d3d12->get_shader_reflection_d3d12();

		PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER create_deserializer = (PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER)(void *)GetProcAddress(lib_d3d12, "D3D12CreateVersionedRootSignatureDeserializer");
		REQUIRE(create_deserializer != nullptr);
		REQUIRE(SUCCEEDED(create_deserializer(reflection_d3d12.root_signature_bytes.ptr(), reflection_d3d12.root_signature_bytes.size(), IID_PPV_ARGS(deserializer.GetAddressOf()))));
		const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *versioned = nullptr;
		REQUIRE(SUCCEEDED(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &versioned)));
		root_signature = &versioned->Desc_1_1;

		for (const RenderingShaderContainer::Shader &shader : container->shaders) {
			Vector<uint8_t> dxil;
			dxil.resize(shader.code_decompressed_size);
			REQUIRE(container->decompress_code(shader.code_compressed_bytes.ptr(), shader.code_compressed_bytes.size(), shader.code_compression_flags, dxil.ptrw(), dxil.size()));
			LocalVector<DxilResource> resources;
			REQUIRE(read_psv0_resources(dxil, resources));
			stage_resources.push_back(resources);
		}
	}

	~ShaderContainerFixture() {
		deserializer.Reset();
		container.unref();
		FreeLibrary(lib_d3d12);
	}

	const D3D12_ROOT_PARAMETER1 &param(uint32_t p_index) const {
		REQUIRE(p_index < root_signature->NumParameters);
		return root_signature->pParameters[p_index];
	}

	// The D3D12 reflection of the uniform at (set, binding).
	const ContainerD3D12::ReflectionBindingDataD3D12 &uniform_d3d12(uint32_t p_set, uint32_t p_binding) const {
		const RDC::ShaderReflection reflection = container->get_shader_reflection();
		REQUIRE(p_set < uint32_t(reflection.uniform_sets.size()));
		const Vector<RDC::ShaderUniform> &uniforms = reflection.uniform_sets[p_set];
		int index = 0;
		while (index < uniforms.size() && uniforms[index].binding != p_binding) {
			index++;
		}
		REQUIRE(index < uniforms.size());
		return reflection_d3d12.reflection_binding_set_uniforms_d3d12[p_set][index];
	}

	// Every root constant and root descriptor sits at a small register outside space 0, and every
	// descriptor table range stays in space 0 at Mesa's set/binding register.
	void check_root_parameter_registers() const {
		for (uint32_t i = 0; i < root_signature->NumParameters; i++) {
			const D3D12_ROOT_PARAMETER1 &root_param = root_signature->pParameters[i];
			if (root_param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
				CHECK_LT(root_param.Constants.ShaderRegister, SMALL_REGISTER_LIMIT);
				CHECK_EQ(root_param.Constants.RegisterSpace, ContainerD3D12::ROOT_CONSTANT_SPACE);
			} else if (root_param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
				for (uint32_t j = 0; j < root_param.DescriptorTable.NumDescriptorRanges; j++) {
					const D3D12_DESCRIPTOR_RANGE1 &range = root_param.DescriptorTable.pDescriptorRanges[j];
					CHECK_EQ(range.RegisterSpace, 0u);
					CHECK_EQ(range.BaseShaderRegister % GODOT_NIR_BINDING_MULTIPLIER, 0u);
				}
			} else {
				CHECK_LT(root_param.Descriptor.ShaderRegister, SMALL_REGISTER_LIMIT);
				CHECK_GE(root_param.Descriptor.RegisterSpace, ContainerD3D12::DYNAMIC_BUFFER_SPACE_BASE);
				CHECK_LT(root_param.Descriptor.RegisterSpace, ContainerD3D12::DYNAMIC_BUFFER_SPACE_BASE + RDC::MAX_UNIFORM_SETS);
			}
		}
	}

	// Every resource each stage's DXIL declares is bound by a root parameter of its class.
	void check_dxil_resources_bound() const {
		for (const LocalVector<DxilResource> &resources : stage_resources) {
			CHECK_FALSE(resources.is_empty());
			for (const DxilResource &resource : resources) {
				INFO("DXIL resource type ", resource.type, ", space ", resource.space, ", registers ", resource.lower_bound, "-", resource.upper_bound);
				CHECK(root_signature_binds(*root_signature, resource));
			}
		}
	}

	bool dxil_declares(uint32_t p_type, uint32_t p_space, uint32_t p_register) const {
		for (const LocalVector<DxilResource> &resources : stage_resources) {
			for (const DxilResource &resource : resources) {
				if (resource.type == p_type && resource.space == p_space && resource.lower_bound == p_register) {
					return true;
				}
			}
		}
		return false;
	}
};

const char *RASTER_VERTEX = R"(#version 450
layout(set = 0, binding = 0, std140) uniform SceneData { mat4 projection; } scene_data;
layout(set = 1, binding = 2, std430) readonly buffer Instances { vec4 offsets[]; } instances;
layout(push_constant, std430) uniform Params { uint base_instance; float scale; } params;
layout(location = 0) in vec3 vertex;
void main() {
	gl_Position = scene_data.projection * vec4(vertex * params.scale + instances.offsets[params.base_instance].xyz, 1.0);
}
)";

const char *RASTER_FRAGMENT = R"(#version 450
layout(set = 1, binding = 0, std140) uniform Material { vec4 albedo; } material;
layout(set = 2, binding = 0) uniform texture2D albedo_texture;
layout(set = 2, binding = 1) uniform sampler albedo_sampler;
layout(push_constant, std430) uniform Params { uint base_instance; float scale; } params;
layout(location = 0) out vec4 frag_color;
void main() {
	frag_color = material.albedo * texture(sampler2D(albedo_texture, albedo_sampler), vec2(0.5)) * params.scale;
}
)";

const char *COMPUTE = R"(#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 1, std430) buffer Output { uint values[]; } output_buffer;
layout(set = 1, binding = 4, std430) buffer Counters { uint values[]; } counters;
layout(push_constant, std430) uniform Params { uint offset; uint pad0; uint pad1; uint pad2; } params;
void main() {
	uint index = gl_GlobalInvocationID.x;
	output_buffer.values[index] = index + gl_NumWorkGroups.x + params.offset;
	atomicAdd(counters.values[0], 1u);
}
)";

TEST_CASE("[D3D12][ShaderContainer] Raster root constants and dynamic buffers use small registers outside space 0") {
	Vector<StageSource> stages;
	stages.push_back({ RDC::SHADER_STAGE_VERTEX, RASTER_VERTEX });
	stages.push_back({ RDC::SHADER_STAGE_FRAGMENT, RASTER_FRAGMENT });
	Vector<uint64_t> dynamic_buffers;
	dynamic_buffers.push_back(ShaderRD::DynamicBuffer::encode(1, 0)); // Material: root CBV.
	dynamic_buffers.push_back(ShaderRD::DynamicBuffer::encode(1, 2)); // Instances: root SRV.
	ShaderContainerFixture fixture(stages, dynamic_buffers);

	// Push constants come first, in both stages.
	const D3D12_ROOT_PARAMETER1 &push_constants = fixture.param(0);
	REQUIRE_EQ(push_constants.ParameterType, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
	CHECK_EQ(push_constants.Constants.ShaderRegister, ContainerD3D12::ROOT_CONSTANT_REGISTER);
	CHECK_EQ(push_constants.Constants.RegisterSpace, ContainerD3D12::ROOT_CONSTANT_SPACE);
	CHECK_EQ(push_constants.Constants.Num32BitValues, 2u);
	CHECK_EQ(fixture.reflection_d3d12.dxil_push_constant_stages, (1u << RDC::SHADER_STAGE_VERTEX) | (1u << RDC::SHADER_STAGE_FRAGMENT));

	const uint32_t dynamic_space = ContainerD3D12::DYNAMIC_BUFFER_SPACE_BASE + 1;
	const ContainerD3D12::ReflectionBindingDataD3D12 &material = fixture.uniform_d3d12(1, 0);
	const D3D12_ROOT_PARAMETER1 &material_param = fixture.param(material.root_param_idx);
	CHECK_EQ(material_param.ParameterType, D3D12_ROOT_PARAMETER_TYPE_CBV);
	CHECK_EQ(material_param.Descriptor.ShaderRegister, 0u);
	CHECK_EQ(material_param.Descriptor.RegisterSpace, dynamic_space);
	CHECK_EQ(material.dxil_stages, 1u << RDC::SHADER_STAGE_FRAGMENT);

	const ContainerD3D12::ReflectionBindingDataD3D12 &instances = fixture.uniform_d3d12(1, 2);
	const D3D12_ROOT_PARAMETER1 &instances_param = fixture.param(instances.root_param_idx);
	CHECK_EQ(instances_param.ParameterType, D3D12_ROOT_PARAMETER_TYPE_SRV);
	CHECK_EQ(instances_param.Descriptor.ShaderRegister, 2u);
	CHECK_EQ(instances_param.Descriptor.RegisterSpace, dynamic_space);
	CHECK_EQ(instances.dxil_stages, 1u << RDC::SHADER_STAGE_VERTEX);

	fixture.check_root_parameter_registers();
	fixture.check_dxil_resources_bound();

	// The DXIL moved with the root signature: the dynamic buffers are declared at their new
	// location, and nothing is left at Mesa's register for them.
	CHECK(fixture.dxil_declares(2, dynamic_space, 0)); // CBV.
	CHECK(fixture.dxil_declares(4, dynamic_space, 2)); // Raw SRV.
	CHECK_FALSE(fixture.dxil_declares(2, 0, 1 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER + 0 * GODOT_NIR_BINDING_MULTIPLIER));
	CHECK_FALSE(fixture.dxil_declares(4, 0, 1 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER + 2 * GODOT_NIR_BINDING_MULTIPLIER));
	// The plain resources keep Mesa's registers in space 0.
	CHECK(fixture.dxil_declares(2, 0, 0)); // SceneData, set 0 binding 0.
	CHECK(fixture.dxil_declares(3, 0, 2 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER)); // albedo_texture.
	CHECK(fixture.dxil_declares(1, 0, 2 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER + GODOT_NIR_BINDING_MULTIPLIER)); // albedo_sampler.
}

TEST_CASE("[D3D12][ShaderContainer] Compute runtime data and dynamic UAVs use small registers outside space 0") {
	Vector<StageSource> stages;
	stages.push_back({ RDC::SHADER_STAGE_COMPUTE, COMPUTE });
	Vector<uint64_t> dynamic_buffers;
	dynamic_buffers.push_back(ShaderRD::DynamicBuffer::encode(1, 4)); // Counters: root UAV.
	ShaderContainerFixture fixture(stages, dynamic_buffers);

	const D3D12_ROOT_PARAMETER1 &push_constants = fixture.param(0);
	REQUIRE_EQ(push_constants.ParameterType, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
	CHECK_EQ(push_constants.Constants.ShaderRegister, ContainerD3D12::ROOT_CONSTANT_REGISTER);
	CHECK_EQ(push_constants.Constants.RegisterSpace, ContainerD3D12::ROOT_CONSTANT_SPACE);
	CHECK_EQ(push_constants.Constants.Num32BitValues, 4u);

	// gl_NumWorkGroups reads NIR-DXIL's runtime data.
	REQUIRE_EQ(fixture.reflection_d3d12.nir_runtime_data_root_param_idx, 1u);
	const D3D12_ROOT_PARAMETER1 &runtime_data = fixture.param(1);
	REQUIRE_EQ(runtime_data.ParameterType, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
	CHECK_EQ(runtime_data.Constants.ShaderRegister, ContainerD3D12::RUNTIME_DATA_REGISTER);
	CHECK_EQ(runtime_data.Constants.RegisterSpace, ContainerD3D12::ROOT_CONSTANT_SPACE);

	const uint32_t dynamic_space = ContainerD3D12::DYNAMIC_BUFFER_SPACE_BASE + 1;
	const ContainerD3D12::ReflectionBindingDataD3D12 &counters = fixture.uniform_d3d12(1, 4);
	const D3D12_ROOT_PARAMETER1 &counters_param = fixture.param(counters.root_param_idx);
	CHECK_EQ(counters_param.ParameterType, D3D12_ROOT_PARAMETER_TYPE_UAV);
	CHECK_EQ(counters_param.Descriptor.ShaderRegister, 4u);
	CHECK_EQ(counters_param.Descriptor.RegisterSpace, dynamic_space);

	fixture.check_root_parameter_registers();
	fixture.check_dxil_resources_bound();

	CHECK(fixture.dxil_declares(2, ContainerD3D12::ROOT_CONSTANT_SPACE, ContainerD3D12::ROOT_CONSTANT_REGISTER)); // Push constants.
	CHECK(fixture.dxil_declares(2, ContainerD3D12::ROOT_CONSTANT_SPACE, ContainerD3D12::RUNTIME_DATA_REGISTER)); // Runtime data.
	CHECK(fixture.dxil_declares(7, dynamic_space, 4)); // Raw UAV.
	CHECK_FALSE(fixture.dxil_declares(7, 0, 1 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER + 4 * GODOT_NIR_BINDING_MULTIPLIER));
	CHECK(fixture.dxil_declares(7, 0, GODOT_NIR_BINDING_MULTIPLIER)); // Output, set 0 binding 1.
}

} // namespace TestD3D12ShaderContainer

#endif // NIR_ENABLED

#endif // D3D12_ENABLED && MODULE_GLSLANG_ENABLED
