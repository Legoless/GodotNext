/**************************************************************************/
/*  test_rd_rt_pipelines.cpp                                              */
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

TEST_FORCE_LINK(test_rd_rt_pipelines)

// RT pipelines for hit-shader material evaluation (#179, plan §21.1 C5) without a GPU: the shader record
// layout of the hit shader binding table, the link and indirect-trace rules, the scan for shader execution
// reordering, the render graph's handling of an indirect trace, and the clean failures of a device without
// the features. godotnext/testbed/tests/rd_parity/rt_pipeline.gd runs the pipelines on a GPU.

#ifdef RD_ENABLED

#include "core/error/error_macros.h"
#include "core/object/class_db.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_device_graph.h"
#include "servers/rendering/rendering_device_ray_query.h"
#include "servers/rendering/rendering_device_rt_pipeline.h"
#include "servers/rendering/rendering_shader_container.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_GLSLANG_ENABLED
#include "modules/glslang/shader_compile.h"
#endif

#include <thirdparty/spirv-headers/include/spirv/unified1/spirv.h>

namespace TestRDRTPipelines {

using RDG = RenderingDeviceGraph;

// Records the errors printed while it is alive.
struct RTErrorCapture {
	ErrorHandlerList handler;
	int count = 0;
	String last_message;

	RTErrorCapture() {
		handler.errfunc = _capture;
		handler.userdata = this;
		add_error_handler(&handler);
	}

	~RTErrorCapture() {
		remove_error_handler(&handler);
	}

	static void _capture(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
		RTErrorCapture *self = static_cast<RTErrorCapture *>(p_self);
		self->count++;
		self->last_message = String::utf8((p_message != nullptr && *p_message != 0) ? p_message : p_error);
	}
};

uint64_t feature_bit(RenderingDevice::Features p_feature) {
	return uint64_t(1) << p_feature;
}

// Assembles a SPIR-V module one instruction at a time.
class RTSpirvAssembler {
	LocalVector<uint32_t> words = { SpvMagicNumber, 0x00010400, 0, 0, 0 };
	uint32_t bound = 1;

public:
	uint32_t id() {
		return bound++;
	}

	void op(SpvOp p_opcode, std::initializer_list<uint32_t> p_operands) {
		words.push_back((uint32_t(p_operands.size() + 1) << SpvWordCountShift) | uint32_t(p_opcode));
		for (uint32_t operand : p_operands) {
			words.push_back(operand);
		}
	}

	// p_operands come first, then the nul-terminated string, padded to whole words.
	void op_string(SpvOp p_opcode, std::initializer_list<uint32_t> p_operands, const char *p_string) {
		LocalVector<uint32_t> string_words;
		const uint32_t length = strlen(p_string) + 1;
		string_words.resize((length + 3) / 4);
		memset(string_words.ptr(), 0, string_words.size() * sizeof(uint32_t));
		memcpy(string_words.ptr(), p_string, length - 1);
		words.push_back((uint32_t(p_operands.size() + string_words.size() + 1) << SpvWordCountShift) | uint32_t(p_opcode));
		for (uint32_t operand : p_operands) {
			words.push_back(operand);
		}
		for (uint32_t word : string_words) {
			words.push_back(word);
		}
	}

