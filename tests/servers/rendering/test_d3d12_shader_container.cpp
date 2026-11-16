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
// GodotNext: dxil_toolchain (#123). The same layout holds for ray query and for the ray-tracing
// stages, which compile only with the GodotNext Mesa NIR library; each stage is its own DXIL library.

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

// GodotNext: dxil_toolchain (#123). A DXIL library has no PSV0 part: its resources are in the RDAT
// (runtime data) part. RDAT holds its version, its part count and one offset per part from the start
// of RDAT; each part is a type, a size and the data. The resource table (type 3) holds the record
// count, the size of one record and the records: class (0 SRV, 1 UAV, 2 CBV, 3 sampler), kind, id,
// space, lower bound, upper bound. The class and kind give the type PSV0 would give: a raw buffer
// (kind 11) and an acceleration structure (kind 16) are raw, a structured buffer (kind 12) is
// structured, and the rest are typed.
bool read_rdat_resources(const Vector<uint8_t> &p_dxil, LocalVector<DxilResource> &r_resources) {
	if (p_dxil.size() < 32 || read_u32(p_dxil.ptr()) != read_u32((const uint8_t *)"DXBC")) {
		return false;
	}
	const uint32_t part_count = read_u32(p_dxil.ptr() + 28);
	for (uint32_t i = 0; i < part_count; i++) {
		const uint32_t offset = read_u32(p_dxil.ptr() + 32 + i * 4);
		if (offset + 16 > uint32_t(p_dxil.size()) || read_u32(p_dxil.ptr() + offset) != read_u32((const uint8_t *)"RDAT")) {
			continue;
		}
		const uint32_t rdat_size = read_u32(p_dxil.ptr() + offset + 4);
		const uint8_t *rdat = p_dxil.ptr() + offset + 8;
		const uint32_t rdat_part_count = read_u32(rdat + 4);
		if (offset + 8 + uint64_t(rdat_size) > uint64_t(p_dxil.size()) || 8 + uint64_t(rdat_part_count) * 4 > rdat_size) {
			return false;
		}
		for (uint32_t j = 0; j < rdat_part_count; j++) {
			const uint32_t part_offset = read_u32(rdat + 8 + j * 4);
			if (part_offset + 16 > rdat_size || read_u32(rdat + part_offset) != 3) {
				continue;
			}
			const uint32_t table_size = read_u32(rdat + part_offset + 4);
			const uint8_t *table = rdat + part_offset + 8;
			const uint32_t record_count = read_u32(table);
			const uint32_t record_size = read_u32(table + 4);
			if (record_size < 24 || part_offset + 8 + uint64_t(table_size) > rdat_size || 8 + uint64_t(record_count) * record_size > table_size) {
				return false;
			}
			for (uint32_t k = 0; k < record_count; k++) {
				const uint8_t *record = table + 8 + k * record_size;
				const uint32_t resource_class = read_u32(record);
				const uint32_t kind = read_u32(record + 4);
				uint32_t type = 1;
				if (resource_class == 0) {
					type = (kind == 11 || kind == 16) ? 4 : (kind == 12 ? 5 : 3);
				} else if (resource_class == 1) {
					type = kind == 11 ? 7 : (kind == 12 ? 8 : 6);
				} else if (resource_class == 2) {
					type = 2;
				}
				r_resources.push_back({ type, read_u32(record + 12), read_u32(record + 16), read_u32(record + 20) });
			}
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

// Compiles each stage with glslang for the D3D12 container format, with the dynamic buffers given as
// ShaderRD::DynamicBuffer keys.
Vector<RDC::ShaderStageSPIRVData> compile_stages(const RenderingShaderContainerFormat &p_format, const Vector<StageSource> &p_stages, const Vector<uint64_t> &p_dynamic_buffers) {
	Vector<RDC::ShaderStageSPIRVData> spirv_stages;
	for (const StageSource &source : p_stages) {
		String error;
		RDC::ShaderStageSPIRVData spirv_stage;
		spirv_stage.shader_stage = source.stage;
		spirv_stage.spirv = compile_glslang_shader(source.stage, source.code, p_format.get_shader_language_version(), p_format.get_shader_spirv_version(source.stage), &error);
		spirv_stage.dynamic_buffers = p_dynamic_buffers;
		REQUIRE_MESSAGE(!spirv_stage.spirv.is_empty(), ("glslang failed: " + error).utf8().get_data());
		spirv_stages.push_back(spirv_stage);
	}
	return spirv_stages;
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
			const Vector<RDC::ShaderStageSPIRVData> spirv_stages = compile_stages(format, p_stages, p_dynamic_buffers);
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
			// GodotNext: dxil_toolchain (#123). A ray-tracing stage is a DXIL library, with RDAT instead of PSV0.
			const bool found = read_psv0_resources(dxil, resources) || read_rdat_resources(dxil, resources);
			REQUIRE(found);
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
	// descriptor table range stays in space 0 at Mesa's set/binding register, except a bindless table's.
	void check_root_parameter_registers() const {
		const RDC::ShaderReflection reflection = container->get_shader_reflection();
		for (uint32_t i = 0; i < root_signature->NumParameters; i++) {
			const D3D12_ROOT_PARAMETER1 &root_param = root_signature->pParameters[i];
			if (root_param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
				CHECK_LT(root_param.Constants.ShaderRegister, SMALL_REGISTER_LIMIT);
				CHECK_EQ(root_param.Constants.RegisterSpace, ContainerD3D12::ROOT_CONSTANT_SPACE);
			} else if (root_param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
				for (uint32_t j = 0; j < root_param.DescriptorTable.NumDescriptorRanges; j++) {
					const D3D12_DESCRIPTOR_RANGE1 &range = root_param.DescriptorTable.pDescriptorRanges[j];
					// GodotNext: rd_bindless (#176). Only a bindless table's range leaves space 0: the only range
					// of its set's descriptor table, at register 0 of the set's space, of the table's size, for a
					// set whose one uniform is the table.
					if (range.RegisterSpace != 0) {
						INFO("Descriptor table ", i, ", register space ", range.RegisterSpace);
						CHECK_EQ(root_param.DescriptorTable.NumDescriptorRanges, 1u);
						CHECK_EQ(range.BaseShaderRegister, ContainerD3D12::BINDLESS_TABLE_REGISTER);
						REQUIRE_GE(range.RegisterSpace, ContainerD3D12::BINDLESS_TABLE_SPACE_BASE);
						const uint32_t set = range.RegisterSpace - ContainerD3D12::BINDLESS_TABLE_SPACE_BASE;
						REQUIRE_LT(set, uint32_t(reflection.uniform_sets.size()));
						REQUIRE_EQ(reflection.uniform_sets[set].size(), 1);
						const RDC::ShaderUniform &table = reflection.uniform_sets[set][0];
						CHECK_EQ(table.type, RDC::UNIFORM_TYPE_BINDLESS_TABLE);
						const bool sampler_range = range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
						CHECK_EQ(table.length == RDC::UNIFORM_TYPE_SAMPLER, sampler_range);
						CHECK_EQ(range.NumDescriptors, sampler_range ? ContainerD3D12::BINDLESS_SAMPLER_RANGE_SIZE : ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE);
						const ContainerD3D12::ReflectionBindingSetDataD3D12 &set_d3d12 = reflection_d3d12.reflection_binding_sets_d3d12[set];
						CHECK_EQ(sampler_range ? set_d3d12.sampler_root_param_idx : set_d3d12.resource_root_param_idx, i);
						continue;
					}
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

	// GodotNext: rd_bindless (#176). Whether a stage's DXIL declares a resource at exactly these registers.
	bool dxil_declares_range(uint32_t p_type, uint32_t p_space, uint32_t p_lower_bound, uint32_t p_upper_bound) const {
		for (const LocalVector<DxilResource> &resources : stage_resources) {
			for (const DxilResource &resource : resources) {
				if (resource.type == p_type && resource.space == p_space && resource.lower_bound == p_lower_bound && resource.upper_bound == p_upper_bound) {
					return true;
				}
			}
		}
		return false;
	}

	// GodotNext: rd_bindless (#176). The one range of the descriptor table at a root parameter.
	const D3D12_DESCRIPTOR_RANGE1 &only_range(uint32_t p_root_param_idx) const {
		const D3D12_ROOT_PARAMETER1 &root_param = param(p_root_param_idx);
		REQUIRE_EQ(root_param.ParameterType, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
		REQUIRE_EQ(root_param.DescriptorTable.NumDescriptorRanges, 1u);
		return root_param.DescriptorTable.pDescriptorRanges[0];
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

// GodotNext: dxil_toolchain (#123). Ray query and the ray-tracing stages, with the GodotNext Mesa NIR library.

// A ray-query kernel with root constants, NIR-DXIL runtime data (gl_NumWorkGroups), a dynamic UAV,
// and an acceleration structure and a read-only buffer in a descriptor table.
const char *RAY_QUERY_COMPUTE = R"(#version 460
#extension GL_EXT_ray_query : require
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, std430) readonly buffer Rays { vec4 data[]; } rays;
layout(set = 1, binding = 2, std430) writeonly buffer Hits { float t[]; } hits;
layout(push_constant, std430) uniform Params { uint count; uint pad0; uint pad1; uint pad2; } params;
void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= params.count) {
		return;
	}
	rayQueryEXT ray_query;
	rayQueryInitializeEXT(ray_query, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, rays.data[2u * index].xyz, 0.0, rays.data[2u * index + 1u].xyz, 1000.0);
	rayQueryProceedEXT(ray_query);
	float t = -1.0;
	if (rayQueryGetIntersectionTypeEXT(ray_query, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		t = rayQueryGetIntersectionTEXT(ray_query, true);
	}
	hits.t[index] = t + float(gl_NumWorkGroups.x);
}
)";

// A raygen with root constants and a dynamic uniform buffer, and a miss stage with a dynamic
// read-only buffer: one container, two DXIL libraries.
const char *RAYGEN = R"(#version 460
#extension GL_EXT_ray_tracing : require
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, rgba32f) uniform restrict writeonly image2D result_image;
layout(set = 1, binding = 0, std140) uniform Camera { vec4 origin; vec4 direction; } camera;
layout(push_constant, std430) uniform Params { float t_max; uint cull_mask; uint pad0; uint pad1; } params;
layout(location = 0) rayPayloadEXT vec4 payload;
void main() {
	payload = vec4(0.0);
	traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, params.cull_mask & 0xFFu, 0, 1, 0, camera.origin.xyz, 0.001, camera.direction.xyz, params.t_max, 0);
	imageStore(result_image, ivec2(gl_LaunchIDEXT.xy), payload);
}
)";

const char *MISS = R"(#version 460
#extension GL_EXT_ray_tracing : require
layout(set = 1, binding = 1, std430) readonly buffer Sky { vec4 color; } sky;
layout(location = 0) rayPayloadInEXT vec4 payload;
void main() {
	payload = sky.color;
}
)";

// With the upstream Mesa NIR library, ray query and the ray-tracing stages are refused (with one
// error; the [ShaderContainer][D3D12] cases check it), so there is no root signature to check.
void check_refused(const Vector<StageSource> &p_stages, const Vector<uint64_t> &p_dynamic_buffers) {
	HMODULE lib_d3d12 = LoadLibraryW(L"D3D12.dll");
	REQUIRE(lib_d3d12 != nullptr);
	{
		RenderingShaderContainerFormatD3D12 format;
		format.set_lib_d3d12(lib_d3d12);
		const Vector<RDC::ShaderStageSPIRVData> spirv_stages = compile_stages(format, p_stages, p_dynamic_buffers);
		Ref<RenderingShaderContainer> container = format.create_container();
		ERR_PRINT_OFF;
		CHECK_FALSE(container->set_code_from_spirv("test_d3d12_shader_container", spirv_stages));
		ERR_PRINT_ON;
	}
	FreeLibrary(lib_d3d12);
}

// With GODOT_D3D12_DXIL_VALIDATE=1 (debug builds), the DXIL validator checks each blob a container
// compiles: it must have checked p_blobs more and rejected none. Without it, nothing is checked.
void check_dxil_validation(const ContainerD3D12::DXILValidationStats &p_before, uint32_t p_blobs) {
	const ContainerD3D12::DXILValidationStats after = ContainerD3D12::get_dxil_validation_stats();
	CHECK_EQ(after.failed, p_before.failed);
	if (after.requested) {
		REQUIRE(after.active);
		CHECK_EQ(after.validated - p_before.validated, p_blobs);
	} else {
		CHECK_EQ(after.validated, 0u);
	}
}

TEST_CASE("[D3D12][ShaderContainer] Ray query root constants, runtime data and dynamic buffers use small registers outside space 0") {
	Vector<StageSource> stages;
	stages.push_back({ RDC::SHADER_STAGE_COMPUTE, RAY_QUERY_COMPUTE });
	Vector<uint64_t> dynamic_buffers;
	dynamic_buffers.push_back(ShaderRD::DynamicBuffer::encode(1, 2)); // Hits: root UAV.
	if (!ContainerD3D12::is_raytracing_supported()) {
		check_refused(stages, dynamic_buffers);
		return;
	}

	const ContainerD3D12::DXILValidationStats validation_before = ContainerD3D12::get_dxil_validation_stats();
	ShaderContainerFixture fixture(stages, dynamic_buffers);
	check_dxil_validation(validation_before, 1);

	const D3D12_ROOT_PARAMETER1 &push_constants = fixture.param(0);
	REQUIRE_EQ(push_constants.ParameterType, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
	CHECK_EQ(push_constants.Constants.ShaderRegister, ContainerD3D12::ROOT_CONSTANT_REGISTER);
	CHECK_EQ(push_constants.Constants.RegisterSpace, ContainerD3D12::ROOT_CONSTANT_SPACE);
	CHECK_EQ(push_constants.Constants.Num32BitValues, 4u);
	CHECK_EQ(fixture.reflection_d3d12.dxil_push_constant_stages, uint32_t(RDC::SHADER_STAGE_COMPUTE_BIT));

	REQUIRE_EQ(fixture.reflection_d3d12.nir_runtime_data_root_param_idx, 1u);
	const D3D12_ROOT_PARAMETER1 &runtime_data = fixture.param(1);
	REQUIRE_EQ(runtime_data.ParameterType, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
	CHECK_EQ(runtime_data.Constants.ShaderRegister, ContainerD3D12::RUNTIME_DATA_REGISTER);
	CHECK_EQ(runtime_data.Constants.RegisterSpace, ContainerD3D12::ROOT_CONSTANT_SPACE);

	const uint32_t dynamic_space = ContainerD3D12::DYNAMIC_BUFFER_SPACE_BASE + 1;
	const ContainerD3D12::ReflectionBindingDataD3D12 &hits = fixture.uniform_d3d12(1, 2);
	const D3D12_ROOT_PARAMETER1 &hits_param = fixture.param(hits.root_param_idx);
	CHECK_EQ(hits_param.ParameterType, D3D12_ROOT_PARAMETER_TYPE_UAV);
	CHECK_EQ(hits_param.Descriptor.ShaderRegister, 2u);
	CHECK_EQ(hits_param.Descriptor.RegisterSpace, dynamic_space);

	fixture.check_root_parameter_registers();
	fixture.check_dxil_resources_bound();

	CHECK(fixture.dxil_declares(2, ContainerD3D12::ROOT_CONSTANT_SPACE, ContainerD3D12::ROOT_CONSTANT_REGISTER)); // Push constants.
	CHECK(fixture.dxil_declares(2, ContainerD3D12::ROOT_CONSTANT_SPACE, ContainerD3D12::RUNTIME_DATA_REGISTER)); // Runtime data.
	CHECK(fixture.dxil_declares(7, dynamic_space, 2)); // Hits: raw UAV.
	CHECK_FALSE(fixture.dxil_declares(7, 0, 1 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER + 2 * GODOT_NIR_BINDING_MULTIPLIER));
	// The acceleration structure (a raw SRV) and the read-only buffer keep Mesa's registers in space 0.
	CHECK(fixture.dxil_declares(4, 0, 0)); // tlas, set 0 binding 0.
	CHECK(fixture.dxil_declares(4, 0, GODOT_NIR_BINDING_MULTIPLIER)); // Rays, set 0 binding 1.
}

TEST_CASE("[D3D12][ShaderContainer] Ray-tracing library stages use small registers outside space 0 for root constants and dynamic buffers") {
	Vector<StageSource> stages;
	stages.push_back({ RDC::SHADER_STAGE_RAYGEN, RAYGEN });
	stages.push_back({ RDC::SHADER_STAGE_MISS, MISS });
	Vector<uint64_t> dynamic_buffers;
	dynamic_buffers.push_back(ShaderRD::DynamicBuffer::encode(1, 0)); // Camera: root CBV.
	dynamic_buffers.push_back(ShaderRD::DynamicBuffer::encode(1, 1)); // Sky: root SRV.
	if (!ContainerD3D12::is_raytracing_supported()) {
		check_refused(stages, dynamic_buffers);
		return;
	}

	const ContainerD3D12::DXILValidationStats validation_before = ContainerD3D12::get_dxil_validation_stats();
	ShaderContainerFixture fixture(stages, dynamic_buffers);
	check_dxil_validation(validation_before, 2);
	REQUIRE_EQ(fixture.container->shaders.size(), 2);

	const D3D12_ROOT_PARAMETER1 &push_constants = fixture.param(0);
	REQUIRE_EQ(push_constants.ParameterType, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
	CHECK_EQ(push_constants.Constants.ShaderRegister, ContainerD3D12::ROOT_CONSTANT_REGISTER);
	CHECK_EQ(push_constants.Constants.RegisterSpace, ContainerD3D12::ROOT_CONSTANT_SPACE);
	CHECK_EQ(push_constants.Constants.Num32BitValues, 4u);
	CHECK_EQ(fixture.reflection_d3d12.dxil_push_constant_stages, uint32_t(RDC::SHADER_STAGE_RAYGEN_BIT));
	CHECK_EQ(fixture.reflection_d3d12.nir_runtime_data_root_param_idx, UINT32_MAX);

	const uint32_t dynamic_space = ContainerD3D12::DYNAMIC_BUFFER_SPACE_BASE + 1;
	const ContainerD3D12::ReflectionBindingDataD3D12 &camera = fixture.uniform_d3d12(1, 0);
	const D3D12_ROOT_PARAMETER1 &camera_param = fixture.param(camera.root_param_idx);
	CHECK_EQ(camera_param.ParameterType, D3D12_ROOT_PARAMETER_TYPE_CBV);
	CHECK_EQ(camera_param.Descriptor.ShaderRegister, 0u);
	CHECK_EQ(camera_param.Descriptor.RegisterSpace, dynamic_space);
	CHECK_EQ(camera.dxil_stages, uint32_t(RDC::SHADER_STAGE_RAYGEN_BIT));

	const ContainerD3D12::ReflectionBindingDataD3D12 &sky = fixture.uniform_d3d12(1, 1);
	const D3D12_ROOT_PARAMETER1 &sky_param = fixture.param(sky.root_param_idx);
	CHECK_EQ(sky_param.ParameterType, D3D12_ROOT_PARAMETER_TYPE_SRV);
	CHECK_EQ(sky_param.Descriptor.ShaderRegister, 1u);
	CHECK_EQ(sky_param.Descriptor.RegisterSpace, dynamic_space);
	CHECK_EQ(sky.dxil_stages, uint32_t(RDC::SHADER_STAGE_MISS_BIT));

	// Each library's resource table (RDAT) agrees with the root signature.
	fixture.check_root_parameter_registers();
	fixture.check_dxil_resources_bound();

	CHECK(fixture.dxil_declares(2, ContainerD3D12::ROOT_CONSTANT_SPACE, ContainerD3D12::ROOT_CONSTANT_REGISTER)); // Push constants.
	CHECK(fixture.dxil_declares(2, dynamic_space, 0)); // Camera: CBV.
	CHECK(fixture.dxil_declares(4, dynamic_space, 1)); // Sky: raw SRV.
	CHECK_FALSE(fixture.dxil_declares(2, 0, 1 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER + 0 * GODOT_NIR_BINDING_MULTIPLIER));
	CHECK_FALSE(fixture.dxil_declares(4, 0, 1 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER + 1 * GODOT_NIR_BINDING_MULTIPLIER));
	CHECK(fixture.dxil_declares(4, 0, 0)); // tlas, set 0 binding 0.
	CHECK(fixture.dxil_declares(6, 0, GODOT_NIR_BINDING_MULTIPLIER)); // result_image: typed UAV, set 0 binding 1.
}

// GodotNext: rd_bindless (#176). Mesa's NIR-to-DXIL sizes a resource array by its element count, so it
// never declared a texture or sampler runtime array and declared a storage-buffer one with a range size
// of 0; pipelines with such shaders failed to create. Each bindless table now has the size of its root
// signature range, at register 0 in its set's space.

const char *COMPUTE_BINDLESS = R"(#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) restrict writeonly buffer Results { uint data[]; } results;
layout(set = 1, binding = 0) uniform texture2D textures[];
layout(set = 2, binding = 0) uniform sampler samplers[];
layout(set = 3, binding = 0, std430) restrict readonly buffer Words { uint data[]; } words[];
layout(push_constant, std430) uniform Params { uint texture_slot; uint sampler_slot; uint buffer_slot; uint pad; } params;
void main() {
	uint index = gl_GlobalInvocationID.x;
	vec4 color = textureLod(sampler2D(textures[nonuniformEXT(params.texture_slot + index)], samplers[nonuniformEXT(params.sampler_slot + index)]), vec2(0.5), 0.0);
	results.data[index] = packUnorm4x8(color) ^ words[nonuniformEXT(params.buffer_slot + index)].data[index];
}
)";

const char *RASTER_BINDLESS_VERTEX = R"(#version 450
layout(push_constant, std430) uniform Params { float scale; } params;
layout(location = 0) in vec3 vertex;
layout(location = 0) out vec2 uv;
void main() {
	uv = vertex.xy;
	gl_Position = vec4(vertex * params.scale, 1.0);
}
)";

const char *RASTER_BINDLESS_FRAGMENT = R"(#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0, std140) uniform Material { uint texture_slot; uint sampler_slot; } material;
layout(set = 1, binding = 0) uniform texture2D textures[];
layout(set = 2, binding = 0) uniform sampler samplers[];
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag_color;
void main() {
	frag_color = texture(sampler2D(textures[nonuniformEXT(material.texture_slot)], samplers[nonuniformEXT(material.sampler_slot)]), uv);
}
)";

TEST_CASE("[D3D12][ShaderContainer] Compute bindless tables are bounded arrays at register 0 in a space of their own") {
	Vector<StageSource> stages;
	stages.push_back({ RDC::SHADER_STAGE_COMPUTE, COMPUTE_BINDLESS });
	const ContainerD3D12::DXILValidationStats validation_before = ContainerD3D12::get_dxil_validation_stats();
	ShaderContainerFixture fixture(stages, Vector<uint64_t>());
	check_dxil_validation(validation_before, 1);

	const uint32_t texture_space = ContainerD3D12::BINDLESS_TABLE_SPACE_BASE + 1;
	const uint32_t sampler_space = ContainerD3D12::BINDLESS_TABLE_SPACE_BASE + 2;
	const uint32_t buffer_space = ContainerD3D12::BINDLESS_TABLE_SPACE_BASE + 3;
	const D3D12_DESCRIPTOR_RANGE_FLAGS resource_flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

	// The root signature binds each table through a descriptor table of one volatile range of the table's
	// size, at register 0 in the table's space.
	const D3D12_DESCRIPTOR_RANGE1 &textures = fixture.only_range(fixture.reflection_d3d12.reflection_binding_sets_d3d12[1].resource_root_param_idx);
	CHECK_EQ(textures.RangeType, D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
	CHECK_EQ(textures.NumDescriptors, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE);
	CHECK_EQ(textures.BaseShaderRegister, ContainerD3D12::BINDLESS_TABLE_REGISTER);
	CHECK_EQ(textures.RegisterSpace, texture_space);
	CHECK_EQ(textures.Flags, resource_flags);

	const D3D12_DESCRIPTOR_RANGE1 &samplers = fixture.only_range(fixture.reflection_d3d12.reflection_binding_sets_d3d12[2].sampler_root_param_idx);
	CHECK_EQ(samplers.RangeType, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);
	CHECK_EQ(samplers.NumDescriptors, ContainerD3D12::BINDLESS_SAMPLER_RANGE_SIZE);
	CHECK_EQ(samplers.BaseShaderRegister, ContainerD3D12::BINDLESS_TABLE_REGISTER);
	CHECK_EQ(samplers.RegisterSpace, sampler_space);
	CHECK_EQ(samplers.Flags, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

	const D3D12_DESCRIPTOR_RANGE1 &words = fixture.only_range(fixture.reflection_d3d12.reflection_binding_sets_d3d12[3].resource_root_param_idx);
	CHECK_EQ(words.RangeType, D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
	CHECK_EQ(words.NumDescriptors, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE);
	CHECK_EQ(words.BaseShaderRegister, ContainerD3D12::BINDLESS_TABLE_REGISTER);
	CHECK_EQ(words.RegisterSpace, buffer_space);
	CHECK_EQ(words.Flags, resource_flags);

	fixture.check_root_parameter_registers();
	fixture.check_dxil_resources_bound();

	// The DXIL declares every table, as an array of exactly the range's size where the range is.
	CHECK(fixture.dxil_declares_range(3, texture_space, 0, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE - 1)); // Typed SRV.
	CHECK(fixture.dxil_declares_range(1, sampler_space, 0, ContainerD3D12::BINDLESS_SAMPLER_RANGE_SIZE - 1)); // Sampler.
	CHECK(fixture.dxil_declares_range(4, buffer_space, 0, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE - 1)); // Raw SRV.
	CHECK_FALSE(fixture.dxil_declares(3, 0, 1 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER));
	CHECK_FALSE(fixture.dxil_declares(1, 0, 2 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER));
	CHECK_FALSE(fixture.dxil_declares(4, 0, 3 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER));
	// The other resources keep Mesa's registers in space 0.
	CHECK(fixture.dxil_declares(7, 0, 0)); // Results, set 0 binding 0: raw UAV.

	// The reflection finds each table where the DXIL declares it.
	CHECK_EQ(fixture.uniform_d3d12(1, 0).dxil_stages, 1u << RDC::SHADER_STAGE_COMPUTE);
	CHECK_EQ(fixture.uniform_d3d12(1, 0).resource_class, uint32_t(RES_CLASS_SRV));
	CHECK_EQ(fixture.uniform_d3d12(2, 0).dxil_stages, 1u << RDC::SHADER_STAGE_COMPUTE);
	CHECK(fixture.uniform_d3d12(2, 0).has_sampler);
	CHECK_EQ(fixture.uniform_d3d12(3, 0).dxil_stages, 1u << RDC::SHADER_STAGE_COMPUTE);
	CHECK_EQ(fixture.uniform_d3d12(3, 0).resource_class, uint32_t(RES_CLASS_SRV));
}

TEST_CASE("[D3D12][ShaderContainer] Raster bindless tables and dynamic buffers keep apart in register spaces") {
	Vector<StageSource> stages;
	stages.push_back({ RDC::SHADER_STAGE_VERTEX, RASTER_BINDLESS_VERTEX });
	stages.push_back({ RDC::SHADER_STAGE_FRAGMENT, RASTER_BINDLESS_FRAGMENT });
	Vector<uint64_t> dynamic_buffers;
	dynamic_buffers.push_back(ShaderRD::DynamicBuffer::encode(0, 0)); // Material: root CBV.
	const ContainerD3D12::DXILValidationStats validation_before = ContainerD3D12::get_dxil_validation_stats();
	ShaderContainerFixture fixture(stages, dynamic_buffers);
	check_dxil_validation(validation_before, 2);

	// The dynamic buffer of set 0 and the tables of sets 1 and 2 each have a space of their own.
	const ContainerD3D12::ReflectionBindingDataD3D12 &material = fixture.uniform_d3d12(0, 0);
	const D3D12_ROOT_PARAMETER1 &material_param = fixture.param(material.root_param_idx);
	CHECK_EQ(material_param.ParameterType, D3D12_ROOT_PARAMETER_TYPE_CBV);
	CHECK_EQ(material_param.Descriptor.ShaderRegister, 0u);
	CHECK_EQ(material_param.Descriptor.RegisterSpace, ContainerD3D12::DYNAMIC_BUFFER_SPACE_BASE + 0);

	const uint32_t texture_space = ContainerD3D12::BINDLESS_TABLE_SPACE_BASE + 1;
	const uint32_t sampler_space = ContainerD3D12::BINDLESS_TABLE_SPACE_BASE + 2;

	const uint32_t texture_param_idx = fixture.reflection_d3d12.reflection_binding_sets_d3d12[1].resource_root_param_idx;
	const D3D12_DESCRIPTOR_RANGE1 &textures = fixture.only_range(texture_param_idx);
	CHECK_EQ(textures.NumDescriptors, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE);
	CHECK_EQ(textures.BaseShaderRegister, ContainerD3D12::BINDLESS_TABLE_REGISTER);
	CHECK_EQ(textures.RegisterSpace, texture_space);
	CHECK_EQ(fixture.param(texture_param_idx).ShaderVisibility, D3D12_SHADER_VISIBILITY_PIXEL);

	const uint32_t sampler_param_idx = fixture.reflection_d3d12.reflection_binding_sets_d3d12[2].sampler_root_param_idx;
	const D3D12_DESCRIPTOR_RANGE1 &samplers = fixture.only_range(sampler_param_idx);
	CHECK_EQ(samplers.NumDescriptors, ContainerD3D12::BINDLESS_SAMPLER_RANGE_SIZE);
	CHECK_EQ(samplers.BaseShaderRegister, ContainerD3D12::BINDLESS_TABLE_REGISTER);
	CHECK_EQ(samplers.RegisterSpace, sampler_space);
	CHECK_EQ(fixture.param(sampler_param_idx).ShaderVisibility, D3D12_SHADER_VISIBILITY_PIXEL);

	fixture.check_root_parameter_registers();
	fixture.check_dxil_resources_bound();

	CHECK(fixture.dxil_declares(2, ContainerD3D12::DYNAMIC_BUFFER_SPACE_BASE + 0, 0)); // Material.
	CHECK(fixture.dxil_declares_range(3, texture_space, 0, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE - 1));
	CHECK(fixture.dxil_declares_range(1, sampler_space, 0, ContainerD3D12::BINDLESS_SAMPLER_RANGE_SIZE - 1));
	CHECK_EQ(fixture.uniform_d3d12(1, 0).dxil_stages, 1u << RDC::SHADER_STAGE_FRAGMENT);
	CHECK_EQ(fixture.uniform_d3d12(2, 0).dxil_stages, 1u << RDC::SHADER_STAGE_FRAGMENT);
}

// GodotNext: rd_bindless (#176). Buffer device addresses raise the shader model to 6.6, where Mesa creates
// handles with createHandleFromBinding; the tables must still be declared where the root signature puts them.
const char *COMPUTE_BINDLESS_DEVICE_ADDRESS = R"(#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 64) in;
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Words { uint data[]; };
layout(set = 0, binding = 0, std430) restrict writeonly buffer Results { uint data[]; } results;
layout(set = 1, binding = 0) uniform texture2D textures[];
layout(set = 2, binding = 0, std430) restrict readonly buffer Colors { uint data[]; } colors[];
layout(push_constant, std430) uniform Params { Words words; uint texture_slot; uint buffer_slot; } params;
void main() {
	uint index = gl_GlobalInvocationID.x;
	uint texel = packUnorm4x8(texelFetch(textures[nonuniformEXT(params.texture_slot + index)], ivec2(0), 0));
	results.data[index] = texel ^ colors[nonuniformEXT(params.buffer_slot + index)].data[index] ^ params.words.data[index];
}
)";

TEST_CASE("[D3D12][ShaderContainer] Bindless tables with buffer device addresses (shader model 6.6) are bounded arrays at register 0 in a space of their own") {
	Vector<StageSource> stages;
	stages.push_back({ RDC::SHADER_STAGE_COMPUTE, COMPUTE_BINDLESS_DEVICE_ADDRESS });
	const ContainerD3D12::DXILValidationStats validation_before = ContainerD3D12::get_dxil_validation_stats();
	ShaderContainerFixture fixture(stages, Vector<uint64_t>());
	check_dxil_validation(validation_before, 1);

	const uint32_t texture_space = ContainerD3D12::BINDLESS_TABLE_SPACE_BASE + 1;
	const uint32_t buffer_space = ContainerD3D12::BINDLESS_TABLE_SPACE_BASE + 2;
	const D3D12_DESCRIPTOR_RANGE1 &textures = fixture.only_range(fixture.reflection_d3d12.reflection_binding_sets_d3d12[1].resource_root_param_idx);
	CHECK_EQ(textures.NumDescriptors, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE);
	CHECK_EQ(textures.RegisterSpace, texture_space);
	const D3D12_DESCRIPTOR_RANGE1 &colors = fixture.only_range(fixture.reflection_d3d12.reflection_binding_sets_d3d12[2].resource_root_param_idx);
	CHECK_EQ(colors.NumDescriptors, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE);
	CHECK_EQ(colors.RegisterSpace, buffer_space);

	fixture.check_root_parameter_registers();
	fixture.check_dxil_resources_bound();

	CHECK(fixture.dxil_declares_range(3, texture_space, 0, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE - 1)); // Typed SRV.
	CHECK(fixture.dxil_declares_range(4, buffer_space, 0, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE - 1)); // Raw SRV.
	CHECK_FALSE(fixture.dxil_declares(3, 0, 1 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER));
	CHECK_FALSE(fixture.dxil_declares(4, 0, 2 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER));
}

// GodotNext: rd_bindless (#176). Bindless tables in a ray-query kernel and in ray-tracing libraries, which
// compile only with the GodotNext Mesa NIR library (dxil_toolchain, #123). Its patches change how
// nir_to_dxil declares resources and creates their handles, so the tables are checked through it too.

const char *RAY_QUERY_BINDLESS = R"(#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, std430) writeonly buffer Hits { float t[]; } hits;
layout(set = 1, binding = 0, std430) readonly buffer Rays { vec4 data[]; } rays[];
layout(set = 2, binding = 0) uniform texture2D textures[];
layout(push_constant, std430) uniform Params { uint count; uint pad0; uint pad1; uint pad2; } params;
void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= params.count) {
		return;
	}
	rayQueryEXT ray_query;
	rayQueryInitializeEXT(ray_query, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, rays[nonuniformEXT(index)].data[0].xyz, 0.0, rays[nonuniformEXT(index)].data[1].xyz, 1000.0);
	rayQueryProceedEXT(ray_query);
	float t = -1.0;
	if (rayQueryGetIntersectionTypeEXT(ray_query, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		t = rayQueryGetIntersectionTEXT(ray_query, true);
	}
	hits.t[index] = t * texelFetch(textures[nonuniformEXT(index)], ivec2(0), 0).x;
}
)";

const char *RAYGEN_BINDLESS = R"(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, rgba32f) uniform restrict writeonly image2D result_image;
layout(set = 1, binding = 0) uniform texture2D textures[];
layout(push_constant, std430) uniform Params { float t_max; uint pad0; uint pad1; uint pad2; } params;
layout(location = 0) rayPayloadEXT vec4 payload;
void main() {
	payload = vec4(0.0);
	traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, 0xFFu, 0, 1, 0, vec3(0.0), 0.001, vec3(0.0, 0.0, 1.0), params.t_max, 0);
	imageStore(result_image, ivec2(gl_LaunchIDEXT.xy), payload * texelFetch(textures[nonuniformEXT(gl_LaunchIDEXT.x)], ivec2(0), 0));
}
)";

// The GodotNext Mesa library translates a hit stage only when it declares a payload and a hit attribute.
const char *CLOSEST_HIT_BINDLESS = R"(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 2, binding = 0, std430) readonly buffer Colors { vec4 color; } colors[];
layout(location = 0) rayPayloadInEXT vec4 payload;
hitAttributeEXT vec2 barycentrics;
void main() {
	payload = colors[nonuniformEXT(gl_InstanceCustomIndexEXT)].color * barycentrics.x;
}
)";

