/**************************************************************************/
/*  rendering_shader_container_d3d12.h                                    */
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

#include "servers/rendering/rendering_shader_container.h"

#define NIR_ENABLED 1

#ifdef SHADER_BAKER_RUNTIME_ENABLED
#undef NIR_ENABLED
#endif

#include "drivers/d3d12/d3d12_godot_nir_bridge.h"

// GodotNext: dxil_toolchain (#123). One slot per stage that can carry specialization constants: vertex,
// fragment, compute and the five ray-tracing stages (each one is its own DXIL library).
#define D3D12_BITCODE_OFFSETS_NUM_STAGES 8

#if NIR_ENABLED
struct nir_shader;
struct nir_shader_compiler_options;
#endif

enum RootSignatureLocationType {
	RS_LOC_TYPE_RESOURCE,
	RS_LOC_TYPE_SAMPLER,
};

enum ResourceClass {
	RES_CLASS_INVALID,
	RES_CLASS_CBV,
	RES_CLASS_SRV,
	RES_CLASS_UAV,
};

struct RenderingDXIL {
	static uint32_t patch_specialization_constant(
			RenderingDeviceCommons::PipelineSpecializationConstantType p_type,
			const void *p_value,
			const uint64_t (&p_stages_bit_offsets)[D3D12_BITCODE_OFFSETS_NUM_STAGES],
			HashMap<RenderingDeviceCommons::ShaderStage, Vector<uint8_t>> &r_stages_bytecodes,
			bool p_is_first_patch);

	static void sign_bytecode(RenderingDeviceCommons::ShaderStage p_stage, Vector<uint8_t> &r_dxil_blob);
};