	Vector<uint8_t> bytes() {
		words[3] = bound;
		Vector<uint8_t> result;
		result.resize(words.size() * sizeof(uint32_t));
		memcpy(result.ptrw(), words.ptr(), result.size());
		return result;
	}
};

// An empty ray generation shader that declares the reordering capabilities in p_usage.
Vector<uint8_t> make_raygen_module(uint32_t p_usage) {
	RTSpirvAssembler a;
	const uint32_t main_id = a.id();
	const uint32_t void_type = a.id();
	const uint32_t function_type = a.id();
	const uint32_t label = a.id();
	a.op(SpvOpCapability, { SpvCapabilityRayTracingKHR });
	if (p_usage & RDRTPipeline::REORDER_USAGE_EXT) {
		a.op(SpvOpCapability, { SpvCapabilityShaderInvocationReorderEXT });
	}
	if (p_usage & RDRTPipeline::REORDER_USAGE_NV) {
		a.op(SpvOpCapability, { SpvCapabilityShaderInvocationReorderNV });
	}
	a.op_string(SpvOpExtension, {}, "SPV_KHR_ray_tracing");
	if (p_usage & RDRTPipeline::REORDER_USAGE_EXT) {
		a.op_string(SpvOpExtension, {}, "SPV_EXT_shader_invocation_reorder");
	}
	if (p_usage & RDRTPipeline::REORDER_USAGE_NV) {
		a.op_string(SpvOpExtension, {}, "SPV_NV_shader_invocation_reorder");
	}
	a.op(SpvOpMemoryModel, { SpvAddressingModelLogical, SpvMemoryModelGLSL450 });
	a.op_string(SpvOpEntryPoint, { SpvExecutionModelRayGenerationKHR, main_id }, "main");
	a.op(SpvOpTypeVoid, { void_type });
	a.op(SpvOpTypeFunction, { function_type, void_type });
	a.op(SpvOpFunction, { void_type, main_id, SpvFunctionControlMaskNone, function_type });
	a.op(SpvOpLabel, { label });
	a.op(SpvOpReturn, {});
	a.op(SpvOpFunctionEnd, {});
	return a.bytes();
}

// A miss shader with one variable in p_storage_class, of the type p_build declares and returns.
Vector<uint8_t> make_payload_module(SpvStorageClass p_storage_class, uint32_t (*p_build)(RTSpirvAssembler &)) {
	RTSpirvAssembler a;
	const uint32_t main_id = a.id();
	a.op(SpvOpCapability, { SpvCapabilityRayTracingKHR });
	a.op_string(SpvOpExtension, {}, "SPV_KHR_ray_tracing");
	a.op(SpvOpMemoryModel, { SpvAddressingModelLogical, SpvMemoryModelGLSL450 });
	a.op_string(SpvOpEntryPoint, { SpvExecutionModelMissKHR, main_id }, "main");
	const uint32_t type = p_build(a);
	const uint32_t pointer = a.id();
	const uint32_t variable = a.id();
	a.op(SpvOpTypePointer, { pointer, uint32_t(p_storage_class), type });
	a.op(SpvOpVariable, { pointer, variable, uint32_t(p_storage_class) });
	const uint32_t void_type = a.id();
	const uint32_t function_type = a.id();
	const uint32_t label = a.id();
	a.op(SpvOpTypeVoid, { void_type });
	a.op(SpvOpTypeFunction, { function_type, void_type });
	a.op(SpvOpFunction, { void_type, main_id, SpvFunctionControlMaskNone, function_type });
	a.op(SpvOpLabel, { label });
	a.op(SpvOpReturn, {});
	a.op(SpvOpFunctionEnd, {});
	return a.bytes();
}

uint32_t payload_size(SpvStorageClass p_storage_class, uint32_t (*p_build)(RTSpirvAssembler &)) {
	const Vector<uint8_t> module = make_payload_module(p_storage_class, p_build);
	return RDRTPipeline::get_spirv_payload_size(module.ptr(), module.size());
}

uint32_t type_uint(RTSpirvAssembler &a) {
	const uint32_t id = a.id();
	a.op(SpvOpTypeInt, { id, 32, 0 });
	return id;
}

uint32_t type_float(RTSpirvAssembler &a, uint32_t p_width = 32) {
	const uint32_t id = a.id();
	a.op(SpvOpTypeFloat, { id, p_width });
	return id;
}

uint32_t type_vector(RTSpirvAssembler &a, uint32_t p_component, uint32_t p_count) {
	const uint32_t id = a.id();
	a.op(SpvOpTypeVector, { id, p_component, p_count });
	return id;
}

uint32_t type_array(RTSpirvAssembler &a, uint32_t p_element, uint32_t p_length, bool p_specialization_constant = false) {
	const uint32_t length = a.id();
	a.op(p_specialization_constant ? SpvOpSpecConstant : SpvOpConstant, { type_uint(a), length, p_length });
	const uint32_t id = a.id();
	a.op(SpvOpTypeArray, { id, p_element, length });
	return id;
}

Vector<RenderingDeviceCommons::ShaderStageSPIRVData> raygen_stages(const Vector<uint8_t> &p_module) {
	RenderingDeviceCommons::ShaderStageSPIRVData stage;
	stage.shader_stage = RenderingDeviceCommons::SHADER_STAGE_RAYGEN;
	stage.spirv = p_module;
	Vector<RenderingDeviceCommons::ShaderStageSPIRVData> stages;
	stages.push_back(stage);
	return stages;
}

class RTTestContainer : public RenderingShaderContainer {
	GDSOFTCLASS(RTTestContainer, RenderingShaderContainer);

protected:
	virtual uint32_t _format() const override { return 0x50545452; }
	virtual uint32_t _format_version() const override { return 1; }
	virtual bool _set_code_from_spirv(const ReflectShader &p_shader) override { return true; }
};

RDRTPipeline::LinkPart library_part(uint32_t p_raygen, uint32_t p_miss) {
	RDRTPipeline::LinkPart part;
	part.library = true;
	part.raygen_count = p_raygen;
	part.miss_count = p_miss;
	part.max_trace_recursion_depth = 1;
	part.push_constant_size = 16;
	part.set_formats.push_back(7);
	part.set_formats.push_back(9);
	return part;
}

// A compute list that binds p_table (when given) and uses p_tracker with p_usage (when given).
void add_compute(RDG &p_graph, RDG::BindlessTableTracker *p_table, RDG::ResourceTracker *p_tracker = nullptr, RDG::ResourceUsage p_usage = RDG::RESOURCE_USAGE_NONE) {
	p_graph.add_compute_list_begin();
	p_graph.add_compute_list_bind_pipeline(RDD::PipelineID());
	if (p_table != nullptr) {
		p_graph.add_compute_list_bindless_table(p_table);
	}
	if (p_tracker != nullptr) {
		p_graph.add_compute_list_usage(p_tracker, p_usage);
	}
	p_graph.add_compute_list_dispatch(1, 1, 1);
	p_graph.add_compute_list_end();
}

// Whether command p_command_index has a barrier that makes buffer p_buffer readable by shaders.
bool has_shader_read_barrier(const RDG &p_graph, int32_t p_command_index, uint64_t p_buffer) {
	LocalVector<RDD::TextureBarrier> textures;
	LocalVector<RDD::BufferBarrier> buffers;
	RDD::MemoryAccessBarrier memory;
	p_graph.command_barriers_for_test(p_command_index, textures, buffers, memory);
	for (const RDD::BufferBarrier &barrier : buffers) {
		if (barrier.buffer.id == p_buffer && barrier.dst_access.has_flag(RDD::BARRIER_ACCESS_SHADER_READ_BIT)) {
			return true;
		}
	}
	return false;
}

} // namespace TestRDRTPipelines

using namespace TestRDRTPipelines;

TEST_CASE("[RD][RTPipeline] A hit group entry is the handle, then the record, aligned to the handle alignment") {
	// No record: the aligned handle, as before records existed.
	CHECK_EQ(RDRTPipeline::get_hit_group_stride(32, 32, 0), 32u);
	CHECK_EQ(RDRTPipeline::get_hit_group_stride(32, 64, 0), 64u);
	// NVIDIA's 32-byte handles: an 8-byte record and the largest record both take one more 32-byte step.
	CHECK_EQ(RDRTPipeline::get_hit_group_stride(32, 32, 8), 64u);
	CHECK_EQ(RDRTPipeline::get_hit_group_stride(32, 32, RDRTPipeline::MAX_HIT_GROUP_RECORD_SIZE), 64u);
	// A 16-byte alignment packs an 8-byte record into 48 bytes.
	CHECK_EQ(RDRTPipeline::get_hit_group_stride(32, 16, 8), 48u);
	CHECK_EQ(RDRTPipeline::get_hit_group_stride(32, 16, 1), 48u);
	// The worst case stays far below Vulkan's smallest maxShaderGroupStride (4096).
	CHECK(RDRTPipeline::get_hit_group_stride(32, 64, RDRTPipeline::MAX_HIT_GROUP_RECORD_SIZE) <= 4096u);
}

TEST_CASE("[RD][RTPipeline] Each table entry gets its hit group's record after the handle, and zeros after it") {
	const uint32_t handle_size = 32;
	const uint32_t record_size = 8;
	const uint32_t stride = RDRTPipeline::get_hit_group_stride(handle_size, 32, record_size);
	REQUIRE_EQ(stride, 64u);

	// Two hit groups' records, MAX_HIT_GROUP_RECORD_SIZE bytes each.
	uint8_t records[2 * RDRTPipeline::MAX_HIT_GROUP_RECORD_SIZE];
	for (uint32_t i = 0; i < sizeof(records); i++) {
		records[i] = uint8_t(0x40 + i);
	}
	// Three entries whose handles the driver already wrote (0xAB), with stale bytes (0xCD) after them.
	uint8_t table[3 * 64];
	memset(table, 0xCD, sizeof(table));
	for (uint32_t e = 0; e < 3; e++) {
		memset(table + e * stride, 0xAB, handle_size);
	}
	// Entry 2 names a hit group the pipeline does not have: it gets a zero record.
	const uint32_t indices[3] = { 1, 0, 5 };
	RDRTPipeline::write_hit_group_records(table, stride, handle_size, record_size, indices, 3, records, 2);

	for (uint32_t e = 0; e < 3; e++) {
		const uint8_t *entry = table + e * stride;
		for (uint32_t b = 0; b < handle_size; b++) {
			CHECK_EQ(entry[b], 0xAB);
		}
		for (uint32_t b = 0; b < record_size; b++) {
			const uint8_t expected = indices[e] < 2 ? records[indices[e] * RDRTPipeline::MAX_HIT_GROUP_RECORD_SIZE + b] : 0;
			CHECK_EQ(entry[handle_size + b], expected);
		}
		for (uint32_t b = handle_size + record_size; b < stride; b++) {
			CHECK_EQ(entry[b], 0);
		}
	}
	CHECK_EQ(table[64 + 32], records[0]); // Entry 1 holds hit group 0's first byte.
	CHECK_EQ(table[32], records[RDRTPipeline::MAX_HIT_GROUP_RECORD_SIZE]); // Entry 0 holds hit group 1's.
}