const char *MISS_BINDLESS = R"(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 2, binding = 0, std430) readonly buffer Colors { vec4 color; } colors[];
layout(location = 0) rayPayloadInEXT vec4 payload;
void main() {
	payload = colors[nonuniformEXT(uint(gl_LaunchIDEXT.x))].color;
}
)";

TEST_CASE("[D3D12][ShaderContainer] Ray query bindless tables are bounded arrays at register 0 in a space of their own") {
	Vector<StageSource> stages;
	stages.push_back({ RDC::SHADER_STAGE_COMPUTE, RAY_QUERY_BINDLESS });
	if (!ContainerD3D12::is_raytracing_supported()) {
		check_refused(stages, Vector<uint64_t>());
		return;
	}

	const ContainerD3D12::DXILValidationStats validation_before = ContainerD3D12::get_dxil_validation_stats();
	ShaderContainerFixture fixture(stages, Vector<uint64_t>());
	check_dxil_validation(validation_before, 1);

	const uint32_t buffer_space = ContainerD3D12::BINDLESS_TABLE_SPACE_BASE + 1;
	const uint32_t texture_space = ContainerD3D12::BINDLESS_TABLE_SPACE_BASE + 2;
	const D3D12_DESCRIPTOR_RANGE1 &rays = fixture.only_range(fixture.reflection_d3d12.reflection_binding_sets_d3d12[1].resource_root_param_idx);
	CHECK_EQ(rays.RangeType, D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
	CHECK_EQ(rays.NumDescriptors, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE);
	CHECK_EQ(rays.RegisterSpace, buffer_space);
	const D3D12_DESCRIPTOR_RANGE1 &textures = fixture.only_range(fixture.reflection_d3d12.reflection_binding_sets_d3d12[2].resource_root_param_idx);
	CHECK_EQ(textures.RangeType, D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
	CHECK_EQ(textures.NumDescriptors, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE);
	CHECK_EQ(textures.RegisterSpace, texture_space);

	fixture.check_root_parameter_registers();
	fixture.check_dxil_resources_bound();

	CHECK(fixture.dxil_declares_range(4, buffer_space, 0, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE - 1)); // Raw SRV.
	CHECK(fixture.dxil_declares_range(3, texture_space, 0, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE - 1)); // Typed SRV.
	CHECK_FALSE(fixture.dxil_declares(4, 0, 1 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER));
	CHECK_FALSE(fixture.dxil_declares(3, 0, 2 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER));
	CHECK(fixture.dxil_declares(4, 0, 0)); // tlas, set 0 binding 0.
	CHECK(fixture.dxil_declares(7, 0, GODOT_NIR_BINDING_MULTIPLIER)); // Hits, set 0 binding 1: raw UAV.
}

TEST_CASE("[D3D12][ShaderContainer] Ray-tracing library bindless tables are bounded arrays at register 0 in a space of their own") {
	Vector<StageSource> stages;
	stages.push_back({ RDC::SHADER_STAGE_RAYGEN, RAYGEN_BINDLESS });
	stages.push_back({ RDC::SHADER_STAGE_CLOSEST_HIT, CLOSEST_HIT_BINDLESS });
	stages.push_back({ RDC::SHADER_STAGE_MISS, MISS_BINDLESS });
	if (!ContainerD3D12::is_raytracing_supported()) {
		check_refused(stages, Vector<uint64_t>());
		return;
	}

	const ContainerD3D12::DXILValidationStats validation_before = ContainerD3D12::get_dxil_validation_stats();
	ShaderContainerFixture fixture(stages, Vector<uint64_t>());
	check_dxil_validation(validation_before, 3);
	REQUIRE_EQ(fixture.container->shaders.size(), 3);

	const uint32_t texture_space = ContainerD3D12::BINDLESS_TABLE_SPACE_BASE + 1;
	const uint32_t buffer_space = ContainerD3D12::BINDLESS_TABLE_SPACE_BASE + 2;
	const D3D12_DESCRIPTOR_RANGE1 &textures = fixture.only_range(fixture.reflection_d3d12.reflection_binding_sets_d3d12[1].resource_root_param_idx);
	CHECK_EQ(textures.RangeType, D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
	CHECK_EQ(textures.NumDescriptors, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE);
	CHECK_EQ(textures.RegisterSpace, texture_space);
	const D3D12_DESCRIPTOR_RANGE1 &colors = fixture.only_range(fixture.reflection_d3d12.reflection_binding_sets_d3d12[2].resource_root_param_idx);
	CHECK_EQ(colors.RangeType, D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
	CHECK_EQ(colors.NumDescriptors, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE);
	CHECK_EQ(colors.RegisterSpace, buffer_space);

	// Each library's resource table (RDAT) agrees with the root signature.
	fixture.check_root_parameter_registers();
	fixture.check_dxil_resources_bound();

	CHECK(fixture.dxil_declares_range(3, texture_space, 0, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE - 1)); // Raygen: typed SRV.
	CHECK(fixture.dxil_declares_range(4, buffer_space, 0, ContainerD3D12::BINDLESS_RESOURCE_RANGE_SIZE - 1)); // Closest hit and miss: raw SRV.
	CHECK_FALSE(fixture.dxil_declares(3, 0, 1 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER));
	CHECK_FALSE(fixture.dxil_declares(4, 0, 2 * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER));
	CHECK_EQ(fixture.uniform_d3d12(1, 0).dxil_stages, uint32_t(RDC::SHADER_STAGE_RAYGEN_BIT));
	CHECK_EQ(fixture.uniform_d3d12(2, 0).dxil_stages, uint32_t(RDC::SHADER_STAGE_CLOSEST_HIT_BIT | RDC::SHADER_STAGE_MISS_BIT));
}

// GodotNext: rd_indirect_count (#177). A vertex shader that reads gl_InstanceIndex, gl_VertexIndex and the draw
// parameters gl_BaseVertex and gl_BaseInstance.
const char *INSTANCE_INDEX_VERTEX = R"(#version 460
layout(location = 0) flat out uvec4 ids;
void main() {
	ids = uvec4(gl_InstanceIndex, gl_VertexIndex, gl_BaseVertex, gl_BaseInstance);
	gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

// The same with gl_DrawID, which NIR-DXIL reads from its runtime data.
const char *DRAW_ID_VERTEX = R"(#version 460
layout(location = 0) flat out uvec4 ids;
void main() {
	ids = uvec4(gl_InstanceIndex, gl_VertexIndex, gl_BaseVertex, gl_DrawID);
	gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

const char *INSTANCE_INDEX_FRAGMENT = R"(#version 450
layout(location = 0) flat in uvec4 ids;
layout(location = 0) out uvec4 frag_ids;
void main() {
	frag_ids = ids;
}
)";

// The data of the DXIL container part p_fourcc, or null.
const uint8_t *find_dxil_part(const Vector<uint8_t> &p_dxil, const char *p_fourcc, uint32_t &r_size) {
	if (p_dxil.size() < 32 || read_u32(p_dxil.ptr()) != read_u32((const uint8_t *)"DXBC")) {
		return nullptr;
	}
	const uint32_t part_count = read_u32(p_dxil.ptr() + 28);
	for (uint32_t i = 0; i < part_count; i++) {
		const uint32_t offset = read_u32(p_dxil.ptr() + 32 + i * 4);
		if (offset + 8 <= uint32_t(p_dxil.size()) && read_u32(p_dxil.ptr() + offset) == read_u32((const uint8_t *)p_fourcc)) {
			r_size = read_u32(p_dxil.ptr() + offset + 4);
			return p_dxil.ptr() + offset + 8;
		}
	}
	return nullptr;
}

struct StageModel {
	uint32_t shader_model_minor = 0; // From the DXIL part's program version.
	bool extended_command_info = false; // SFI0 bit 32: SV_StartVertexLocation or SV_StartInstanceLocation.
};

// Compiles p_vertex (by default the vertex shader above) and the fragment shader above, with or without
// base_instance_native. r_runtime_data_root_param_idx is the root parameter of NIR-DXIL's runtime data, which
// the Direct3D 12 driver fills for compute only (UINT32_MAX when the shader reads none).
LocalVector<StageModel> compile_instance_index_stages(bool p_base_instance_native, uint32_t &r_runtime_data_root_param_idx, const char *p_vertex = INSTANCE_INDEX_VERTEX) {
	LocalVector<StageModel> models;
	HMODULE lib_d3d12 = LoadLibraryW(L"D3D12.dll");
	REQUIRE(lib_d3d12 != nullptr);
	RenderingShaderContainerFormatD3D12 format;
	format.set_lib_d3d12(lib_d3d12);
	Vector<StageSource> sources;
	sources.push_back({ RDC::SHADER_STAGE_VERTEX, p_vertex });
	sources.push_back({ RDC::SHADER_STAGE_FRAGMENT, INSTANCE_INDEX_FRAGMENT });
	const Vector<RDC::ShaderStageSPIRVData> spirv_stages = compile_stages(format, sources, Vector<uint64_t>());
	Ref<RenderingShaderContainer> container = format.create_container();
	REQUIRE(container->set_code_from_spirv("test_base_instance_native", spirv_stages, p_base_instance_native));
	r_runtime_data_root_param_idx = static_cast<ContainerD3D12 *>(container.ptr())->get_shader_reflection_d3d12().nir_runtime_data_root_param_idx;
	for (const RenderingShaderContainer::Shader &shader : container->shaders) {
		Vector<uint8_t> dxil;
		dxil.resize(shader.code_decompressed_size);
		REQUIRE(container->decompress_code(shader.code_compressed_bytes.ptr(), shader.code_compressed_bytes.size(), shader.code_compression_flags, dxil.ptrw(), dxil.size()));
		StageModel model;
		uint32_t size = 0;
		const uint8_t *program = find_dxil_part(dxil, "DXIL", size);
		REQUIRE(program != nullptr);
		REQUIRE(size >= 4);
		model.shader_model_minor = read_u32(program) & 0xf; // Kind << 16 | major << 4 | minor.
		CHECK_EQ((read_u32(program) >> 4) & 0xf, 6u);
		const uint8_t *features = find_dxil_part(dxil, "SFI0", size);
		REQUIRE(features != nullptr);
		REQUIRE(size >= 8);
		model.extended_command_info = (read_u32(features + 4) & 1u) != 0;
		models.push_back(model);
	}
	container.unref();
	FreeLibrary(lib_d3d12);
	return models;
}

// GodotNext: rd_indirect_count (#177). By default the first instance and first vertex read as 0, so the DXIL
// stays at the base shader model; base_instance_native reads them from SV_StartInstanceLocation and
// SV_StartVertexLocation, which need shader model 6.8. Vulkan's gl_BaseVertex is the first vertex (Mesa's
// first_vertex system value), so it reads SV_StartVertexLocation too, and none of these reads the runtime data.
TEST_CASE("[D3D12][ShaderContainer] base_instance_native compiles gl_InstanceIndex with SV_StartInstanceLocation at shader model 6.8") {
	uint32_t runtime_data = 0;
	const LocalVector<StageModel> plain = compile_instance_index_stages(false, runtime_data);
	REQUIRE_EQ(plain.size(), 2u);
	for (const StageModel &model : plain) {
		CHECK(model.shader_model_minor < 8u);
		CHECK_FALSE(model.extended_command_info);
	}
	CHECK_EQ(runtime_data, UINT32_MAX);

	const LocalVector<StageModel> native = compile_instance_index_stages(true, runtime_data);
	REQUIRE_EQ(native.size(), 2u);
	// The vertex stage reads the start locations; both stages are shader model 6.8.
	CHECK_EQ(native[0].shader_model_minor, 8u);
	CHECK(native[0].extended_command_info);
	CHECK_EQ(native[1].shader_model_minor, 8u);
	CHECK_FALSE(native[1].extended_command_info);
	// gl_BaseVertex and gl_BaseInstance need no runtime data, which the driver never sets for draws.
	CHECK_EQ(runtime_data, UINT32_MAX);

	// gl_DrawID does read the runtime data, in both modes: the check above can see it.
	compile_instance_index_stages(false, runtime_data, DRAW_ID_VERTEX);
	CHECK_NE(runtime_data, UINT32_MAX);
	compile_instance_index_stages(true, runtime_data, DRAW_ID_VERTEX);
	CHECK_NE(runtime_data, UINT32_MAX);
}

} // namespace TestD3D12ShaderContainer

#endif // NIR_ENABLED

#endif // D3D12_ENABLED && MODULE_GLSLANG_ENABLED