class RenderingShaderContainerD3D12 : public RenderingShaderContainer {
	GDSOFTCLASS(RenderingShaderContainerD3D12, RenderingShaderContainer);

public:
	static constexpr uint32_t REQUIRED_SHADER_MODEL = 0x62; // D3D_SHADER_MODEL_6_2
	// GodotNext: d3d12_gpu_based_validation (#272). Root constants and root descriptors use small
	// registers in register spaces of their own. With GPU-based validation on, CreateRootSignature
	// commits memory in proportion to the highest root constant or root descriptor register (about
	// 23 bytes per register), so registers of the form set * GODOT_NIR_DESCRIPTOR_SET_MULTIPLIER ran
	// it out of memory. Descriptor tables keep space 0 and the registers Mesa assigns.
	static constexpr uint32_t ROOT_CONSTANT_SPACE = 1;
	static constexpr uint32_t ROOT_CONSTANT_REGISTER = 0;
	static constexpr uint32_t RUNTIME_DATA_REGISTER = 1;
	// The dynamic buffers of set i: register <binding> in space DYNAMIC_BUFFER_SPACE_BASE + i.
	static constexpr uint32_t DYNAMIC_BUFFER_SPACE_BASE = 2;
	// Mesa keeps a resource's register space in 5 bits (nir_variable::data.descriptor_set).
	static_assert(DYNAMIC_BUFFER_SPACE_BASE + RenderingDeviceCommons::MAX_UNIFORM_SETS <= 32);
	// GodotNext: dxil_toolchain (#123). Version 2 has 8 specialization-constant bit-offset slots instead of 3.
	// GodotNext: d3d12_rt_pipelines (#121). Version 4 appends the ray-tracing stages' RaytracingStageDataD3D12 to the
	// footer. Older containers are still read, without it (see has_raytracing_stage_data): versions 2 and 3 have
	// version 2's layout.
	static constexpr uint32_t FORMAT_VERSION = 4;
	// GodotNext: dxil_atomics (#279). The layout of FORMAT_VERSION, with DXIL from a Mesa NIR library that declares
	// atomics without readnone (25.3.1-gn3 or later, see declares_atomics_without_readnone()). A build writes this or
	// FORMAT_VERSION, and shader caches rebuild containers of the other one, so a build with such a library and one
	// with an older library do not reuse each other's DXIL. FORMAT_VERSION covers both 25.3.1-gn2 and the upstream
	// library, whose builds still reuse each other's caches as before #279; a later library that changes DXIL again
	// needs its own format version. Before #121 the two were 2 and 3.
	static constexpr uint32_t FORMAT_VERSION_ATOMICS_WITHOUT_READNONE = 5;
	static constexpr uint32_t RAYTRACING_STAGE_DATA_FORMAT_VERSION = 4;
	// GodotNext: d3d12_rt_pipelines (#121). A hit group's shader record (shaderRecordEXT) is the constant buffer at
	// register SHADER_RECORD_REGISTER in ROOT_CONSTANT_SPACE, filled by the root constants of a local root signature
	// from the record's bytes after the shader identifier. Before Mesa's register assignment the record is a uniform
	// buffer at descriptor set SHADER_RECORD_SPIRV_SET (past every real set), binding 0.
	static constexpr uint32_t SHADER_RECORD_REGISTER = 2;
	static constexpr uint32_t SHADER_RECORD_SPIRV_SET = RenderingDeviceCommons::MAX_UNIFORM_SETS;
	static constexpr uint32_t SHADER_RECORD_SPIRV_BINDING = 0;
	static constexpr uint32_t RAYTRACING_STAGE_COUNT = RenderingDeviceCommons::SHADER_STAGE_INTERSECTION - RenderingDeviceCommons::SHADER_STAGE_RAYGEN + 1;
	// GodotNext: d3d12_rt_pipelines (#121). The stages that have RaytracingStageDataD3D12 (by stage - SHADER_STAGE_RAYGEN):
	// ray generation through intersection, and no stage after them.
	static constexpr bool is_raytracing_stage(uint32_t p_stage) {
		return p_stage >= RenderingDeviceCommons::SHADER_STAGE_RAYGEN && p_stage <= RenderingDeviceCommons::SHADER_STAGE_INTERSECTION;
	}
	// GodotNext: d3d12_rt_pipelines (#121). Whether a shader's root signature starts with its push constants (root
	// constants at ROOT_CONSTANT_REGISTER): when a stage's DXIL reads them, and in every ray-tracing shader that declares
	// them. A ray-tracing pipeline uses its layout-defining shader's root signature as the global root signature of every
	// stage, including stages of other shaders, which may read push constants that this shader's stages do not.
	static bool has_root_constants(RenderingDeviceCommons::PipelineType p_pipeline_type, uint32_t p_push_constant_size, uint32_t p_dxil_push_constant_stages) {
		return p_dxil_push_constant_stages != 0 || (p_pipeline_type == RenderingDeviceCommons::PIPELINE_TYPE_RAYTRACING && p_push_constant_size > 0);
	}
	// GodotNext: dxil_toolchain (#123). The DXIL shader model ray query (inline ray tracing, tier 1.1) and the
	// ray-tracing library targets need.
	static constexpr uint32_t RAY_QUERY_SHADER_MODEL = 0x65; // D3D_SHADER_MODEL_6_5
	// GodotNext: rd_bindless (#176). A bindless table is its set's only binding, so its set has no dynamic
	// buffers, and the table takes register 0 of its set's space: register BINDLESS_TABLE_REGISTER in space
	// BINDLESS_TABLE_SPACE_BASE + set (see _remap_bindless_tables()).
	static constexpr uint32_t BINDLESS_TABLE_SPACE_BASE = DYNAMIC_BUFFER_SPACE_BASE;
	static constexpr uint32_t BINDLESS_TABLE_REGISTER = 0;
	// The size of a bindless table's runtime array in the DXIL, and of the descriptor range its set binds.
	// The driver allocates every table at this size, so the range never reads past the table's heap
	// allocation.
	static constexpr uint32_t BINDLESS_RESOURCE_RANGE_SIZE = 65536;
	static constexpr uint32_t BINDLESS_SAMPLER_RANGE_SIZE = 1024;
	// GodotNext: rd_indirect_count (#177). SV_StartVertexLocation and SV_StartInstanceLocation (extended command
	// information), which shaders compiled with base_instance_native read.
	static constexpr uint32_t BASE_INSTANCE_NATIVE_SHADER_MODEL = 0x68; // D3D_SHADER_MODEL_6_8
	// GodotNext: rd_mesh_shaders (#178). Whether task and mesh stages translate to DXIL (amplification and mesh
	// shaders). The Mesa fork's mesh patch turns it on (#122); until then SUPPORTS_MESH_SHADER is false on D3D12.
	static constexpr bool MESH_STAGES_SUPPORTED = false;
	static constexpr uint32_t SHADER_STAGES_BIT_OFFSET_INDICES[RenderingDeviceCommons::SHADER_STAGE_MAX] = {
		0, // SHADER_STAGE_VERTEX
		1, // SHADER_STAGE_FRAGMENT
		UINT32_MAX, // SHADER_STAGE_TESSELATION_CONTROL
		UINT32_MAX, // SHADER_STAGE_TESSELATION_EVALUATION
		2, // SHADER_STAGE_COMPUTE
		3, // SHADER_STAGE_RAYGEN
		4, // SHADER_STAGE_ANY_HIT
		5, // SHADER_STAGE_CLOSEST_HIT
		6, // SHADER_STAGE_MISS
		7, // SHADER_STAGE_INTERSECTION
		// GodotNext: rd_mesh_shaders (#178). No specialization-constant slot until the stages compile (#122): a 0 here
		// would patch the vertex stage's offsets.
		UINT32_MAX, // SHADER_STAGE_TASK
		UINT32_MAX, // SHADER_STAGE_MESH
	};