TEST_CASE("[RD][RTPipeline] The SPIR-V scan finds both reordering capabilities") {
	CHECK_EQ(RDRTPipeline::get_spirv_reorder_usage(make_raygen_module(0).ptr(), make_raygen_module(0).size()), 0u);
	const Vector<uint8_t> ext = make_raygen_module(RDRTPipeline::REORDER_USAGE_EXT);
	CHECK_EQ(RDRTPipeline::get_spirv_reorder_usage(ext.ptr(), ext.size()), uint32_t(RDRTPipeline::REORDER_USAGE_EXT));
	const Vector<uint8_t> nv = make_raygen_module(RDRTPipeline::REORDER_USAGE_NV);
	CHECK_EQ(RDRTPipeline::get_spirv_reorder_usage(nv.ptr(), nv.size()), uint32_t(RDRTPipeline::REORDER_USAGE_NV));
	const Vector<uint8_t> both = make_raygen_module(RDRTPipeline::REORDER_USAGE_EXT | RDRTPipeline::REORDER_USAGE_NV);
	CHECK_EQ(RDRTPipeline::get_spirv_reorder_usage(both.ptr(), both.size()), uint32_t(RDRTPipeline::REORDER_USAGE_EXT | RDRTPipeline::REORDER_USAGE_NV));

	// Not a module, or one cut short in its header: nothing.
	CHECK_EQ(RDRTPipeline::get_spirv_reorder_usage(nullptr, 0), 0u);
	CHECK_EQ(RDRTPipeline::get_spirv_reorder_usage(nv.ptr(), 12), 0u);
	Vector<uint8_t> not_spirv = nv;
	not_spirv.write[0] ^= 0xFF;
	CHECK_EQ(RDRTPipeline::get_spirv_reorder_usage(not_spirv.ptr(), not_spirv.size()), 0u);

	// The error names the shader and the extension it lacks.
	const String error = RDRTPipeline::get_missing_reorder_error("hit lighting", RDRTPipeline::REORDER_USAGE_NV);
	CHECK(error.contains("'hit lighting'"));
	CHECK(error.contains("GL_NV_shader_invocation_reorder"));
	CHECK_FALSE(error.contains("GL_EXT_shader_invocation_reorder"));
	CHECK(error.contains("SUPPORTS_RAYTRACING_REORDER"));
}

TEST_CASE("[RD][RTPipeline] Shader containers keep the reordering they were compiled with") {
	Ref<RTTestContainer> container;
	container.instantiate();
	REQUIRE(container->set_code_from_spirv("reorder", raygen_stages(make_raygen_module(RDRTPipeline::REORDER_USAGE_NV))));
	CHECK_EQ(container->get_raytracing_reorder_usage(), uint32_t(RDRTPipeline::REORDER_USAGE_NV));

	// The bytes of a shader cache: the usage survives them, for shader_create_from_bytecode(). Container version 5
	// keeps it in ReflectionData's word at offset 72, after the 20-byte container header (this container adds no
	// header data), followed by a zero padding word.
	const PackedByteArray bytes = container->to_bytes();
	REQUIRE(bytes.size() >= 20 + 80);
	uint32_t words[3] = {};
	memcpy(&words[0], bytes.ptr() + 4, sizeof(uint32_t));
	memcpy(&words[1], bytes.ptr() + 20 + 72, sizeof(uint32_t));
	memcpy(&words[2], bytes.ptr() + 20 + 76, sizeof(uint32_t));
	CHECK_EQ(words[0], 5u);
	CHECK_EQ(words[1], uint32_t(RDRTPipeline::REORDER_USAGE_NV));
	CHECK_EQ(words[2], 0u);
	Ref<RTTestContainer> loaded;
	loaded.instantiate();
	REQUIRE(loaded->from_bytes(bytes));
	CHECK_EQ(loaded->get_raytracing_reorder_usage(), uint32_t(RDRTPipeline::REORDER_USAGE_NV));

	Ref<RTTestContainer> plain;
	plain.instantiate();
	REQUIRE(plain->set_code_from_spirv("plain", raygen_stages(make_raygen_module(0))));
	CHECK_EQ(plain->get_raytracing_reorder_usage(), 0u);
}

// The shader compiler is the glslang module, so this case exists only in builds that have it, like a
// module's own tests. The minimal template disables every module.
#ifdef MODULE_GLSLANG_ENABLED
TEST_CASE("[RD][RTPipeline] Reordering ray generation shaders from glslang reflect with their extension") {
	// The fixture's reordering ray generation shader, in both GLSL extensions, with the targets the Vulkan
	// container uses for ray tracing stages (Vulkan 1.1 GLSL, SPIR-V 1.4).
	const char *variants[2][3] = {
		{ "GL_EXT_shader_invocation_reorder", "hitObjectEXT", "EXT" },
		{ "GL_NV_shader_invocation_reorder", "hitObjectNV", "NV" },
	};
	const uint32_t usages[2] = { RDRTPipeline::REORDER_USAGE_EXT, RDRTPipeline::REORDER_USAGE_NV };
	for (int v = 0; v < 2; v++) {
		const String suffix = variants[v][2];
		const String source = String("#version 460\n#extension GL_EXT_ray_tracing : require\n#extension ") + variants[v][0] + " : require\n" +
				"layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;\n" +
				"layout(location = 0) rayPayloadEXT uvec4 payload;\n" +
				"void main() {\n" +
				"\t" + variants[v][1] + " hit;\n" +
				"\thitObjectTraceRay" + suffix + "(hit, tlas, gl_RayFlagsNoneEXT, 0xFF, 0, 1, 0, vec3(0.0), 0.0, vec3(0.0, 0.0, -1.0), 10.0, 0);\n" +
				"\treorderThread" + suffix + "(hit);\n" +
				"\thitObjectExecuteShader" + suffix + "(hit, 0);\n" +
				"}\n";
		String error;
		const Vector<uint8_t> spirv = compile_glslang_shader(RenderingDeviceCommons::SHADER_STAGE_RAYGEN, source, RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_4, &error);
		if (spirv.is_empty()) {
			FAIL_CHECK(("glslang failed to compile the " + suffix + " ray generation shader:\n" + error).utf8().get_data());
			continue;
		}
		CHECK_EQ(RDRTPipeline::get_spirv_reorder_usage(spirv.ptr(), spirv.size()), usages[v]);

		Ref<RTTestContainer> container;
		container.instantiate();
		RTErrorCapture errors;
		ERR_PRINT_OFF;
		const bool reflected = container->set_code_from_spirv("reorder " + suffix, raygen_stages(spirv));
		ERR_PRINT_ON;
		CHECK_MESSAGE(reflected, String("the " + suffix + " shader reflects: " + errors.last_message));
		CHECK_EQ(errors.count, 0);
		CHECK_EQ(container->get_raytracing_reorder_usage(), usages[v]);
		CHECK_EQ(RDRTPipeline::get_spirv_payload_size(spirv.ptr(), spirv.size()), 16u);
	}
}

TEST_CASE("[RD][RTPipeline] Payloads of glslang modules measure as declared") {
	struct Case {
		RenderingDeviceCommons::ShaderStage stage;
		const char *declaration;
		uint32_t size;
	};
	const Case cases[] = {
		{ RenderingDeviceCommons::SHADER_STAGE_MISS, "layout(location = 0) rayPayloadInEXT uvec4 payload;\nvoid main() { payload = uvec4(1u); }\n", 16 },
		{ RenderingDeviceCommons::SHADER_STAGE_CLOSEST_HIT, "struct Payload { vec3 color; float t; uint ids[3]; };\nlayout(location = 0) rayPayloadInEXT Payload payload;\nvoid main() { payload.t = gl_HitTEXT; }\n", 28 },
		{ RenderingDeviceCommons::SHADER_STAGE_CLOSEST_HIT, "struct Payload { uvec4 data[5]; };\nlayout(location = 0) rayPayloadInEXT Payload payload;\nvoid main() { payload.data[4] = uvec4(gl_PrimitiveID); }\n", 80 },
	};
	for (const Case &c : cases) {
		const String source = String("#version 460\n#extension GL_EXT_ray_tracing : require\n") + c.declaration;
		String error;
		const Vector<uint8_t> spirv = compile_glslang_shader(c.stage, source, RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_4, &error);
		if (spirv.is_empty()) {
			FAIL_CHECK(("glslang failed to compile:\n" + source + "\n" + error).utf8().get_data());
			continue;
		}
		CHECK_MESSAGE(RDRTPipeline::get_spirv_payload_size(spirv.ptr(), spirv.size()) == c.size, c.declaration);
	}
}
#endif // MODULE_GLSLANG_ENABLED

TEST_CASE("[RD][RTPipeline] A ray payload is measured with every scalar aligned to the largest one") {
	// The fixture's payload.
	CHECK_EQ(payload_size(SpvStorageClassRayPayloadKHR, [](RTSpirvAssembler &a) { return type_vector(a, type_uint(a), 4); }), 16u);
	CHECK_EQ(payload_size(SpvStorageClassIncomingRayPayloadKHR, [](RTSpirvAssembler &a) { return type_vector(a, type_uint(a), 4); }), 16u);
	// struct { vec4; uvec4; float[3]; }: 32-bit scalars only, so the sum of their sizes.
	CHECK_EQ(payload_size(SpvStorageClassRayPayloadKHR, [](RTSpirvAssembler &a) {
		const uint32_t f = type_float(a);
		const uint32_t v = type_vector(a, f, 4);
		const uint32_t u = type_vector(a, type_uint(a), 4);
		const uint32_t arr = type_array(a, f, 3);
		const uint32_t id = a.id();
		a.op(SpvOpTypeStruct, { id, v, u, arr });
		return id;
	}),
			44u);
	// mat3: three vec3 columns.
	CHECK_EQ(payload_size(SpvStorageClassRayPayloadKHR, [](RTSpirvAssembler &a) {
		const uint32_t column = type_vector(a, type_float(a), 3);
		const uint32_t id = a.id();
		a.op(SpvOpTypeMatrix, { id, column, 3 });
		return id;
	}),
			36u);
	// struct { double; float; }: the float starts 8 bytes in and ends the payload at 12.
	CHECK_EQ(payload_size(SpvStorageClassRayPayloadKHR, [](RTSpirvAssembler &a) {
		const uint32_t d = type_float(a, 64);
		const uint32_t f = type_float(a);
		const uint32_t id = a.id();
		a.op(SpvOpTypeStruct, { id, d, f });
		return id;
	}),
			12u);
	// struct { float; double; }: the float is padded to 8 bytes.
	CHECK_EQ(payload_size(SpvStorageClassRayPayloadKHR, [](RTSpirvAssembler &a) {
		const uint32_t f = type_float(a);
		const uint32_t d = type_float(a, 64);
		const uint32_t id = a.id();
		a.op(SpvOpTypeStruct, { id, f, d });
		return id;
	}),
			16u);
	// A buffer address is an 8-byte scalar; a bool counts as 4 bytes.
	CHECK_EQ(payload_size(SpvStorageClassRayPayloadKHR, [](RTSpirvAssembler &a) {
		const uint32_t u = type_uint(a);
		const uint32_t address = a.id();
		a.op(SpvOpTypePointer, { address, SpvStorageClassPhysicalStorageBuffer, u });
		const uint32_t id = a.id();
		a.op(SpvOpTypeStruct, { id, u, address });
		return id;
	}),
			16u);
	CHECK_EQ(payload_size(SpvStorageClassRayPayloadKHR, [](RTSpirvAssembler &a) {
		const uint32_t id = a.id();
		a.op(SpvOpTypeBool, { id });
		return id;
	}),
			4u);
	// Five uvec4: over the 64 bytes a library holds.
	CHECK_EQ(payload_size(SpvStorageClassIncomingRayPayloadKHR, [](RTSpirvAssembler &a) { return type_array(a, type_vector(a, type_uint(a), 4), 5); }), 80u);
	// An empty struct, and a variable that is not a payload.
	CHECK_EQ(payload_size(SpvStorageClassRayPayloadKHR, [](RTSpirvAssembler &a) {
		const uint32_t id = a.id();
		a.op(SpvOpTypeStruct, { id });
		return id;
	}),
			0u);
	CHECK_EQ(payload_size(SpvStorageClassPrivate, [](RTSpirvAssembler &a) { return type_array(a, type_vector(a, type_uint(a), 4), 5); }), 0u);
	// No fixed size: a length from a specialization constant, or a runtime array.
	CHECK_EQ(payload_size(SpvStorageClassRayPayloadKHR, [](RTSpirvAssembler &a) { return type_array(a, type_uint(a), 4, true); }), RDRTPipeline::PAYLOAD_SIZE_UNKNOWN);
	CHECK_EQ(payload_size(SpvStorageClassRayPayloadKHR, [](RTSpirvAssembler &a) {
		const uint32_t u = type_uint(a);
		const uint32_t id = a.id();
		a.op(SpvOpTypeRuntimeArray, { id, u });
		return id;
	}),
			RDRTPipeline::PAYLOAD_SIZE_UNKNOWN);
	// Not a module: nothing.
	CHECK_EQ(RDRTPipeline::get_spirv_payload_size(nullptr, 0), 0u);
	const Vector<uint8_t> module = make_payload_module(SpvStorageClassRayPayloadKHR, [](RTSpirvAssembler &a) { return type_uint(a); });
	CHECK_EQ(RDRTPipeline::get_spirv_payload_size(module.ptr(), module.size()), 4u);
	CHECK_EQ(RDRTPipeline::get_spirv_payload_size(module.ptr(), 12), 0u);

	const String error = RDRTPipeline::get_library_payload_error("material 7", RDD::SHADER_STAGE_CLOSEST_HIT, 80);
	CHECK(error.contains("'material 7'"));
	CHECK(error.contains("80-byte ray payload in its closest-hit stage"));
	CHECK(error.contains(itos(RDRTPipeline::LIBRARY_MAX_PAYLOAD_SIZE)));
	const String unknown = RDRTPipeline::get_library_payload_error("", RDD::SHADER_STAGE_MISS, RDRTPipeline::PAYLOAD_SIZE_UNKNOWN);
	CHECK(unknown.contains("not fixed"));
	CHECK(unknown.contains("miss stage"));
}