	struct ReflectionBindingSetDataD3D12 {
		uint32_t resource_root_param_idx = UINT32_MAX;
		uint32_t resource_descriptor_count = 0;
		uint32_t sampler_root_param_idx = UINT32_MAX;
		uint32_t sampler_descriptor_count = 0;
	};

	struct ReflectionBindingDataD3D12 {
		uint32_t resource_class = 0;
		uint32_t has_sampler = 0;
		uint32_t dxil_stages = 0;
		uint32_t resource_descriptor_offset = UINT32_MAX;
		uint32_t sampler_descriptor_offset = UINT32_MAX;
		uint32_t root_param_idx = UINT32_MAX; // Root descriptor only.
	};

	struct ReflectionSpecializationDataD3D12 {
		uint64_t stages_bit_offsets[D3D12_BITCODE_OFFSETS_NUM_STAGES] = {};
	};

	// GodotNext: d3d12_rt_pipelines (#121). What a ray-tracing stage's DXIL library takes and reads, for its state
	// object: the shader config's payload and attribute sizes, the hit group rules
	// (RDRTPipeline::get_hit_group_interface_error()) and the local root signature of the shader record. Sizes in bytes,
	// as DXIL lays the types out.
	enum RaytracingStageFlags : uint32_t {
		RAYTRACING_STAGE_PRESENT = 1 << 0,
		// The stage declares and uses its payload parameter (rayPayloadInEXT); without it, it gets a stand-in.
		RAYTRACING_STAGE_PAYLOAD_DECLARED = 1 << 1,
		// The stage declares and uses its attributes parameter (hitAttributeEXT); without it, it gets a stand-in.
		RAYTRACING_STAGE_ATTRIBUTES_DECLARED = 1 << 2,
	};
	struct RaytracingStageDataD3D12 {
		uint32_t flags = 0;
		// The largest ray payload the stage receives or passes to traceRayEXT (D3D12_RAYTRACING_SHADER_CONFIG).
		uint32_t payload_size = 0;
		// The same, measured as Vulkan measures it (RDRTPipeline::get_spirv_payload_size()), for the library limit.
		uint32_t spirv_payload_size = 0;
		// The payload parameter of an any-hit, closest-hit or miss stage, declared or stand-in.
		uint32_t incoming_payload_size = 0;
		// The attributes parameter of an any-hit or closest-hit stage (declared or stand-in), or the attributes an
		// intersection stage reports.
		uint32_t attribute_size = 0;
		// The shader record constant buffer the stage reads (a multiple of 16 bytes), 0 when it reads none.
		uint32_t record_size = 0;
	};

protected:
	struct ReflectionDataD3D12 {
		uint32_t spirv_specialization_constants_ids_mask = 0;
		uint32_t dxil_push_constant_stages = 0;
		uint32_t nir_runtime_data_root_param_idx = 0;
	};

	struct ContainerFooterD3D12 {
		uint32_t root_signature_length = 0;
		uint32_t root_signature_crc = 0;
	};

	void *lib_d3d12 = nullptr;
	ReflectionDataD3D12 reflection_data_d3d12;
	Vector<ReflectionBindingSetDataD3D12> reflection_binding_set_data_d3d12;
	Vector<ReflectionBindingDataD3D12> reflection_binding_set_uniforms_data_d3d12;
	Vector<ReflectionSpecializationDataD3D12> reflection_specialization_data_d3d12;
	Vector<uint8_t> root_signature_bytes;
	uint32_t root_signature_crc = 0;
	// GodotNext: rd_indirect_count (#177). Set from ReflectShader::base_instance_native for one compilation.
	bool base_instance_native = false;
	// GodotNext: d3d12_rt_pipelines (#121). By stage - SHADER_STAGE_RAYGEN; read from containers of format version 4 or
	// later (has_raytracing_stage_data).
	RaytracingStageDataD3D12 raytracing_stage_data_d3d12[RAYTRACING_STAGE_COUNT];
	bool has_raytracing_stage_data = false;

#if NIR_ENABLED
	// GodotNext: d3d12_rt_pipelines (#121)
	void _add_raytracing_stand_ins(const HashMap<int, nir_shader *> &p_stages_nir_shaders);
	void _remove_unused_resources(nir_shader *p_shader) const;
	void _remap_shader_record(nir_shader *p_shader, RenderingDeviceCommons::ShaderStage p_stage);