TEST_CASE("[RD][RTPipeline] Link rules for the base and each library") {
	const RDRTPipeline::LinkPart base = library_part(1, 1);
	const RDRTPipeline::LinkPart hits = library_part(0, 0);

	SUBCASE("A library with ray generation shaders, or a linked pipeline, is a base") {
		CHECK_EQ(RDRTPipeline::get_link_base_status(base), RDRTPipeline::LINK_OK);
		RDRTPipeline::LinkPart linked = base;
		linked.library = false;
		linked.linked = true;
		CHECK_EQ(RDRTPipeline::get_link_base_status(linked), RDRTPipeline::LINK_OK);
	}
	SUBCASE("A plain pipeline, or a library without ray generation shaders, is not") {
		RDRTPipeline::LinkPart plain = base;
		plain.library = false;
		CHECK_EQ(RDRTPipeline::get_link_base_status(plain), RDRTPipeline::LINK_BASE_NOT_LIBRARY);
		CHECK_EQ(RDRTPipeline::get_link_base_status(hits), RDRTPipeline::LINK_BASE_NO_RAYGEN);
	}
	SUBCASE("A library of hit groups with the base's depth and layout links") {
		CHECK_EQ(RDRTPipeline::get_link_library_status(base, hits), RDRTPipeline::LINK_OK);
	}
	SUBCASE("Each broken rule has its own status") {
		RDRTPipeline::LinkPart not_library = hits;
		not_library.library = false;
		CHECK_EQ(RDRTPipeline::get_link_library_status(base, not_library), RDRTPipeline::LINK_LIBRARY_NOT_LIBRARY);
		CHECK_EQ(RDRTPipeline::get_link_library_status(base, library_part(1, 0)), RDRTPipeline::LINK_LIBRARY_HAS_RAYGEN_OR_MISS);
		CHECK_EQ(RDRTPipeline::get_link_library_status(base, library_part(0, 1)), RDRTPipeline::LINK_LIBRARY_HAS_RAYGEN_OR_MISS);
		RDRTPipeline::LinkPart deeper = hits;
		deeper.max_trace_recursion_depth = 2;
		CHECK_EQ(RDRTPipeline::get_link_library_status(base, deeper), RDRTPipeline::LINK_RECURSION_MISMATCH);
		RDRTPipeline::LinkPart other_set = hits;
		other_set.set_formats.write[1] = 10;
		CHECK_EQ(RDRTPipeline::get_link_library_status(base, other_set), RDRTPipeline::LINK_LAYOUT_MISMATCH);
		RDRTPipeline::LinkPart fewer_sets = hits;
		fewer_sets.set_formats.resize(1);
		CHECK_EQ(RDRTPipeline::get_link_library_status(base, fewer_sets), RDRTPipeline::LINK_LAYOUT_MISMATCH);
		RDRTPipeline::LinkPart other_push = hits;
		other_push.push_constant_size = 32;
		CHECK_EQ(RDRTPipeline::get_link_library_status(base, other_push), RDRTPipeline::LINK_LAYOUT_MISMATCH);
		// Set formats leave immutable samplers out, but the driver's set layouts hold them.
		RDRTPipeline::LinkPart other_samplers = hits;
		other_samplers.immutable_sampler_hash = 0x5A3D;
		CHECK_EQ(RDRTPipeline::get_link_library_status(base, other_samplers), RDRTPipeline::LINK_LAYOUT_MISMATCH);
		RDRTPipeline::LinkPart same_samplers = base;
		same_samplers.immutable_sampler_hash = 0x5A3D;
		CHECK_EQ(RDRTPipeline::get_link_library_status(same_samplers, other_samplers), RDRTPipeline::LINK_OK);
		CHECK(String(RDRTPipeline::get_link_error(RDRTPipeline::LINK_LAYOUT_MISMATCH)).contains("immutable samplers"));
	}
	SUBCASE("Every status but OK has an error") {
		for (int status = RDRTPipeline::LINK_BASE_NOT_LIBRARY; status <= RDRTPipeline::LINK_DUPLICATE_LIBRARY; status++) {
			CHECK_FALSE(String(RDRTPipeline::get_link_error(RDRTPipeline::LinkStatus(status))).is_empty());
		}
		CHECK(String(RDRTPipeline::get_link_error(RDRTPipeline::LINK_OK)).is_empty());
	}
}

TEST_CASE("[RD][RTPipeline] An indirect trace reads 12 bytes at a 4-byte offset of an indirect, addressable buffer") {
	BitField<RDD::BufferUsageBits> usage = RDD::BUFFER_USAGE_STORAGE_BIT;
	CHECK_EQ(RDRTPipeline::get_indirect_status(usage, 0, 64), RDRTPipeline::INDIRECT_NO_INDIRECT_USAGE);
	usage.set_flag(RDD::BUFFER_USAGE_INDIRECT_BIT);
	CHECK_EQ(RDRTPipeline::get_indirect_status(usage, 0, 64), RDRTPipeline::INDIRECT_NO_DEVICE_ADDRESS);
	usage.set_flag(RDD::BUFFER_USAGE_DEVICE_ADDRESS_BIT);
	CHECK_EQ(RDRTPipeline::get_indirect_status(usage, 0, 64), RDRTPipeline::INDIRECT_OK);
	CHECK_EQ(RDRTPipeline::get_indirect_status(usage, 52, 64), RDRTPipeline::INDIRECT_OK);
	CHECK_EQ(RDRTPipeline::get_indirect_status(usage, 2, 64), RDRTPipeline::INDIRECT_MISALIGNED);
	CHECK_EQ(RDRTPipeline::get_indirect_status(usage, 56, 64), RDRTPipeline::INDIRECT_OUT_OF_RANGE);
	CHECK_EQ(RDRTPipeline::get_indirect_status(usage, 128, 64), RDRTPipeline::INDIRECT_OUT_OF_RANGE);
	CHECK_EQ(RDRTPipeline::get_indirect_status(usage, 0, 8), RDRTPipeline::INDIRECT_OUT_OF_RANGE);
	for (int status = RDRTPipeline::INDIRECT_NO_INDIRECT_USAGE; status <= RDRTPipeline::INDIRECT_OUT_OF_RANGE; status++) {
		CHECK_FALSE(String(RDRTPipeline::get_indirect_error(RDRTPipeline::IndirectStatus(status))).is_empty());
	}
}

TEST_CASE("[RD][RTPipeline] An indirect trace waits on the compute pass that wrote its launch size") {
	RDG graph;
	graph.set_driver_traits_for_test(true, true);
	RDG::ResourceTracker *arguments = RDG::resource_tracker_create();
	arguments->buffer_driver_id = RDD::BufferID(51);
	const RDD::ShaderBindingTable table;

	graph.begin();
	// 0: a compute list writes the launch size.
	graph.add_compute_list_begin();
	graph.add_compute_list_bind_pipeline(RDD::PipelineID());
	graph.add_compute_list_usage(arguments, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE);
	graph.add_compute_list_dispatch(1, 1, 1);
	graph.add_compute_list_end();
	// 1: an indirect trace reads it.
	graph.add_raytracing_list_begin();
	graph.add_raytracing_list_bind_pipeline(RDD::RaytracingPipelineID());
	CHECK(graph.add_raytracing_list_trace_rays_indirect(table, table, table, RDD::BufferID(51), arguments, 0));
	graph.add_raytracing_list_end();
	CHECK_EQ(arguments->usage, RDG::RESOURCE_USAGE_INDIRECT_BUFFER_READ);
	// 2: a direct trace, for comparison.
	graph.add_raytracing_list_begin();
	graph.add_raytracing_list_bind_pipeline(RDD::RaytracingPipelineID());
	graph.add_raytracing_list_trace_rays(table, table, table, 1, 1, 1);
	graph.add_raytracing_list_end();

	LocalVector<RDD::TextureBarrier> texture_barriers;
	LocalVector<RDD::BufferBarrier> buffer_barriers;
	RDD::MemoryAccessBarrier memory_barrier;
	graph.command_barriers_for_test(1, texture_barriers, buffer_barriers, memory_barrier);
	REQUIRE_EQ(buffer_barriers.size(), 1u);
	CHECK_EQ(buffer_barriers[0].buffer.id, 51u);
	CHECK(buffer_barriers[0].src_access.has_flag(RDD::BARRIER_ACCESS_SHADER_WRITE_BIT));
	CHECK(buffer_barriers[0].dst_access.has_flag(RDD::BARRIER_ACCESS_INDIRECT_COMMAND_READ_BIT));

	// The indirect list runs at the stage that reads the launch size, after the compute stage.
	BitField<RDD::PipelineStageBits> self;
	BitField<RDD::PipelineStageBits> previous;
	BitField<RDD::PipelineStageBits> next;
	graph.command_get_stages(1, self, previous, next);
	CHECK(self.has_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT));
	CHECK(self.has_flag(RDD::PIPELINE_STAGE_RAY_TRACING_SHADER_BIT));
	CHECK(previous.has_flag(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT));
	graph.command_get_stages(2, self, previous, next);
	CHECK_FALSE(self.has_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT));

	LocalVector<int32_t> order;
	RDG::AsyncPlan plan;
	graph.plan_for_test(order, plan, false);
	int32_t compute_position = -1;
	int32_t trace_position = -1;
	for (uint32_t i = 0; i < order.size(); i++) {
		compute_position = order[i] == 0 ? int32_t(i) : compute_position;
		trace_position = order[i] == 1 ? int32_t(i) : trace_position;
	}
	CHECK(trace_position > compute_position);

	RDG::resource_tracker_free(arguments);
}

TEST_CASE("[RD][RTPipeline] An indirect trace whose launch-size buffer the list also uses another way gives one named error") {
	// A tracker keeps the first usage it gets in a raytracing list, so the launch size must not meet a uniform
	// set's read of the same buffer there.
	RDG graph;
	graph.set_driver_traits_for_test(true, true);
	RDG::ResourceTracker *launch = RDG::resource_tracker_create();
	launch->buffer_driver_id = RDD::BufferID(51);
	const RDD::ShaderBindingTable table;
	graph.begin();
	RTErrorCapture errors;

	SUBCASE("The buffer bound in a uniform set: the trace is refused and records nothing") {
		graph.add_raytracing_list_begin();
		graph.add_raytracing_list_bind_pipeline(RDD::RaytracingPipelineID());
		graph.add_raytracing_list_usage(launch, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ); // As a bound uniform set adds it.
		ERR_PRINT_OFF;
		CHECK_FALSE(graph.add_raytracing_list_trace_rays_indirect(table, table, table, RDD::BufferID(51), launch, 0));
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("The launch-size buffer of an indirect trace is also used in this raytracing list (Storage Buffer Read). A buffer has one usage per raytracing list, so it would not be synchronized for the indirect read; bind it in none of this raytracing list's uniform sets."));
		graph.add_raytracing_list_end(); // 0
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(launch->raytracing_list_usage, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
		BitField<RDD::PipelineStageBits> self;
		BitField<RDD::PipelineStageBits> previous;
		BitField<RDD::PipelineStageBits> next;
		graph.command_get_stages(0, self, previous, next);
		CHECK_FALSE(self.has_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT));
	}
	SUBCASE("The same buffer traced twice in one list") {
		graph.add_raytracing_list_begin();
		graph.add_raytracing_list_bind_pipeline(RDD::RaytracingPipelineID());
		CHECK(graph.add_raytracing_list_trace_rays_indirect(table, table, table, RDD::BufferID(51), launch, 0));
		CHECK(graph.add_raytracing_list_trace_rays_indirect(table, table, table, RDD::BufferID(51), launch, 12));
		graph.add_raytracing_list_end();
		CHECK_EQ(errors.count, 0);
		CHECK_EQ(launch->usage, RDG::RESOURCE_USAGE_INDIRECT_BUFFER_READ);
	}
	SUBCASE("The buffer read from a uniform set in an earlier list of the frame: the trace is recorded") {
		graph.add_raytracing_list_begin();
		graph.add_raytracing_list_bind_pipeline(RDD::RaytracingPipelineID());
		graph.add_raytracing_list_usage(launch, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
		graph.add_raytracing_list_trace_rays(table, table, table, 1, 1, 1);
		graph.add_raytracing_list_end(); // 0
		graph.add_raytracing_list_begin();
		graph.add_raytracing_list_bind_pipeline(RDD::RaytracingPipelineID());
		CHECK(graph.add_raytracing_list_trace_rays_indirect(table, table, table, RDD::BufferID(51), launch, 0));
		graph.add_raytracing_list_end(); // 1
		CHECK_EQ(errors.count, 0);
		CHECK_EQ(launch->usage, RDG::RESOURCE_USAGE_INDIRECT_BUFFER_READ);
	}

	RDG::resource_tracker_free(launch);
}