	bool _convert_spirv_to_nir(Span<ReflectShaderStage> p_spirv, const nir_shader_compiler_options *p_compiler_options, HashMap<int, nir_shader *> &r_stages_nir_shaders, Vector<RenderingDeviceCommons::ShaderStage> &r_stages, BitField<RenderingDeviceCommons::ShaderStage> &r_stages_processed);
	bool _convert_nir_to_dxil(const HashMap<int, nir_shader *> &p_stages_nir_shaders, BitField<RenderingDeviceCommons::ShaderStage> p_stages_processed, HashMap<RenderingDeviceCommons::ShaderStage, Vector<uint8_t>> &r_dxil_blobs);
	bool _convert_spirv_to_dxil(Span<ReflectShaderStage> p_spirv, HashMap<RenderingDeviceCommons::ShaderStage, Vector<uint8_t>> &r_dxil_blobs, Vector<RenderingDeviceCommons::ShaderStage> &r_stages, BitField<RenderingDeviceCommons::ShaderStage> &r_stages_processed);
	void _remap_dynamic_buffers(nir_shader *p_shader) const; // GodotNext: d3d12_gpu_based_validation (#272).
	void _remap_bindless_tables(nir_shader *p_shader) const; // GodotNext: rd_bindless (#176).
	bool _generate_root_signature(BitField<RenderingDeviceCommons::ShaderStage> p_stages_processed);

	// GodotNirCallbacks.
	static void _nir_report_resource(uint32_t p_register, uint32_t p_space, uint32_t p_dxil_type, void *p_data);
	static void _nir_report_sc_bit_offset(uint32_t p_sc_id, uint64_t p_bit_offset, void *p_data);
	static void _nir_report_bitcode_bit_offset(uint64_t p_bit_offset, void *p_data);
#endif

	// RenderingShaderContainer overrides.
	virtual uint32_t _format() const override;
	virtual uint32_t _format_version() const override;
	virtual uint32_t _from_bytes_reflection_extra_data(const uint8_t *p_bytes) override;
	virtual uint32_t _from_bytes_reflection_binding_uniform_extra_data_start(const uint8_t *p_bytes) override;
	virtual uint32_t _from_bytes_reflection_binding_uniform_extra_data(const uint8_t *p_bytes, uint32_t p_index) override;
	virtual uint32_t _from_bytes_reflection_specialization_extra_data_start(const uint8_t *p_bytes) override;
	virtual uint32_t _from_bytes_reflection_specialization_extra_data(const uint8_t *p_bytes, uint32_t p_index) override;
	virtual uint32_t _from_bytes_footer_extra_data(const uint8_t *p_bytes) override;
	virtual uint32_t _to_bytes_reflection_extra_data(uint8_t *p_bytes) const override;
	virtual uint32_t _to_bytes_reflection_binding_uniform_extra_data(uint8_t *p_bytes, uint32_t p_index) const override;
	virtual uint32_t _to_bytes_reflection_specialization_extra_data(uint8_t *p_bytes, uint32_t p_index) const override;
	virtual uint32_t _to_bytes_footer_extra_data(uint8_t *p_bytes) const override;
	virtual void _set_from_shader_reflection_post(const ReflectShader &p_shader) override;
	virtual bool _set_code_from_spirv(const ReflectShader &p_shader) override;

public:
	struct ShaderReflectionD3D12 {
		uint32_t spirv_specialization_constants_ids_mask = 0;
		uint32_t dxil_push_constant_stages = 0;
		uint32_t nir_runtime_data_root_param_idx = 0;
		Vector<ReflectionBindingSetDataD3D12> reflection_binding_sets_d3d12;
		Vector<Vector<ReflectionBindingDataD3D12>> reflection_binding_set_uniforms_d3d12;
		Vector<ReflectionSpecializationDataD3D12> reflection_specialization_data_d3d12;
		Vector<uint8_t> root_signature_bytes;
		uint32_t root_signature_crc = 0;
		// GodotNext: d3d12_rt_pipelines (#121)
		RaytracingStageDataD3D12 raytracing_stages[RAYTRACING_STAGE_COUNT];
		bool has_raytracing_stage_data = false;
	};