TEST_CASE("[RD][RTPipeline] An indirect trace's launch-size buffer that a bound bindless table has not read since a write gives one named error") {
	// A storage-buffer table holds the launch-size buffer. The table's read in a raytracing list becomes a usage
	// only at the list's end, and a buffer has one usage per list, so it must not meet the indirect read there.
	RDG graph;
	graph.set_driver_traits_for_test(true, true);
	RDG::BindlessTableTracker *buffers = RDG::bindless_table_tracker_create(RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
	RDG::ResourceTracker *launch = RDG::resource_tracker_create();
	launch->buffer_driver_id = RDD::BufferID(51);
	RDG::bindless_table_add_resident(buffers, launch);
	const RDD::ShaderBindingTable table;

	// A first bind of the table makes the buffer ready.
	graph.begin();
	add_compute(graph, buffers);
	REQUIRE(buffers->dirty.is_empty());
	graph.begin();
	RTErrorCapture errors;

	SUBCASE("The table bound before the trace: the trace is refused, and the table's read gets its barrier") {
		add_compute(graph, nullptr, launch, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE); // 0: writes the launch size.
		REQUIRE_EQ(buffers->dirty.size(), 1u);
		graph.add_raytracing_list_begin();
		graph.add_raytracing_list_bind_pipeline(RDD::RaytracingPipelineID());
		graph.add_raytracing_list_bindless_table(buffers);
		ERR_PRINT_OFF;
		CHECK_FALSE(graph.add_raytracing_list_trace_rays_indirect(table, table, table, RDD::BufferID(51), launch, 0));
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("The launch-size buffer of an indirect trace is in a bindless table that this raytracing list binds, and the table has not read it since it was last written. A buffer has one usage per raytracing list, so the table's read would not be synchronized; bind the table in an earlier list after the write, or keep the buffer out of the table."));
		graph.add_raytracing_list_trace_rays(table, table, table, 1, 1, 1);
		graph.add_raytracing_list_end(); // 1
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(launch->usage, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
		CHECK(has_shader_read_barrier(graph, 1, 51));
		CHECK(buffers->dirty.is_empty());
	}
	SUBCASE("The table bound after the trace: the list's end names it, and the buffer stays dirty for the next bind") {
		add_compute(graph, nullptr, launch, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE); // 0: writes the launch size.
		graph.add_raytracing_list_begin();
		graph.add_raytracing_list_bind_pipeline(RDD::RaytracingPipelineID());
		CHECK(graph.add_raytracing_list_trace_rays_indirect(table, table, table, RDD::BufferID(51), launch, 0));
		graph.add_raytracing_list_bindless_table(buffers);
		graph.add_raytracing_list_trace_rays(table, table, table, 1, 1, 1);
		CHECK_EQ(errors.count, 0);
		ERR_PRINT_OFF;
		graph.add_raytracing_list_end(); // 1
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("A buffer in a bindless table that this raytracing list binds is also the launch-size buffer of one of its indirect traces, and the table has not read it since it was last written. A buffer has one usage per raytracing list, so the table's reads in this raytracing list are not synchronized; bind the table in an earlier list after the write, or keep the buffer out of the table."));
		CHECK_EQ(launch->usage, RDG::RESOURCE_USAGE_INDIRECT_BUFFER_READ);
		CHECK_FALSE(has_shader_read_barrier(graph, 1, 51));
		REQUIRE_EQ(buffers->dirty.size(), 1u);
		CHECK_EQ(buffers->dirty[0], launch);
		// The next command that binds the table gets the barrier the raytracing list could not have.
		add_compute(graph, buffers); // 2
		CHECK(has_shader_read_barrier(graph, 2, 51));
		CHECK(buffers->dirty.is_empty());
		CHECK_EQ(errors.count, 1);
	}
	SUBCASE("A resident the table has read since its last write: the trace is recorded") {
		graph.add_raytracing_list_begin();
		graph.add_raytracing_list_bind_pipeline(RDD::RaytracingPipelineID());
		graph.add_raytracing_list_bindless_table(buffers);
		CHECK(graph.add_raytracing_list_trace_rays_indirect(table, table, table, RDD::BufferID(51), launch, 0));
		graph.add_raytracing_list_end();
		CHECK_EQ(errors.count, 0);
		CHECK_EQ(launch->usage, RDG::RESOURCE_USAGE_INDIRECT_BUFFER_READ);
		CHECK(buffers->dirty.is_empty());
	}

	RDG::bindless_table_remove_resident(buffers, launch);
	RDG::bindless_table_tracker_free(buffers);
	RDG::resource_tracker_free(launch);
}

TEST_CASE("[RD][RTPipeline] Without SUPPORTS_RAYTRACING_PIPELINE, libraries and links fail cleanly") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_RAY_QUERY));

	RTErrorCapture errors;
	ERR_PRINT_OFF;
	const RID library = rd->raytracing_pipeline_create({}, {}, {}, 1, RenderingDevice::RAYTRACING_PIPELINE_FLAG_LIBRARY);
	const int library_errors = errors.count;
	const String library_message = errors.last_message;
	errors.count = 0;
	const RID linked = rd->raytracing_pipeline_link(RID(), {});
	const int link_errors = errors.count;
	const String link_message = errors.last_message;
	ERR_PRINT_ON;

	CHECK_FALSE(library.is_valid());
	CHECK_EQ(library_errors, 1);
	CHECK_EQ(library_message, String(RDRayQuery::ERROR_NO_RAYTRACING_PIPELINE));
	CHECK_FALSE(linked.is_valid());
	CHECK_EQ(link_errors, 1);
	CHECK_EQ(link_message, String(RDRayQuery::ERROR_NO_RAYTRACING_PIPELINE));

	memdelete(rd);
}