	RenderingShaderContainerD3D12();
	RenderingShaderContainerD3D12(void *p_lib_d3d12);
	ShaderReflectionD3D12 get_shader_reflection_d3d12() const;

	// GodotNext: dxil_toolchain (#123). Whether this build links the GodotNext Mesa NIR library (the build
	// found its godotnext-mesa.txt label), which compiles ray query and the ray-tracing stages. With the
	// upstream library those shaders are refused with one error. get_mesa_label() returns the label, or an
	// empty string.
	static bool is_raytracing_supported();
	static const char *get_mesa_label();
	// GodotNext: dxil_atomics (#279). Whether the linked library declares dx.op.atomicBinOp and
	// dx.op.atomicCompareExchange without readnone, as DXC does: the GodotNext Mesa NIR library 25.3.1-gn3 or later.
	// Older libraries declare them readnone, and D3D12 GPU-based validation then loses atomics whose result is
	// unused (#277).
	static bool declares_atomics_without_readnone();

	// GodotNext: dxil_toolchain (#123). The debug-only DXIL validator (GODOT_D3D12_DXIL_VALIDATE=1 in a build with
	// DEBUG_ENABLED). requested: the variable is set. active: dxil.dll is loaded; asking loads it when it was
	// requested. validated and failed: the blobs checked, and rejected, since the process started. Other builds
	// report all zeros.
	struct DXILValidationStats {
		bool requested = false;
		bool active = false;
		uint32_t validated = 0;
		uint32_t failed = 0;
	};
	static DXILValidationStats get_dxil_validation_stats();
	// Puts one DXIL container through the validator, as compiling a shader does before signing it, with one error
	// message if it is rejected. Returns false only when the validator rejects it. Does nothing unless validation
	// was requested. For tests.
	static bool validate_dxil(const String &p_shader_name, RenderingDeviceCommons::ShaderStage p_stage, const Vector<uint8_t> &p_dxil);
	// The text disassembly of one DXIL container, from IDxcCompiler::Disassemble in the Windows SDK's dxcompiler.dll
	// (through Mesa's dxil_validator wrapper), or an empty string with r_error set. It does not turn validation on.
	// Builds without DEBUG_ENABLED always return an empty string. For tests.
	static String disassemble_dxil(const Vector<uint8_t> &p_dxil, String *r_error = nullptr);
};

// GodotNext: rd_mesh_shaders (#178). The task and mesh entries of SHADER_STAGES_BIT_OFFSET_INDICES index
// stages_bit_offsets unchecked once the stages compile, so turning MESH_STAGES_SUPPORTED on (#122) needs real slots.
static_assert(!RenderingShaderContainerD3D12::MESH_STAGES_SUPPORTED ||
				(RenderingShaderContainerD3D12::SHADER_STAGES_BIT_OFFSET_INDICES[RenderingDeviceCommons::SHADER_STAGE_TASK] < uint32_t(D3D12_BITCODE_OFFSETS_NUM_STAGES) &&
						RenderingShaderContainerD3D12::SHADER_STAGES_BIT_OFFSET_INDICES[RenderingDeviceCommons::SHADER_STAGE_MESH] < uint32_t(D3D12_BITCODE_OFFSETS_NUM_STAGES)),
		"Task and mesh stages need specialization-constant bit-offset slots before MESH_STAGES_SUPPORTED is turned on.");

class RenderingShaderContainerFormatD3D12 : public RenderingShaderContainerFormat {
protected:
	void *lib_d3d12 = nullptr;

public:
	void set_lib_d3d12(void *p_lib_d3d12);
	virtual Ref<RenderingShaderContainer> create_container() const override;
	virtual ShaderLanguageVersion get_shader_language_version() const override;
	virtual ShaderSpirvVersion get_shader_spirv_version(ShaderStage p_stage) const override;
	// GodotNext: d3d12_dxr (#118). True only when the linked Mesa NIR library compiles ray query, so the
	// shader baker bakes the ray-query ShaderRD versions for D3D12 exactly when they can compile.
	virtual bool supports_acceleration_structures() const override;
	RenderingShaderContainerFormatD3D12();
	virtual ~RenderingShaderContainerFormatD3D12();
};