TEST_CASE("[RD][RTPipeline] Without SUPPORTS_RAYTRACING_PIPELINE_LIBRARY, libraries and links fail with one named error") {
	// RT pipelines without libraries, and without a driver: reaching the driver would crash.
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_RAYTRACING_PIPELINE) | feature_bit(RenderingDevice::SUPPORTS_RAY_QUERY) | feature_bit(RenderingDevice::SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT));

	RTErrorCapture errors;
	ERR_PRINT_OFF;
	const RID library = rd->raytracing_pipeline_create({}, {}, {}, 1, RenderingDevice::RAYTRACING_PIPELINE_FLAG_LIBRARY);
	const int library_errors = errors.count;
	const String library_message = errors.last_message;
	errors.count = 0;
	const RID linked = rd->raytracing_pipeline_link(RID(), {});
	const int link_errors = errors.count;
	const String link_message = errors.last_message;
	ERR_PRINT_ON;

	CHECK_FALSE(library.is_valid());
	CHECK_EQ(library_errors, 1);
	CHECK_EQ(library_message, String(RDRTPipeline::ERROR_NO_LIBRARIES));
	CHECK_FALSE(linked.is_valid());
	CHECK_EQ(link_errors, 1);
	CHECK_EQ(link_message, String(RDRTPipeline::ERROR_NO_LIBRARIES));
	// The errors name the feature a caller can check first.
	CHECK(String(RDRTPipeline::ERROR_NO_LIBRARIES).contains("SUPPORTS_RAYTRACING_PIPELINE_LIBRARY"));
	CHECK(String(RDRTPipeline::ERROR_NO_INDIRECT).contains("SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT"));

	memdelete(rd);
}

TEST_CASE("[RD][RTPipeline] A pipeline fails with one named error before any driver work") {
	// A device with RT pipelines and libraries but without the features under test, and without a driver:
	// reaching the driver would crash, so each case must stop at its own error.
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_RAYTRACING_PIPELINE) | feature_bit(RenderingDevice::SUPPORTS_RAY_QUERY) | feature_bit(RenderingDevice::SUPPORTS_RAYTRACING_PIPELINE_LIBRARY));
	REQUIRE_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_RAYTRACING_REORDER));

	RTErrorCapture errors;
	SUBCASE("The reorder flag needs SUPPORTS_RAYTRACING_REORDER") {
		ERR_PRINT_OFF;
		const RID pipeline = rd->raytracing_pipeline_create({}, {}, {}, 1, RenderingDevice::RAYTRACING_PIPELINE_FLAG_REORDER);
		ERR_PRINT_ON;
		CHECK_FALSE(pipeline.is_valid());
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String(RDRTPipeline::ERROR_NO_REORDER));
	}
	SUBCASE("A record holds at most 32 bytes") {
		RenderingDevice::HitGroup hit_groups[2];
		hit_groups[0].record_data.resize_initialized(RDRTPipeline::MAX_HIT_GROUP_RECORD_SIZE);
		hit_groups[1].record_data.resize_initialized(RDRTPipeline::MAX_HIT_GROUP_RECORD_SIZE + 1);
		ERR_PRINT_OFF;
		const RID pipeline = rd->raytracing_pipeline_create({}, {}, Span<RenderingDevice::HitGroup>(hit_groups, 2), 1);
		ERR_PRINT_ON;
		CHECK_FALSE(pipeline.is_valid());
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, RDRTPipeline::get_record_too_large_error(1, RDRTPipeline::MAX_HIT_GROUP_RECORD_SIZE + 1));
	}
	SUBCASE("A link names a base that is not a pipeline") {
		ERR_PRINT_OFF;
		const RID pipeline = rd->raytracing_pipeline_link(RID(), {});
		ERR_PRINT_ON;
		CHECK_FALSE(pipeline.is_valid());
		CHECK_EQ(errors.count, 1);
		CHECK(errors.last_message.contains("base"));
	}

	memdelete(rd);
}

TEST_CASE("[RD][RTPipeline] Without SER, a shader that reorders fails with one named error before the driver") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_RAYTRACING_PIPELINE) | feature_bit(RenderingDevice::SUPPORTS_RAY_QUERY));
	CHECK_EQ(rd->get_missing_raytracing_reorder(0), 0u);
	CHECK_EQ(rd->get_missing_raytracing_reorder(RDRTPipeline::REORDER_USAGE_NV), uint32_t(RDRTPipeline::REORDER_USAGE_NV));
	// Without the feature the limit reads 0, whatever the driver would return.
	CHECK_EQ(rd->limit_get(RenderingDevice::LIMIT_RAYTRACING_REORDER_EXTENSIONS), 0u);

	RTErrorCapture errors;
	ERR_PRINT_OFF;
	const RID shader = rd->shader_create_from_spirv(raygen_stages(make_raygen_module(RDRTPipeline::REORDER_USAGE_NV)), "reorder");
	const int shader_errors = errors.count;
	const String shader_message = errors.last_message;
	errors.count = 0;
	const Vector<uint8_t> bytecode = rd->shader_compile_binary_from_spirv(raygen_stages(make_raygen_module(RDRTPipeline::REORDER_USAGE_EXT)), "reorder");
	const int bytecode_errors = errors.count;
	const String bytecode_message = errors.last_message;
	ERR_PRINT_ON;

	CHECK_FALSE(shader.is_valid());
	CHECK_EQ(shader_errors, 1);
	CHECK_EQ(shader_message, RDRTPipeline::get_missing_reorder_error("reorder", RDRTPipeline::REORDER_USAGE_NV));
	CHECK(bytecode.is_empty());
	CHECK_EQ(bytecode_errors, 1);
	CHECK_EQ(bytecode_message, RDRTPipeline::get_missing_reorder_error("reorder", RDRTPipeline::REORDER_USAGE_EXT));

	memdelete(rd);
}

TEST_CASE("[RD][RTPipeline] The reorder feature, its limit and the pipeline flags are bound") {
	CHECK_EQ(int64_t(RenderingDevice::RAYTRACING_PIPELINE_FLAG_LIBRARY), 1);
	CHECK_EQ(int64_t(RenderingDevice::RAYTRACING_PIPELINE_FLAG_REORDER), 2);
	CHECK(ClassDB::has_integer_constant("RenderingDevice", "SUPPORTS_RAYTRACING_REORDER"));
	CHECK(ClassDB::has_integer_constant("RenderingDevice", "SUPPORTS_RAYTRACING_PIPELINE_LIBRARY"));
	CHECK(ClassDB::has_integer_constant("RenderingDevice", "SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT"));
	CHECK(ClassDB::has_integer_constant("RenderingDevice", "LIMIT_RAYTRACING_REORDER_EXTENSIONS"));
	CHECK(ClassDB::has_integer_constant("RenderingDevice", "RAYTRACING_PIPELINE_FLAG_LIBRARY"));
	CHECK(ClassDB::has_method("RenderingDevice", "raytracing_pipeline_link"));
	CHECK(ClassDB::has_method("RenderingDevice", "raytracing_list_trace_rays_indirect"));

	Ref<RDHitGroup> hit_group;
	hit_group.instantiate();
	CHECK(hit_group->get_record_data().is_empty());
	PackedByteArray record;
	record.push_back(7);
	hit_group->set("record_data", record);
	CHECK_EQ(hit_group->get_record_data(), record);
}

#endif // RD_ENABLED
