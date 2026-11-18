/**************************************************************************/
/*  test_rendering_shader_container.cpp                                   */
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

TEST_FORCE_LINK(test_rendering_shader_container)

// GodotNext: dxil_toolchain (#123). The shader container's has_ray_query, beside #175's int64_atomics (version 4;
// version 5 appended #179's raytracing_reorder, version 6 #178's workgroup sizes) and, on Windows, the D3D12
// container (FORMAT_VERSION 4, shader models, the ray-tracing gate on the Mesa NIR library, the debug-only DXIL
// validator and the DXIL operations that read the instance values, and #121's ray-tracing stage data, shader
// records, stand-ins and unused-resource removal). Most SPIR-V modules are assembled by hand; the GLSL cases compile
// their shaders with the glslang module. No case needs a GPU. With the GodotNext Mesa NIR library, the instance-value
// case disassembles DXIL with dxil.dll and dxcompiler.dll from the installed Windows SDK.
// GodotNext: dxil_atomics (#279). Containers of another format or format version are not current, the D3D12 format
// version follows whether the Mesa NIR library declares atomics without readnone, and the atomic-declaration case
// disassembles DXIL the same way to check how the linked library declares the DXIL atomic operations (without
// readnone from 25.3.1-gn3 on).
// GodotNext: dxil_nonuniform (#293). The D3D12 format version is 6 with 25.3.1-gn4 or later, and the non-uniform case
// disassembles DXIL to check which createHandle calls mark their index non-uniform (from 25.3.1-gn4 on).

#include "core/error/error_macros.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/rendering_device_atomics64.h"
#include "servers/rendering/rendering_shader_container.h"

#include "modules/modules_enabled.gen.h" // For glslang.

// GodotNext: rd_mesh_shaders (#178). The task and mesh stage cases compile GLSL on every platform.
#ifdef MODULE_GLSLANG_ENABLED
#include "modules/glslang/shader_compile.h"
#endif
#ifdef VULKAN_ENABLED
#include "drivers/vulkan/rendering_shader_container_vulkan.h"
#endif
#ifdef METAL_ENABLED
#include "drivers/metal/rendering_shader_container_metal.h"
#endif

#ifdef D3D12_ENABLED
#include "drivers/d3d12/rendering_shader_container_d3d12.h"

#ifdef MODULE_GLSLANG_ENABLED
#include "modules/glslang/shader_compile.h"
#endif

#include <windows.h>
#endif

namespace TestRenderingShaderContainer {

using RDC = RenderingDeviceCommons;

// Records the errors and warnings printed while it is alive.
struct ErrorCapture {
	ErrorHandlerList handler;
	int count = 0;
	String first_message;

	ErrorCapture() {
		handler.errfunc = _capture;
		handler.userdata = this;
		add_error_handler(&handler);
	}

	~ErrorCapture() {
		remove_error_handler(&handler);
	}

	static void _capture(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
		ErrorCapture *self = static_cast<ErrorCapture *>(p_self);
		if (self->count == 0) {
			self->first_message = String::utf8((p_message != nullptr && *p_message != 0) ? p_message : p_error);
		}
		self->count++;
	}
};

enum class TestModule {
	COMPUTE,
	COMPUTE_RAY_QUERY,
	COMPUTE_RAY_QUERY_INT64_ATOMICS,
	RAYGEN,
};

void append_instruction(LocalVector<uint32_t> &r_words, uint32_t p_opcode, std::initializer_list<uint32_t> p_operands) {
	r_words.push_back(((uint32_t(p_operands.size()) + 1) << 16) | p_opcode);
	for (uint32_t operand : p_operands) {
		r_words.push_back(operand);
	}
}

// An instruction whose last operand is a literal string (NUL-terminated, padded to whole words).
void append_instruction_with_string(LocalVector<uint32_t> &r_words, uint32_t p_opcode, std::initializer_list<uint32_t> p_operands, const char *p_string) {
	const uint32_t length = uint32_t(strlen(p_string)) + 1;
	const uint32_t string_words = (length + 3) / 4;
	r_words.push_back(((uint32_t(p_operands.size()) + string_words + 1) << 16) | p_opcode);
	for (uint32_t operand : p_operands) {
		r_words.push_back(operand);
	}
	for (uint32_t i = 0; i < string_words; i++) {
		uint32_t word = 0;
		for (uint32_t j = 0; j < 4 && i * 4 + j < length; j++) {
			word |= uint32_t(uint8_t(p_string[i * 4 + j])) << (8 * j);
		}
		r_words.push_back(word);
	}
}

// A SPIR-V 1.4 module with an empty "main": a compute shader with local size 8x4x2 (optionally declaring the
// RayQueryKHR capability, alone or with Int64 and Int64Atomics), or a ray generation shader.
RDC::ShaderStageSPIRVData make_module(TestModule p_module) {
	enum : uint32_t {
		OP_EXTENSION = 10,
		OP_MEMORY_MODEL = 14,
		OP_ENTRY_POINT = 15,
		OP_EXECUTION_MODE = 16,
		OP_CAPABILITY = 17,
		OP_TYPE_VOID = 19,
		OP_TYPE_FUNCTION = 33,
		OP_FUNCTION = 54,
		OP_FUNCTION_END = 56,
		OP_LABEL = 248,
		OP_RETURN = 253,
		CAPABILITY_SHADER = 1,
		CAPABILITY_INT64 = 11,
		CAPABILITY_INT64_ATOMICS = 12,
		CAPABILITY_RAY_QUERY_KHR = 4472,
		CAPABILITY_RAY_TRACING_KHR = 4479,
		EXECUTION_MODEL_GL_COMPUTE = 5,
		EXECUTION_MODEL_RAY_GENERATION_KHR = 5313,
		EXECUTION_MODE_LOCAL_SIZE = 17,
		ID_MAIN = 1,
		ID_VOID = 2,
		ID_FUNCTION_TYPE = 3,
		ID_LABEL = 4,
		ID_BOUND = 5,
	};

	LocalVector<uint32_t> words = { 0x07230203, 0x00010400, 0, ID_BOUND, 0 };
	append_instruction(words, OP_CAPABILITY, { CAPABILITY_SHADER });
	if (p_module == TestModule::COMPUTE_RAY_QUERY_INT64_ATOMICS) {
		append_instruction(words, OP_CAPABILITY, { CAPABILITY_INT64 });
		append_instruction(words, OP_CAPABILITY, { CAPABILITY_INT64_ATOMICS });
	}
	if (p_module == TestModule::COMPUTE_RAY_QUERY || p_module == TestModule::COMPUTE_RAY_QUERY_INT64_ATOMICS) {
		append_instruction(words, OP_CAPABILITY, { CAPABILITY_RAY_QUERY_KHR });
		append_instruction_with_string(words, OP_EXTENSION, {}, "SPV_KHR_ray_query");
	} else if (p_module == TestModule::RAYGEN) {
		append_instruction(words, OP_CAPABILITY, { CAPABILITY_RAY_TRACING_KHR });
		append_instruction_with_string(words, OP_EXTENSION, {}, "SPV_KHR_ray_tracing");
	}
	append_instruction(words, OP_MEMORY_MODEL, { 0, 1 }); // Logical, GLSL450.
	const bool raygen = p_module == TestModule::RAYGEN;
	append_instruction_with_string(words, OP_ENTRY_POINT, { raygen ? EXECUTION_MODEL_RAY_GENERATION_KHR : EXECUTION_MODEL_GL_COMPUTE, ID_MAIN }, "main");
	if (!raygen) {
		append_instruction(words, OP_EXECUTION_MODE, { ID_MAIN, EXECUTION_MODE_LOCAL_SIZE, 8, 4, 2 });
	}
	append_instruction(words, OP_TYPE_VOID, { ID_VOID });
	append_instruction(words, OP_TYPE_FUNCTION, { ID_FUNCTION_TYPE, ID_VOID });
	append_instruction(words, OP_FUNCTION, { ID_VOID, ID_MAIN, 0, ID_FUNCTION_TYPE });
	append_instruction(words, OP_LABEL, { ID_LABEL });
	append_instruction(words, OP_RETURN, {});
	append_instruction(words, OP_FUNCTION_END, {});

	RDC::ShaderStageSPIRVData stage;
	stage.shader_stage = raygen ? RDC::SHADER_STAGE_RAYGEN : RDC::SHADER_STAGE_COMPUTE;
	stage.spirv.resize(words.size() * sizeof(uint32_t));
	memcpy(stage.spirv.ptrw(), words.ptr(), stage.spirv.size());
	return stage;
}

Vector<RDC::ShaderStageSPIRVData> make_stages(TestModule p_module) {
	Vector<RDC::ShaderStageSPIRVData> stages;
	stages.push_back(make_module(p_module));
	return stages;
}

// A container that stores the SPIR-V as its code, to test the shared container code on every platform.
class TestShaderContainer : public RenderingShaderContainer {
	GDSOFTCLASS(TestShaderContainer, RenderingShaderContainer);

protected:
	virtual uint32_t _format() const override { return 0x54534554; }
	virtual uint32_t _format_version() const override { return 1; }

	virtual bool _set_code_from_spirv(const ReflectShader &p_shader) override {
		shaders.resize(p_shader.shader_stages.size());
		for (uint32_t i = 0; i < p_shader.shader_stages.size(); i++) {
			Shader &shader = shaders.write[i];
			shader.shader_stage = p_shader.shader_stages[i].shader_stage;
			shader.code_compressed_bytes = p_shader.shader_stages[i].spirv_data();
			shader.code_decompressed_size = shader.code_compressed_bytes.size();
		}
		return true;
	}

public:
	bool has_ray_query() const { return reflection_data.has_ray_query != 0; }
};

uint32_t read_u32(const PackedByteArray &p_bytes, int64_t p_offset) {
	uint32_t value = 0;
	memcpy(&value, p_bytes.ptr() + p_offset, sizeof(value));
	return value;
}

void write_u32(PackedByteArray &r_bytes, int64_t p_offset, uint32_t p_value) {
	memcpy(r_bytes.ptrw() + p_offset, &p_value, sizeof(p_value));
}

// The container header is magic, version, format, format version and shader count (5 x uint32_t).
constexpr int64_t HEADER_VERSION_OFFSET = 4;
constexpr int64_t HEADER_FORMAT_OFFSET = 8;
constexpr int64_t HEADER_FORMAT_VERSION_OFFSET = 12;
// ReflectionData follows the header (TestShaderContainer adds no header data). Version 4's was 72 bytes and ended with
// int64_atomics (#175, offset 64) and has_ray_query (#123, offset 68). Version 5's was 80 bytes: it appended
// raytracing_reorder (#179, offset 72) and an explicit padding word (offset 76). Version 6's is 104 bytes: the task and
// mesh workgroup sizes (#178, offsets 76 and 88) take that padding word, and a new explicit padding word follows them
// (offset 100).
constexpr int64_t REFLECTION_OFFSET = 20;
constexpr int64_t REFLECTION_INT64_ATOMICS_OFFSET = REFLECTION_OFFSET + 64;
constexpr int64_t REFLECTION_HAS_RAY_QUERY_OFFSET = REFLECTION_OFFSET + 68;
constexpr int64_t REFLECTION_RAYTRACING_REORDER_OFFSET = REFLECTION_OFFSET + 72;
constexpr int64_t REFLECTION_TASK_LOCAL_SIZE_OFFSET = REFLECTION_OFFSET + 76;
constexpr int64_t REFLECTION_MESH_LOCAL_SIZE_OFFSET = REFLECTION_OFFSET + 88;
constexpr int64_t REFLECTION_PADDING_OFFSET = REFLECTION_OFFSET + 100;
constexpr int64_t REFLECTION_END = REFLECTION_OFFSET + 104;

TEST_CASE("[ShaderContainer] Reflection sets has_ray_query from the SPIR-V RayQueryKHR capability") {
	CHECK(RenderingShaderContainer::CONTAINER_VERSION == 6);

	Ref<TestShaderContainer> plain;
	plain.instantiate();
	REQUIRE(plain->set_code_from_spirv("plain", make_stages(TestModule::COMPUTE)));
	CHECK_FALSE(plain->has_ray_query());
	const RDC::ShaderReflection plain_reflection = plain->get_shader_reflection();
	CHECK(plain_reflection.pipeline_type == RDC::PIPELINE_TYPE_COMPUTE);
	CHECK(plain_reflection.compute_local_size[0] == 8);
	CHECK(plain_reflection.compute_local_size[1] == 4);
	CHECK(plain_reflection.compute_local_size[2] == 2);

	Ref<TestShaderContainer> ray_query;
	ray_query.instantiate();
	REQUIRE(ray_query->set_code_from_spirv("ray_query", make_stages(TestModule::COMPUTE_RAY_QUERY)));
	CHECK(ray_query->has_ray_query());

	// The flag survives serialization.
	const PackedByteArray bytes = ray_query->to_bytes();
	REQUIRE(bytes.size() > HEADER_FORMAT_VERSION_OFFSET + 4);
	CHECK(read_u32(bytes, HEADER_VERSION_OFFSET) == RenderingShaderContainer::CONTAINER_VERSION);
	Ref<TestShaderContainer> loaded;
	loaded.instantiate();
	REQUIRE(loaded->from_bytes(bytes));
	CHECK(loaded->has_ray_query());
	CHECK(loaded->get_shader_reflection().compute_local_size[0] == 8);

	Ref<TestShaderContainer> loaded_plain;
	loaded_plain.instantiate();
	REQUIRE(loaded_plain->from_bytes(plain->to_bytes()));
	CHECK_FALSE(loaded_plain->has_ray_query());
}

TEST_CASE("[ShaderContainer] Containers of versions 2 to 5 are rejected with one message and rebuilt") {
	Ref<TestShaderContainer> container;
	container.instantiate();
	REQUIRE(container->set_code_from_spirv("stale", make_stages(TestModule::COMPUTE)));
	const PackedByteArray current = container->to_bytes();
	REQUIRE(current.size() > HEADER_FORMAT_VERSION_OFFSET + 4);

	// Version 2 is the layout before #175, version 3 is #175's (int64_atomics, no has_ray_query), version 4 is #123's
	// (72 bytes, no raytracing_reorder) and version 5 is #179's (80 bytes, no task or mesh stages and workgroup sizes,
	// #178).
	const uint32_t stale_versions[] = { 2, 3, 4, 5 };
	for (const uint32_t stale_version : stale_versions) {
		PackedByteArray stale = current;
		write_u32(stale, HEADER_VERSION_OFFSET, stale_version);

		{
			// Shader caches check the header first; that check prints nothing.
			ErrorCapture capture;
			uint32_t version = 0;
			CHECK_FALSE(RenderingShaderContainer::is_container_version_current(stale, &version));
			CHECK(version == stale_version);
			CHECK(capture.count == 0);
		}

		{
			ErrorCapture capture;
			Ref<TestShaderContainer> loaded;
			loaded.instantiate();
			CHECK_FALSE(loaded->from_bytes(stale));
			CHECK(capture.count == 1);
			CHECK_MESSAGE(capture.first_message.contains(vformat("(%d, expected 6)", stale_version)), capture.first_message);
		}
	}

	// Rebuilding from the SPIR-V gives a current container that loads without a message.
	Ref<TestShaderContainer> rebuilt;
	rebuilt.instantiate();
	REQUIRE(rebuilt->set_code_from_spirv("stale", make_stages(TestModule::COMPUTE)));
	const PackedByteArray fresh = rebuilt->to_bytes();
	uint32_t fresh_version = 0;
	CHECK(RenderingShaderContainer::is_container_version_current(fresh, &fresh_version));
	CHECK(fresh_version == 6);
	{
		ErrorCapture capture;
		Ref<TestShaderContainer> loaded;
		loaded.instantiate();
		CHECK(loaded->from_bytes(fresh));
		CHECK(capture.count == 0);
		CHECK(loaded->shaders.size() == 1);
	}

	// Bytes that are not a shader container at all.
	PackedByteArray garbage;
	garbage.resize_initialized(32);
	uint32_t garbage_version = 7;
	CHECK_FALSE(RenderingShaderContainer::is_container_version_current(garbage, &garbage_version));
	CHECK(garbage_version == 0);
	CHECK_FALSE(RenderingShaderContainer::is_container_version_current(PackedByteArray()));
}

// GodotNext: dxil_atomics (#279). Shader caches also rebuild a container of the current version whose format or format
// version is not the one this build writes (on D3D12, DXIL from a Mesa NIR library that declares the atomics
// differently).
TEST_CASE("[ShaderContainer] A container of another format or format version is not current") {
	Ref<TestShaderContainer> container;
	container.instantiate();
	REQUIRE(container->set_code_from_spirv("current", make_stages(TestModule::COMPUTE)));
	const PackedByteArray current = container->to_bytes();
	REQUIRE(current.size() > HEADER_FORMAT_VERSION_OFFSET + 4);
	CHECK(container->get_current_format_version() == 1);
	CHECK(read_u32(current, HEADER_FORMAT_VERSION_OFFSET) == 1);

	// The check reads only the header and prints nothing.
	ErrorCapture capture;
	uint32_t version = 0;
	uint32_t format_version = 0;
	CHECK(container->is_container_current(current, &version, &format_version));
	CHECK(version == RenderingShaderContainer::CONTAINER_VERSION);
	CHECK(format_version == 1);

	// An older or a newer format version is not current, and the one found is reported. The version check alone
	// still passes, and from_bytes() reads the older one.
	const uint32_t other_format_versions[] = { 0, 2 };
	for (const uint32_t other_format_version : other_format_versions) {
		PackedByteArray other = current;
		write_u32(other, HEADER_FORMAT_VERSION_OFFSET, other_format_version);
		CHECK_FALSE(container->is_container_current(other, &version, &format_version));
		CHECK(version == RenderingShaderContainer::CONTAINER_VERSION);
		CHECK(format_version == other_format_version);
		CHECK(RenderingShaderContainer::is_container_version_current(other));
	}
	{
		PackedByteArray older = current;
		write_u32(older, HEADER_FORMAT_VERSION_OFFSET, 0);
		Ref<TestShaderContainer> loaded;
		loaded.instantiate();
		CHECK(loaded->from_bytes(older));
	}
	CHECK(capture.count == 0);

	// Another format.
	PackedByteArray other_format = current;
	write_u32(other_format, HEADER_FORMAT_OFFSET, 0x12345678);
	CHECK_FALSE(container->is_container_current(other_format, &version, &format_version));
	CHECK(format_version == 1);

	// Another container version (4, the one before #179's raytracing_reorder): its format version is not read.
	PackedByteArray old = current;
	write_u32(old, HEADER_VERSION_OFFSET, 4);
	CHECK_FALSE(container->is_container_current(old, &version, &format_version));
	CHECK(version == 4);
	CHECK(format_version == 0);

	// Bytes that are not a shader container at all.
	PackedByteArray garbage;
	garbage.resize_initialized(32);
	version = 7;
	format_version = 7;
	CHECK_FALSE(container->is_container_current(garbage, &version, &format_version));
	CHECK(version == 0);
	CHECK(format_version == 0);
	CHECK_FALSE(container->is_container_current(PackedByteArray()));
	CHECK(capture.count == 0);
}

TEST_CASE("[ShaderContainer] Version 6 keeps int64_atomics, has_ray_query and raytracing_reorder at their version-5 offsets and ends with a zero padding word") {
	Ref<TestShaderContainer> both;
	both.instantiate();
	REQUIRE(both->set_code_from_spirv("both", make_stages(TestModule::COMPUTE_RAY_QUERY_INT64_ATOMICS)));
	CHECK(both->has_ray_query());
	CHECK(both->get_int64_atomics_usage() == uint32_t(RDAtomics64::USAGE_INT64_ATOMICS));

	const PackedByteArray bytes = both->to_bytes();
	REQUIRE(bytes.size() >= REFLECTION_END + 4);
	CHECK(read_u32(bytes, HEADER_VERSION_OFFSET) == 6);
	CHECK(read_u32(bytes, REFLECTION_INT64_ATOMICS_OFFSET) == uint32_t(RDAtomics64::USAGE_INT64_ATOMICS));
	CHECK(read_u32(bytes, REFLECTION_HAS_RAY_QUERY_OFFSET) == 1);
	// A compute shader does not reorder; test_rd_rt_pipelines.cpp checks a raygen shader's word.
	CHECK(read_u32(bytes, REFLECTION_RAYTRACING_REORDER_OFFSET) == 0);
	CHECK(read_u32(bytes, REFLECTION_PADDING_OFFSET) == 0);
	// The shader name follows ReflectionData directly (no reflection extra data in this container).
	CHECK(memcmp(bytes.ptr() + REFLECTION_END, "both", 4) == 0);

	Ref<TestShaderContainer> loaded;
	loaded.instantiate();
	REQUIRE(loaded->from_bytes(bytes));
	CHECK(loaded->has_ray_query());
	CHECK(loaded->get_int64_atomics_usage() == uint32_t(RDAtomics64::USAGE_INT64_ATOMICS));
	CHECK(loaded->get_shader_reflection().int64_atomics == uint32_t(RDAtomics64::USAGE_INT64_ATOMICS));

	// The two words are independent: a ray-query shader without int64 atomics writes 0 for int64_atomics, and a
	// container whose has_ray_query word is 0 keeps its int64_atomics.
	Ref<TestShaderContainer> ray_query;
	ray_query.instantiate();
	REQUIRE(ray_query->set_code_from_spirv("ray_query", make_stages(TestModule::COMPUTE_RAY_QUERY)));
	const PackedByteArray ray_query_bytes = ray_query->to_bytes();
	REQUIRE(ray_query_bytes.size() >= REFLECTION_END);
	CHECK(read_u32(ray_query_bytes, REFLECTION_INT64_ATOMICS_OFFSET) == 0);
	CHECK(read_u32(ray_query_bytes, REFLECTION_HAS_RAY_QUERY_OFFSET) == 1);

	PackedByteArray int64_only = bytes;
	write_u32(int64_only, REFLECTION_HAS_RAY_QUERY_OFFSET, 0);
	Ref<TestShaderContainer> loaded_int64_only;
	loaded_int64_only.instantiate();
	REQUIRE(loaded_int64_only->from_bytes(int64_only));
	CHECK_FALSE(loaded_int64_only->has_ray_query());
	CHECK(loaded_int64_only->get_int64_atomics_usage() == uint32_t(RDAtomics64::USAGE_INT64_ATOMICS));
}

// GodotNext: rd_mesh_shaders (#178). Container version 6: the task and mesh stages, their workgroup sizes and the
// rules for combining them with the other render stages. The shaders compile with the glslang module.
#ifdef MODULE_GLSLANG_ENABLED
const char *MESH_TASK_GLSL = R"(#version 460
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

struct Payload {
	uint meshlets[32];
};
taskPayloadSharedEXT Payload payload;

layout(set = 0, binding = 0, std430) readonly buffer Visible {
	uint data[];
}
visible;

void main() {
	payload.meshlets[gl_LocalInvocationIndex] = visible.data[gl_GlobalInvocationID.x];
	EmitMeshTasksEXT(32, 1, 1);
}
)";

const char *MESH_MESH_GLSL = R"(#version 460
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 16, local_size_y = 2, local_size_z = 1) in;
layout(triangles, max_vertices = 64, max_primitives = 124) out;

struct Payload {
	uint meshlets[32];
};
taskPayloadSharedEXT Payload payload;

layout(push_constant, std430) uniform Params {
	uint base;
}
params;

layout(location = 0) perprimitiveEXT out uint primitive_id[];

void main() {
	SetMeshOutputsEXT(3, 1);
	if (gl_LocalInvocationIndex == 0) {
		gl_MeshVerticesEXT[0].gl_Position = vec4(-1.0, -1.0, 0.0, 1.0);
		gl_MeshVerticesEXT[1].gl_Position = vec4(3.0, -1.0, 0.0, 1.0);
		gl_MeshVerticesEXT[2].gl_Position = vec4(-1.0, 3.0, 0.0, 1.0);
		gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
		primitive_id[0] = payload.meshlets[gl_WorkGroupID.x] + params.base;
	}
}
)";

// A mesh shader without a task stage, so it reads no payload.
const char *MESH_ONLY_GLSL = R"(#version 460
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(triangles, max_vertices = 3, max_primitives = 1) out;

layout(location = 0) perprimitiveEXT out uint primitive_id[];

void main() {
	SetMeshOutputsEXT(3, 1);
	if (gl_LocalInvocationIndex == 0) {
		gl_MeshVerticesEXT[0].gl_Position = vec4(-1.0, -1.0, 0.0, 1.0);
		gl_MeshVerticesEXT[1].gl_Position = vec4(3.0, -1.0, 0.0, 1.0);
		gl_MeshVerticesEXT[2].gl_Position = vec4(-1.0, 3.0, 0.0, 1.0);
		gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
		primitive_id[0] = gl_WorkGroupID.x;
	}
}
)";

const char *MESH_FRAGMENT_GLSL = R"(#version 460
#extension GL_EXT_mesh_shader : require

layout(location = 0) perprimitiveEXT flat in uint primitive_id;
layout(location = 0) out uint color;

void main() {
	color = primitive_id;
}
)";

const char *MESH_VERTEX_GLSL = R"(#version 450

void main() {
	gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

// Compiles one stage for Vulkan 1.1 at the SPIR-V version the Vulkan container format asks for (1.4 for the task
// and mesh stages, which GL_EXT_mesh_shader needs).
RDC::ShaderStageSPIRVData compile_mesh_case_stage(RDC::ShaderStage p_stage, const char *p_source) {
	String error;
	RDC::ShaderStageSPIRVData stage;
	stage.shader_stage = p_stage;
	const bool mesh_stage = p_stage == RDC::SHADER_STAGE_TASK || p_stage == RDC::SHADER_STAGE_MESH;
	stage.spirv = compile_glslang_shader(p_stage, String::utf8(p_source), RDC::SHADER_LANGUAGE_VULKAN_VERSION_1_1, mesh_stage ? RDC::SHADER_SPIRV_VERSION_1_4 : RDC::SHADER_SPIRV_VERSION_1_3, &error);
	REQUIRE_MESSAGE(!stage.spirv.is_empty(), vformat("glslang failed to compile the %s stage:\n%s", RDC::SHADER_STAGE_NAMES[p_stage], error));
	return stage;
}

Vector<RDC::ShaderStageSPIRVData> task_mesh_fragment_stages() {
	Vector<RDC::ShaderStageSPIRVData> stages;
	stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_TASK, MESH_TASK_GLSL));
	stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_MESH, MESH_MESH_GLSL));
	stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_FRAGMENT, MESH_FRAGMENT_GLSL));
	return stages;
}

void check_task_mesh_reflection(const RDC::ShaderReflection &p_reflection) {
	CHECK(p_reflection.pipeline_type == RDC::PIPELINE_TYPE_RASTERIZATION);
	CHECK(p_reflection.is_mesh());
	CHECK(uint32_t(p_reflection.stages_bits) == uint32_t(RDC::SHADER_STAGE_TASK_BIT | RDC::SHADER_STAGE_MESH_BIT | RDC::SHADER_STAGE_FRAGMENT_BIT));
	REQUIRE(p_reflection.stages_vector.size() == 3);
	CHECK(p_reflection.stages_vector[0] == RDC::SHADER_STAGE_FRAGMENT);
	CHECK(p_reflection.stages_vector[1] == RDC::SHADER_STAGE_TASK);
	CHECK(p_reflection.stages_vector[2] == RDC::SHADER_STAGE_MESH);
	CHECK(p_reflection.task_local_size[0] == 32);
	CHECK(p_reflection.task_local_size[1] == 1);
	CHECK(p_reflection.task_local_size[2] == 1);
	CHECK(p_reflection.mesh_local_size[0] == 16);
	CHECK(p_reflection.mesh_local_size[1] == 2);
	CHECK(p_reflection.mesh_local_size[2] == 1);
	CHECK(p_reflection.vertex_input_mask == 0);
	CHECK(p_reflection.fragment_output_mask == 1);
	CHECK(p_reflection.push_constant_size == 4);
	CHECK(p_reflection.push_constant_stages.has_flag(RDC::SHADER_STAGE_MESH_BIT));
	CHECK_FALSE(p_reflection.push_constant_stages.has_flag(RDC::SHADER_STAGE_TASK_BIT));
	REQUIRE(p_reflection.uniform_sets.size() == 1);
	REQUIRE(p_reflection.uniform_sets[0].size() == 1);
	CHECK(p_reflection.uniform_sets[0][0].stages == uint32_t(RDC::SHADER_STAGE_TASK_BIT));
	CHECK(p_reflection.compute_local_size[0] == 0);
}

TEST_CASE("[ShaderContainer] Version 6 round-trips task and mesh stages and their workgroup sizes") {
	Ref<TestShaderContainer> container;
	container.instantiate();
	REQUIRE(container->set_code_from_spirv("task_mesh", task_mesh_fragment_stages()));
	check_task_mesh_reflection(container->get_shader_reflection());

	// The sizes are ReflectionData's six words before its padding word.
	const PackedByteArray bytes = container->to_bytes();
	REQUIRE(bytes.size() >= REFLECTION_END + 4);
	CHECK(read_u32(bytes, HEADER_VERSION_OFFSET) == 6);
	CHECK(read_u32(bytes, REFLECTION_TASK_LOCAL_SIZE_OFFSET) == 32);
	CHECK(read_u32(bytes, REFLECTION_TASK_LOCAL_SIZE_OFFSET + 4) == 1);
	CHECK(read_u32(bytes, REFLECTION_TASK_LOCAL_SIZE_OFFSET + 8) == 1);
	CHECK(read_u32(bytes, REFLECTION_MESH_LOCAL_SIZE_OFFSET) == 16);
	CHECK(read_u32(bytes, REFLECTION_MESH_LOCAL_SIZE_OFFSET + 4) == 2);
	CHECK(read_u32(bytes, REFLECTION_MESH_LOCAL_SIZE_OFFSET + 8) == 1);
	CHECK(read_u32(bytes, REFLECTION_PADDING_OFFSET) == 0);
	CHECK(memcmp(bytes.ptr() + REFLECTION_END, "task_mesh", 9) == 0);

	Ref<TestShaderContainer> loaded;
	loaded.instantiate();
	{
		ErrorCapture capture;
		REQUIRE(loaded->from_bytes(bytes));
		CHECK(capture.count == 0);
	}
	check_task_mesh_reflection(loaded->get_shader_reflection());
	REQUIRE(loaded->shaders.size() == 3);
	CHECK(loaded->shaders[1].shader_stage == RDC::SHADER_STAGE_MESH);

	// A copy that claims version 5 is rejected with one message, so caches rebuild it.
	PackedByteArray stale = bytes;
	write_u32(stale, HEADER_VERSION_OFFSET, 5);
	ErrorCapture capture;
	Ref<TestShaderContainer> stale_loaded;
	stale_loaded.instantiate();
	CHECK_FALSE(stale_loaded->from_bytes(stale));
	CHECK(capture.count == 1);
	CHECK_MESSAGE(capture.first_message.contains("(5, expected 6)"), capture.first_message);
}

TEST_CASE("[ShaderContainer] A stage that enables GL_EXT_mesh_shader compiles to SPIR-V 1.4") {
	// The fragment shader of a mesh pipeline reads per-primitive values, which need the extension and so SPIR-V 1.4,
	// while the container formats ask for SPIR-V 1.3 for fragment shaders.
	String error;
	const Vector<uint8_t> fragment = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, String::utf8(MESH_FRAGMENT_GLSL), RDC::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RDC::SHADER_SPIRV_VERSION_1_3, &error);
	REQUIRE_MESSAGE(fragment.size() >= 8, error);
	CHECK(read_u32(fragment, 4) == 0x00010400);
	// Without the extension the requested version stands.
	const Vector<uint8_t> vertex = compile_glslang_shader(RDC::SHADER_STAGE_VERTEX, String::utf8(MESH_VERTEX_GLSL), RDC::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RDC::SHADER_SPIRV_VERSION_1_3, &error);
	REQUIRE_MESSAGE(vertex.size() >= 8, error);
	CHECK(read_u32(vertex, 4) == 0x00010300);
	// Only a directive that enables it counts: a comment or "disable" that names it keeps SPIR-V 1.3, which Vulkan 1.1
	// devices without VK_KHR_spirv_1_4 load. Spaces around the parts of a directive do not matter. (glslang refuses
	// the extension's directive in a vertex shader, so these are fragment shaders.) A failed compile must not be read:
	// REQUIRE does not stop a test case in builds without exceptions.
	const char *named_only[] = {
		"#version 450\n// Unlike the mesh path, this needs no GL_EXT_mesh_shader.\nlayout(location = 0) out vec4 color;\nvoid main() {\n\tcolor = vec4(1.0);\n}\n",
		"#version 450\n#extension GL_EXT_mesh_shader : disable\nlayout(location = 0) out vec4 color;\nvoid main() {\n\tcolor = vec4(1.0);\n}\n",
	};
	for (const char *source : named_only) {
		const Vector<uint8_t> named = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, String::utf8(source), RDC::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RDC::SHADER_SPIRV_VERSION_1_3, &error);
		CHECK_MESSAGE(named.size() >= 8, error);
		if (named.size() >= 8) {
			CHECK_MESSAGE(read_u32(named, 4) == 0x00010300, source);
		}
	}
	const String spaced = String::utf8(MESH_FRAGMENT_GLSL).replace("#extension GL_EXT_mesh_shader : require", "  #  extension GL_EXT_mesh_shader:enable");
	CHECK(spaced != String::utf8(MESH_FRAGMENT_GLSL));
	const Vector<uint8_t> enabled = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, spaced, RDC::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RDC::SHADER_SPIRV_VERSION_1_3, &error);
	CHECK_MESSAGE(enabled.size() >= 8, error);
	if (enabled.size() >= 8) {
		CHECK(read_u32(enabled, 4) == 0x00010400);
	}
}

TEST_CASE("[ShaderContainer] A mesh shader needs no task stage, and its task workgroup size stays 0") {
	Vector<RDC::ShaderStageSPIRVData> stages;
	stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_MESH, MESH_ONLY_GLSL));
	stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_FRAGMENT, MESH_FRAGMENT_GLSL));
	Ref<TestShaderContainer> container;
	container.instantiate();
	REQUIRE(container->set_code_from_spirv("mesh_only", stages));
	Ref<TestShaderContainer> loaded;
	loaded.instantiate();
	REQUIRE(loaded->from_bytes(container->to_bytes()));
	const RDC::ShaderReflection reflection = loaded->get_shader_reflection();
	CHECK(reflection.is_mesh());
	CHECK(uint32_t(reflection.stages_bits) == uint32_t(RDC::SHADER_STAGE_MESH_BIT | RDC::SHADER_STAGE_FRAGMENT_BIT));
	CHECK(reflection.task_local_size[0] == 0);
	CHECK(reflection.task_local_size[1] == 0);
	CHECK(reflection.task_local_size[2] == 0);
	CHECK(reflection.mesh_local_size[0] == 64);
	CHECK(reflection.mesh_local_size[1] == 1);
	CHECK(reflection.mesh_local_size[2] == 1);

	// A vertex shader's container is not a mesh shader and has no mesh sizes.
	Vector<RDC::ShaderStageSPIRVData> vertex_stages;
	vertex_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_VERTEX, MESH_VERTEX_GLSL));
	Ref<TestShaderContainer> vertex;
	vertex.instantiate();
	REQUIRE(vertex->set_code_from_spirv("vertex", vertex_stages));
	CHECK_FALSE(vertex->get_shader_reflection().is_mesh());
	CHECK(vertex->get_shader_reflection().mesh_local_size[0] == 0);
}

TEST_CASE("[ShaderContainer] Task and mesh stages mix with no vertex stage, and a task stage needs a mesh stage") {
	SUBCASE("Vertex and mesh") {
		Vector<RDC::ShaderStageSPIRVData> stages;
		stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_VERTEX, MESH_VERTEX_GLSL));
		stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_MESH, MESH_ONLY_GLSL));
		Ref<TestShaderContainer> container;
		container.instantiate();
		ErrorCapture capture;
		CHECK_FALSE(container->set_code_from_spirv("mixed", stages));
		CHECK(capture.count == 2); // The rule, then set_code_from_spirv()'s reflection failure.
		CHECK_MESSAGE(capture.first_message.contains("'mixed': mesh and task shaders cannot be mixed with vertex or tessellation shaders."), capture.first_message);
	}
	SUBCASE("Task without mesh") {
		Vector<RDC::ShaderStageSPIRVData> stages;
		stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_TASK, MESH_TASK_GLSL));
		stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_FRAGMENT, MESH_FRAGMENT_GLSL));
		Ref<TestShaderContainer> container;
		container.instantiate();
		ErrorCapture capture;
		CHECK_FALSE(container->set_code_from_spirv("task_only", stages));
		CHECK(capture.count == 2);
		CHECK_MESSAGE(capture.first_message.contains("'task_only': a task shader needs a mesh shader in the same shader."), capture.first_message);
	}
}

#ifdef VULKAN_ENABLED
TEST_CASE("[ShaderContainer][Vulkan] Vulkan containers hold task and mesh stages as SPIR-V 1.4") {
	RenderingShaderContainerFormatVulkan format;
	CHECK(format.supports_mesh_shaders());
	CHECK(format.get_shader_spirv_version(RDC::SHADER_STAGE_TASK) == RDC::SHADER_SPIRV_VERSION_1_4);
	CHECK(format.get_shader_spirv_version(RDC::SHADER_STAGE_MESH) == RDC::SHADER_SPIRV_VERSION_1_4);
	CHECK(format.get_shader_spirv_version(RDC::SHADER_STAGE_VERTEX) == RDC::SHADER_SPIRV_VERSION_1_3);

	Ref<RenderingShaderContainer> container = format.create_container();
	REQUIRE(container->set_code_from_spirv("task_mesh", task_mesh_fragment_stages()));
	Ref<RenderingShaderContainer> loaded = format.create_container();
	REQUIRE(loaded->from_bytes(container->to_bytes()));
	check_task_mesh_reflection(loaded->get_shader_reflection());
	REQUIRE(loaded->shaders.size() == 3);
	CHECK(loaded->shaders[0].shader_stage == RDC::SHADER_STAGE_TASK);
	CHECK(loaded->shaders[1].shader_stage == RDC::SHADER_STAGE_MESH);
	CHECK(loaded->shaders[2].shader_stage == RDC::SHADER_STAGE_FRAGMENT);
}
#endif // VULKAN_ENABLED

#ifdef METAL_ENABLED
// The MSL source the Metal container stores for a shader (before any compiled library).
String metal_source(const Ref<RenderingShaderContainerMetal> &p_container, int64_t p_index) {
	const RenderingShaderContainer::Shader &shader = p_container->shaders[p_index];
	Vector<uint8_t> code;
	code.resize(shader.code_decompressed_size);
	REQUIRE(p_container->decompress_code(shader.code_compressed_bytes.ptr(), shader.code_compressed_bytes.size(), shader.code_compression_flags, code.ptrw(), code.size()));
	return String::utf8((const char *)code.ptr(), p_container->mtl_shaders[p_index].source_size);
}

TEST_CASE("[ShaderContainer][Metal] Task and mesh stages compile to object and mesh functions, with the payload size") {
	const MetalDeviceProfile *profile = MetalDeviceProfile::get_profile(MetalDeviceProfile::Platform::macOS, MetalDeviceProfile::GPU::Apple9, os_version::MACOS_14_0);
	REQUIRE(profile != nullptr);
	RenderingShaderContainerFormatMetal format(profile);
	CHECK(format.supports_mesh_shaders());
	// The shader baker bakes mesh-shader versions only for profiles whose GPUs run them (Apple7 and later).
	const MetalDeviceProfile *apple6 = MetalDeviceProfile::get_profile(MetalDeviceProfile::Platform::iOS, MetalDeviceProfile::GPU::Apple6, os_version::IOS_17_0);
	REQUIRE(apple6 != nullptr);
	CHECK_FALSE(RenderingShaderContainerFormatMetal(apple6).supports_mesh_shaders());
	// An Apple7 profile for iOS 15 has MSL 2.4 and no argument buffers, so its exports bake no mesh-shader versions.
	const MetalDeviceProfile *ios15 = MetalDeviceProfile::get_profile(MetalDeviceProfile::Platform::iOS, MetalDeviceProfile::GPU::Apple7, os_version::IOS_15_0);
	REQUIRE(ios15 != nullptr);
	CHECK_FALSE(RenderingShaderContainerFormatMetal(ios15).supports_mesh_shaders());
	CHECK(RenderingShaderContainerMetal::FORMAT_VERSION == 4);

	Ref<RenderingShaderContainerMetal> container = format.create_container();
	REQUIRE(container.is_valid());
	REQUIRE(container->set_code_from_spirv("task_mesh", task_mesh_fragment_stages()));
	// The payload is 32 uints, which the mesh pipeline reserves (payloadMemoryLength).
	CHECK(container->mtl_reflection_data.task_payload_size == 128);
	REQUIRE(container->shaders.size() == 3);
	CHECK(metal_source(container, 0).contains("[[object]]"));
	CHECK(metal_source(container, 1).contains("[[mesh]]"));

	Ref<RenderingShaderContainerMetal> loaded = format.create_container();
	REQUIRE(loaded->from_bytes(container->to_bytes()));
	CHECK(loaded->mtl_reflection_data.task_payload_size == 128);
	check_task_mesh_reflection(loaded->get_shader_reflection());
}
#endif // METAL_ENABLED
#endif // MODULE_GLSLANG_ENABLED

#ifdef D3D12_ENABLED
using DXILValidationStats = RenderingShaderContainerD3D12::DXILValidationStats;

// The offset of a part's data in a DXBC container, or -1. The container header is 32 bytes and ends with the part
// count, followed by one uint32_t offset per part; each part starts with its four-character code and its size.
int64_t find_dxbc_part(const Vector<uint8_t> &p_container, const char *p_fourcc, uint32_t *r_size = nullptr) {
	const uint8_t *bytes = p_container.ptr();
	const int64_t size = p_container.size();
	if (size < 32 || memcmp(bytes, "DXBC", 4) != 0) {
		return -1;
	}
	uint32_t part_count = 0;
	memcpy(&part_count, bytes + 28, sizeof(part_count));
	for (uint32_t i = 0; i < part_count && 32 + (i + 1) * 4 <= size; i++) {
		uint32_t offset = 0;
		memcpy(&offset, bytes + 32 + i * 4, sizeof(offset));
		if (int64_t(offset) + 8 <= size && memcmp(bytes + offset, p_fourcc, 4) == 0) {
			uint32_t part_size = 0;
			memcpy(&part_size, bytes + offset + 4, sizeof(part_size));
			if (int64_t(offset) + 8 + part_size > size) {
				return -1;
			}
			if (r_size != nullptr) {
				*r_size = part_size;
			}
			return int64_t(offset) + 8;
		}
	}
	return -1;
}

// The DXIL program header at the start of the DXIL part: (shader kind << 16) | (major << 4) | minor.
// Shader kinds from the public DXIL specification: compute 5, library 6.
uint32_t dxil_program_version(const Vector<uint8_t> &p_container) {
	uint32_t part_size = 0;
	const int64_t dxil = find_dxbc_part(p_container, "DXIL", &part_size);
	if (dxil < 0 || part_size < 4) {
		return 0;
	}
	uint32_t program_version = 0;
	memcpy(&program_version, p_container.ptr() + dxil, sizeof(program_version));
	return program_version;
}

// The [V] check of #123. With GODOT_D3D12_DXIL_VALIDATE=1 set (debug builds), the DXIL validator (dxil.dll from the
// Windows SDK) must be loaded, and must have checked exactly p_blobs more blobs since p_before without rejecting
// any. Without the variable, nothing is validated.
void check_dxil_validation(const DXILValidationStats &p_before, uint32_t p_blobs) {
	const DXILValidationStats after = RenderingShaderContainerD3D12::get_dxil_validation_stats();
	if (after.requested) {
		REQUIRE_MESSAGE(after.active, "GODOT_D3D12_DXIL_VALIDATE=1 is set, but the DXIL validator is not loaded: dxil.dll is missing or older than validator 1.8 (see the error printed before).");
		CHECK(after.validated - p_before.validated == p_blobs);
		CHECK(after.failed == p_before.failed);
	} else {
		CHECK_FALSE(after.active);
		CHECK(after.validated == 0);
		CHECK(after.failed == 0);
	}
}

// The DXIL of one shader, or an empty blob if there is none. REQUIRE does not stop a test case in builds without
// exceptions, so this must not index past the end after a failed REQUIRE.
Vector<uint8_t> decompressed_code(const Ref<RenderingShaderContainer> &p_container, int p_index) {
	if (p_index < 0 || p_index >= p_container->shaders.size()) {
		return Vector<uint8_t>();
	}
	const RenderingShaderContainer::Shader &shader = p_container->shaders[p_index];
	Vector<uint8_t> code;
	code.resize(shader.code_decompressed_size);
	p_container->decompress_code(shader.code_compressed_bytes.ptr(), shader.code_compressed_bytes.size(), shader.code_compression_flags, code.ptrw(), code.size());
	return code;
}

// GodotNext: dxil_atomics (#279), dxil_nonuniform (#293). Whether a GodotNext Mesa NIR library label ("25.3.1-gn3") is
// 25.3.1-gn<p_gn> or later, as drivers/d3d12/SCsub compares them. An empty label (the upstream library) is not.
bool mesa_label_is_at_least(const String &p_label, int p_gn) {
	const PackedStringArray parts = p_label.replace("-gn", ".").split(".");
	if (parts.size() != 4) {
		return false;
	}
	const int minimum[4] = { 25, 3, 1, p_gn };
	for (int i = 0; i < 4; i++) {
		const int part = parts[i].to_int();
		if (part != minimum[i]) {
			return part > minimum[i];
		}
	}
	return true;
}

// GodotNext: dxil_atomics (#279). 25.3.1-gn3 or later.
bool mesa_label_declares_atomics_without_readnone(const String &p_label) {
	return mesa_label_is_at_least(p_label, 3);
}

// GodotNext: dxil_nonuniform (#293). 25.3.1-gn4 or later.
bool mesa_label_marks_non_uniform_resource_indices(const String &p_label) {
	return mesa_label_is_at_least(p_label, 4);
}

TEST_CASE("[ShaderContainer][D3D12] D3D12 containers: format version, shader models and the ray-tracing gate") {
	CHECK(RenderingShaderContainerD3D12::FORMAT_VERSION == 4);
	// GodotNext: dxil_atomics (#279). The format version follows the library: 5 when it declares atomics without
	// readnone (25.3.1-gn3 or later), else 4 (3 and 2 before #121's ray-tracing stage data).
	CHECK(RenderingShaderContainerD3D12::FORMAT_VERSION_ATOMICS_WITHOUT_READNONE == 5);
	CHECK(RenderingShaderContainerD3D12::RAYTRACING_STAGE_DATA_FORMAT_VERSION == 4);
	CHECK(mesa_label_declares_atomics_without_readnone("25.3.1-gn3"));
	CHECK(mesa_label_declares_atomics_without_readnone("25.4.0-gn1"));
	CHECK_FALSE(mesa_label_declares_atomics_without_readnone("25.3.1-gn2"));
	CHECK_FALSE(mesa_label_declares_atomics_without_readnone(""));
	const bool atomics_without_readnone = RenderingShaderContainerD3D12::declares_atomics_without_readnone();
	CHECK(atomics_without_readnone == mesa_label_declares_atomics_without_readnone(RenderingShaderContainerD3D12::get_mesa_label()));
	// GodotNext: dxil_nonuniform (#293). 6 when the library also marks non-uniform resource indices (25.3.1-gn4 or
	// later), which also declares atomics without readnone.
	CHECK(RenderingShaderContainerD3D12::FORMAT_VERSION_NON_UNIFORM_RESOURCE_INDEX == 6);
	CHECK(mesa_label_marks_non_uniform_resource_indices("25.3.1-gn4"));
	CHECK(mesa_label_marks_non_uniform_resource_indices("25.3.2-gn1"));
	CHECK_FALSE(mesa_label_marks_non_uniform_resource_indices("25.3.1-gn3"));
	CHECK_FALSE(mesa_label_marks_non_uniform_resource_indices(""));
	const bool non_uniform_resource_indices = RenderingShaderContainerD3D12::marks_non_uniform_resource_indices();
	CHECK(non_uniform_resource_indices == mesa_label_marks_non_uniform_resource_indices(RenderingShaderContainerD3D12::get_mesa_label()));
	if (non_uniform_resource_indices) {
		CHECK(atomics_without_readnone);
	}
	// GodotNext: dxil_mesh_shaders (#122). 25.3.1-gn4 and later also translate task and mesh stages to DXIL.
	CHECK(RenderingShaderContainerD3D12::is_mesh_shading_supported() == mesa_label_is_at_least(RenderingShaderContainerD3D12::get_mesa_label(), 4));
	const uint32_t format_version = non_uniform_resource_indices
			? RenderingShaderContainerD3D12::FORMAT_VERSION_NON_UNIFORM_RESOURCE_INDEX
			: (atomics_without_readnone ? RenderingShaderContainerD3D12::FORMAT_VERSION_ATOMICS_WITHOUT_READNONE : RenderingShaderContainerD3D12::FORMAT_VERSION);
	CHECK(RenderingShaderContainerD3D12::get_library_format_version() == format_version);

	HMODULE lib_d3d12 = LoadLibraryW(L"d3d12.dll");
	REQUIRE(lib_d3d12 != nullptr);
	RenderingShaderContainerFormatD3D12 format;
	format.set_lib_d3d12(lib_d3d12);
	const DXILValidationStats validation_before = RenderingShaderContainerD3D12::get_dxil_validation_stats();

	// A plain compute shader compiles to compute DXIL at the required shader model (6.2).
	Ref<RenderingShaderContainer> plain = format.create_container();
	REQUIRE(plain->set_code_from_spirv("plain", make_stages(TestModule::COMPUTE)));
	REQUIRE(plain->shaders.size() == 1);
	CHECK(dxil_program_version(decompressed_code(plain, 0)) == ((5u << 16) | 0x62));

	// It is written as container version 6 and the library's D3D12 format version, and copies of versions 2 to 5 are
	// each rejected once.
	const PackedByteArray bytes = plain->to_bytes();
	REQUIRE(bytes.size() > HEADER_FORMAT_VERSION_OFFSET + 4);
	CHECK(read_u32(bytes, HEADER_VERSION_OFFSET) == 6);
	CHECK(read_u32(bytes, HEADER_FORMAT_VERSION_OFFSET) == format_version);
	CHECK(plain->get_current_format_version() == format_version);
	CHECK(plain->is_container_current(bytes));
	{
		Ref<RenderingShaderContainer> loaded = format.create_container();
		CHECK(loaded->from_bytes(bytes));
		CHECK(Object::cast_to<RenderingShaderContainerD3D12>(loaded.ptr())->get_shader_reflection_d3d12().has_raytracing_stage_data);
	}
	// GodotNext: d3d12_rt_pipelines (#121). Format versions 4 and 5 end with the ray-tracing stage data. Containers of
	// format versions 2 and 3 (version 2's layout, from a build before #121 with an older library or with 25.3.1-gn3)
	// still load without it, and say they have none, so ray-tracing pipelines refuse their stages.
	const uint32_t old_layout_format_versions[] = { 2, 3 };
	for (const uint32_t old_format_version : old_layout_format_versions) {
		constexpr int64_t raytracing_stage_data_size = sizeof(RenderingShaderContainerD3D12::RaytracingStageDataD3D12) * RenderingShaderContainerD3D12::RAYTRACING_STAGE_COUNT;
		PackedByteArray old_layout = bytes;
		old_layout.resize(bytes.size() - raytracing_stage_data_size);
		write_u32(old_layout, HEADER_FORMAT_VERSION_OFFSET, old_format_version);
		CHECK_FALSE(plain->is_container_current(old_layout));
		ErrorCapture capture;
		Ref<RenderingShaderContainer> loaded = format.create_container();
		CHECK(loaded->from_bytes(old_layout));
		CHECK(capture.count == 0);
		CHECK_FALSE(Object::cast_to<RenderingShaderContainerD3D12>(loaded.ptr())->get_shader_reflection_d3d12().has_raytracing_stage_data);
	}
	// A container another kind of library wrote is not current, so shader caches rebuild it.
	const uint32_t library_format_versions[] = { 4, 5, 6 };
	for (const uint32_t other_format_version : library_format_versions) {
		if (other_format_version == format_version) {
			continue;
		}
		PackedByteArray other = bytes;
		write_u32(other, HEADER_FORMAT_VERSION_OFFSET, other_format_version);
		uint32_t found = 0;
		CHECK_FALSE(plain->is_container_current(other, nullptr, &found));
		CHECK(found == other_format_version);
	}
	const uint32_t stale_versions[] = { 2, 3, 4, 5 };
	for (const uint32_t stale_version : stale_versions) {
		PackedByteArray stale = bytes;
		write_u32(stale, HEADER_VERSION_OFFSET, stale_version);
		ErrorCapture capture;
		Ref<RenderingShaderContainer> loaded = format.create_container();
		CHECK_FALSE(loaded->from_bytes(stale));
		CHECK(capture.count == 1);
	}

	Ref<RenderingShaderContainer> ray_query = format.create_container();
	Ref<RenderingShaderContainer> raygen = format.create_container();
	if (RenderingShaderContainerD3D12::is_raytracing_supported()) {
		// The GodotNext Mesa NIR library: ray query compiles at 6.5, and a ray-tracing stage is a 6.5 library.
		CHECK(String(RenderingShaderContainerD3D12::get_mesa_label()).contains("-gn"));
		{
			// No message from Mesa, nor from the DXIL validator when GODOT_D3D12_DXIL_VALIDATE=1 is set.
			ErrorCapture capture;
			REQUIRE(ray_query->set_code_from_spirv("ray_query", make_stages(TestModule::COMPUTE_RAY_QUERY)));
			REQUIRE(raygen->set_code_from_spirv("raygen", make_stages(TestModule::RAYGEN)));
			CHECK_MESSAGE(capture.count == 0, capture.first_message);
		}
		CHECK(dxil_program_version(decompressed_code(ray_query, 0)) == ((5u << 16) | 0x65));
		REQUIRE(raygen->shaders.size() == 1);
		CHECK(raygen->shaders[0].shader_stage == RDC::SHADER_STAGE_RAYGEN);
		CHECK(dxil_program_version(decompressed_code(raygen, 0)) == ((6u << 16) | 0x65));
		check_dxil_validation(validation_before, 3);
	} else {
		// The upstream library: both are refused with exactly one message each.
		CHECK(String(RenderingShaderContainerD3D12::get_mesa_label()).is_empty());
		CHECK_FALSE(atomics_without_readnone);
		{
			ErrorCapture capture;
			CHECK_FALSE(ray_query->set_code_from_spirv("ray_query", make_stages(TestModule::COMPUTE_RAY_QUERY)));
			CHECK(capture.count == 1);
			CHECK_MESSAGE(capture.first_message.contains("uses ray query"), capture.first_message);
		}
		{
			ErrorCapture capture;
			CHECK_FALSE(raygen->set_code_from_spirv("raygen", make_stages(TestModule::RAYGEN)));
			CHECK(capture.count == 1);
			CHECK_MESSAGE(capture.first_message.contains("uses ray-tracing stages"), capture.first_message);
		}
		check_dxil_validation(validation_before, 1);
	}
}

// The validator itself, so that "0 validator errors" rests on a check that can fail: a compiled blob passes, and the
// same blob with a damaged shader-feature part is rejected with one error message. The failure counts toward the
// "D3D12: DXIL validation checked N blobs; M failed." total that the process prints at exit.
TEST_CASE("[ShaderContainer][D3D12] The DXIL validator accepts a compiled blob and rejects a damaged one") {
	HMODULE lib_d3d12 = LoadLibraryW(L"d3d12.dll");
	REQUIRE(lib_d3d12 != nullptr);
	RenderingShaderContainerFormatD3D12 format;
	format.set_lib_d3d12(lib_d3d12);
	Ref<RenderingShaderContainer> plain = format.create_container();
	REQUIRE(plain->set_code_from_spirv("plain", make_stages(TestModule::COMPUTE)));
	REQUIRE(plain->shaders.size() == 1);
	const Vector<uint8_t> dxil = decompressed_code(plain, 0);

	// The shader-feature part (SFI0) is one 64-bit mask of the optional features the module uses. Claiming doubles
	// (bit 0), which this shader does not use, makes the part disagree with the module.
	Vector<uint8_t> damaged = dxil;
	uint32_t features_size = 0;
	const int64_t features = find_dxbc_part(damaged, "SFI0", &features_size);
	REQUIRE(features >= 0);
	REQUIRE(features_size == 8);
	REQUIRE((damaged[features] & 0x01) == 0);
	damaged.write[features] |= 0x01;

	const DXILValidationStats before = RenderingShaderContainerD3D12::get_dxil_validation_stats();
	if (before.requested) {
		REQUIRE_MESSAGE(before.active, "GODOT_D3D12_DXIL_VALIDATE=1 is set, but the DXIL validator is not loaded: dxil.dll is missing or older than validator 1.8 (see the error printed before).");
		{
			ErrorCapture capture;
			CHECK(RenderingShaderContainerD3D12::validate_dxil("plain", RDC::SHADER_STAGE_COMPUTE, dxil));
			CHECK_MESSAGE(capture.count == 0, capture.first_message);
		}
		{
			ErrorCapture capture;
			CHECK_FALSE(RenderingShaderContainerD3D12::validate_dxil("damaged_on_purpose", RDC::SHADER_STAGE_COMPUTE, damaged));
			CHECK(capture.count == 1);
			CHECK_MESSAGE(capture.first_message.contains("DXIL validation failed for shader 'damaged_on_purpose'"), capture.first_message);
		}
		const DXILValidationStats after = RenderingShaderContainerD3D12::get_dxil_validation_stats();
		CHECK(after.validated - before.validated == 2);
		CHECK(after.failed - before.failed == 1);
	} else {
		// Without GODOT_D3D12_DXIL_VALIDATE=1, validation stays off: nothing is checked and nothing is printed.
		ErrorCapture capture;
		CHECK(RenderingShaderContainerD3D12::validate_dxil("plain", RDC::SHADER_STAGE_COMPUTE, dxil));
		CHECK(RenderingShaderContainerD3D12::validate_dxil("damaged_on_purpose", RDC::SHADER_STAGE_COMPUTE, damaged));
		CHECK(capture.count == 0);
		const DXILValidationStats after = RenderingShaderContainerD3D12::get_dxil_validation_stats();
		CHECK_FALSE(after.active);
		CHECK(after.validated == 0);
		CHECK(after.failed == 0);
	}
}

// The shader compiler is the glslang module, so this case exists only in builds that have it (the minimal template
// disables every module), like test_nextgi_rq_toolchain.cpp.
#ifdef MODULE_GLSLANG_ENABLED
// The RD smoke test's ray-query kernel (godotnext/testbed/tests/rd_smoke/rq_kernel.glsl): opaque traversal, one
// rayQueryProceedEXT and the committed getters, with an acceleration structure, storage buffers and push constants.
const char *RAY_QUERY_COMPUTE_GLSL = R"(
#version 460
#extension GL_EXT_ray_query : require

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

struct SmokeRay {
	vec4 origin_tmin;
	vec4 dir_tmax;
};

layout(set = 0, binding = 1, std430) restrict readonly buffer Rays {
	SmokeRay data[];
}
rays;

struct Result {
	float t;
	uint custom_index;
	uint primitive;
	float u;
	float v;
	uint front_face;
	uint committed;
	uint candidate_count;
};

layout(set = 0, binding = 2, std430) restrict writeonly buffer Results {
	Result data[];
}
results;

layout(push_constant, std430) uniform Params {
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.count) {
		return;
	}
	SmokeRay smoke_ray = rays.data[i];
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, smoke_ray.origin_tmin.xyz, smoke_ray.origin_tmin.w, smoke_ray.dir_tmax.xyz, smoke_ray.dir_tmax.w);
	rayQueryProceedEXT(rq);

	Result result;
	result.t = -1.0;
	result.custom_index = 0u;
	result.primitive = 0u;
	result.u = 0.0;
	result.v = 0.0;
	result.front_face = 0u;
	result.committed = 0u;
	result.candidate_count = 0u;
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		result.t = rayQueryGetIntersectionTEXT(rq, true);
		result.custom_index = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
		result.primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
		result.u = bary.x;
		result.v = bary.y;
		result.front_face = rayQueryGetIntersectionFrontFaceEXT(rq, true) ? 1u : 0u;
		result.committed = 1u;
		result.candidate_count = 1u;
	}
	results.data[i] = result;
}
)";

// A ray-tracing pipeline: the raygen traces into an acceleration structure and stores the payload in an image; it
// also reads push constants and a specialization constant. The closest-hit and miss stages fill the payload.
const char *RAYGEN_GLSL = R"(
#version 460
#extension GL_EXT_ray_tracing : require

layout(constant_id = 0) const bool FLIP_Y = false;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, rgba32f) uniform restrict writeonly image2D result_image;

layout(push_constant, std430) uniform Params {
	float t_max;
	uint cull_mask;
	uint pad0;
	uint pad1;
}
params;

layout(location = 0) rayPayloadEXT vec4 payload;

void main() {
	vec2 uv = (vec2(gl_LaunchIDEXT.xy) + 0.5) / vec2(gl_LaunchSizeEXT.xy);
	if (FLIP_Y) {
		uv.y = 1.0 - uv.y;
	}
	payload = vec4(0.0);
	traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, params.cull_mask & 0xFFu, 0, 1, 0, vec3(uv * 2.0 - 1.0, -1.0), 0.001, vec3(0.0, 0.0, 1.0), params.t_max, 0);
	imageStore(result_image, ivec2(gl_LaunchIDEXT.xy), payload);
}
)";

const char *CLOSEST_HIT_GLSL = R"(
#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec4 payload;
hitAttributeEXT vec2 barycentrics;

void main() {
	float front = gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT ? 0.5 : 0.0;
	payload = vec4(barycentrics, gl_HitTEXT, float(gl_InstanceCustomIndexEXT) + float(gl_PrimitiveID) + front);
}
)";

const char *MISS_GLSL = R"(
#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec4 payload;

void main() {
	payload = vec4(0.0, 0.0, -1.0, gl_WorldRayDirectionEXT.z);
}
)";

// A procedural hit group: the intersection stage reports hits on a unit sphere, and the any-hit stage ignores some.
const char *ANY_HIT_GLSL = R"(
#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec4 payload;
hitAttributeEXT vec2 attributes;

void main() {
	if (attributes.y > 0.5) {
		ignoreIntersectionEXT;
	}
	payload.w += 1.0;
}
)";

const char *INTERSECTION_GLSL = R"(
#version 460
#extension GL_EXT_ray_tracing : require

hitAttributeEXT vec2 attributes;

void main() {
	vec3 origin = gl_ObjectRayOriginEXT;
	vec3 direction = gl_ObjectRayDirectionEXT;
	float a = dot(direction, direction);
	float b = dot(origin, direction);
	float discriminant = b * b - a * (dot(origin, origin) - 1.0);
	if (discriminant >= 0.0) {
		float t = (-b - sqrt(discriminant)) / a;
		attributes = vec2(t, discriminant);
		reportIntersectionEXT(t, 0u);
	}
}
)";

// Compiles one stage with the SPIR-V targets the D3D12 container format asks for.
RDC::ShaderStageSPIRVData compile_glsl_stage(const RenderingShaderContainerFormat &p_format, RDC::ShaderStage p_stage, const char *p_source) {
	String error;
	RDC::ShaderStageSPIRVData stage;
	stage.shader_stage = p_stage;
	stage.spirv = compile_glslang_shader(p_stage, String::utf8(p_source), p_format.get_shader_language_version(), p_format.get_shader_spirv_version(p_stage), &error);
	CHECK_MESSAGE(!stage.spirv.is_empty(), vformat("glslang failed to compile the %s stage:\n%s", RDC::SHADER_STAGE_NAMES[p_stage], error));
	return stage;
}

// The [V] check of #123. Run it with GODOT_D3D12_DXIL_VALIDATE=1 set, and every DXIL blob below also goes through
// the DXIL validator (dxil.dll from the Windows SDK) before it is signed: check_dxil_validation() then requires the
// validator to be loaded and to have checked each blob without rejecting any.
TEST_CASE("[ShaderContainer][D3D12] GLSL ray query and a raygen, any-hit, closest-hit, miss and intersection library compile to DXIL") {
	HMODULE lib_d3d12 = LoadLibraryW(L"d3d12.dll");
	REQUIRE(lib_d3d12 != nullptr);
	RenderingShaderContainerFormatD3D12 format;
	format.set_lib_d3d12(lib_d3d12);

	Vector<RDC::ShaderStageSPIRVData> ray_query_stages;
	ray_query_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_COMPUTE, RAY_QUERY_COMPUTE_GLSL));
	Vector<RDC::ShaderStageSPIRVData> library_stages;
	library_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_RAYGEN, RAYGEN_GLSL));
	library_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_CLOSEST_HIT, CLOSEST_HIT_GLSL));
	library_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_MISS, MISS_GLSL));
	Vector<RDC::ShaderStageSPIRVData> hit_group_stages;
	hit_group_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_ANY_HIT, ANY_HIT_GLSL));
	hit_group_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_INTERSECTION, INTERSECTION_GLSL));
	REQUIRE_FALSE(ray_query_stages[0].spirv.is_empty());
	for (const RDC::ShaderStageSPIRVData &stage : library_stages) {
		REQUIRE_FALSE(stage.spirv.is_empty());
	}
	for (const RDC::ShaderStageSPIRVData &stage : hit_group_stages) {
		REQUIRE_FALSE(stage.spirv.is_empty());
	}

	const DXILValidationStats validation_before = RenderingShaderContainerD3D12::get_dxil_validation_stats();
	Ref<RenderingShaderContainer> ray_query = format.create_container();
	Ref<RenderingShaderContainer> library = format.create_container();
	Ref<RenderingShaderContainer> hit_group = format.create_container();
	if (RenderingShaderContainerD3D12::is_raytracing_supported()) {
		{
			ErrorCapture capture;
			REQUIRE(ray_query->set_code_from_spirv("ray_query_kernel", ray_query_stages));
			REQUIRE(library->set_code_from_spirv("raytracing_library", library_stages));
			REQUIRE(hit_group->set_code_from_spirv("procedural_hit_group", hit_group_stages));
			CHECK_MESSAGE(capture.count == 0, capture.first_message);
		}

		// Ray query: DXIL compute at shader model 6.5.
		REQUIRE(ray_query->shaders.size() == 1);
		CHECK(dxil_program_version(decompressed_code(ray_query, 0)) == ((5u << 16) | 0x65));

		// Each ray-tracing stage is its own DXIL library (shader kind 6) at shader model 6.5.
		uint32_t stages_found = 0;
		for (const Ref<RenderingShaderContainer> &container : { library, hit_group }) {
			for (int i = 0; i < container->shaders.size(); i++) {
				stages_found |= 1u << container->shaders[i].shader_stage;
				CHECK_MESSAGE(dxil_program_version(decompressed_code(container, i)) == ((6u << 16) | 0x65), String(RDC::SHADER_STAGE_NAMES[container->shaders[i].shader_stage]));
			}
		}
		CHECK(library->shaders.size() == 3);
		CHECK(hit_group->shaders.size() == 2);
		CHECK(stages_found == (RDC::SHADER_STAGE_RAYGEN_BIT | RDC::SHADER_STAGE_ANY_HIT_BIT | RDC::SHADER_STAGE_CLOSEST_HIT_BIT | RDC::SHADER_STAGE_MISS_BIT | RDC::SHADER_STAGE_INTERSECTION_BIT));

		// The raygen's push constants become root constants, and its specialization constant gets a bitcode offset
		// in the raygen slot of the D3D12 format version 2 table, so pipelines can patch it.
		const RenderingShaderContainerD3D12 *library_d3d12 = Object::cast_to<RenderingShaderContainerD3D12>(library.ptr());
		REQUIRE(library_d3d12 != nullptr);
		const RenderingShaderContainerD3D12::ShaderReflectionD3D12 reflection = library_d3d12->get_shader_reflection_d3d12();
		CHECK((reflection.dxil_push_constant_stages & RDC::SHADER_STAGE_RAYGEN_BIT) != 0);
		REQUIRE(reflection.reflection_specialization_data_d3d12.size() == 1);
		const uint64_t (&bit_offsets)[D3D12_BITCODE_OFFSETS_NUM_STAGES] = reflection.reflection_specialization_data_d3d12[0].stages_bit_offsets;
		CHECK(bit_offsets[RenderingShaderContainerD3D12::SHADER_STAGES_BIT_OFFSET_INDICES[RDC::SHADER_STAGE_RAYGEN]] != 0);
		CHECK(bit_offsets[RenderingShaderContainerD3D12::SHADER_STAGES_BIT_OFFSET_INDICES[RDC::SHADER_STAGE_CLOSEST_HIT]] == 0);
		CHECK(bit_offsets[RenderingShaderContainerD3D12::SHADER_STAGES_BIT_OFFSET_INDICES[RDC::SHADER_STAGE_COMPUTE]] == 0);

		// One ray-query blob, three library blobs and two hit-group blobs.
		check_dxil_validation(validation_before, 6);
	} else {
		// The upstream library: each is refused with exactly one message.
		{
			ErrorCapture capture;
			CHECK_FALSE(ray_query->set_code_from_spirv("ray_query_kernel", ray_query_stages));
			CHECK(capture.count == 1);
			CHECK_MESSAGE(capture.first_message.contains("uses ray query"), capture.first_message);
		}
		{
			ErrorCapture capture;
			CHECK_FALSE(library->set_code_from_spirv("raytracing_library", library_stages));
			CHECK(capture.count == 1);
			CHECK_MESSAGE(capture.first_message.contains("uses ray-tracing stages"), capture.first_message);
		}
		{
			ErrorCapture capture;
			CHECK_FALSE(hit_group->set_code_from_spirv("procedural_hit_group", hit_group_stages));
			CHECK(capture.count == 1);
			CHECK_MESSAGE(capture.first_message.contains("uses ray-tracing stages"), capture.first_message);
		}
		check_dxil_validation(validation_before, 0);
	}
}

// GodotNext: rd_mesh_shaders (#178), dxil_mesh_shaders (#122). With 25.3.1-gn4 or later, task and mesh stages
// compile to Shader Model 6.5 amplification (kind 14) and mesh (kind 13) DXIL; older libraries refuse them with one
// named error.
const char *MESH_SPEC_AND_CULL_TASK_GLSL = R"(#version 460
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 8, local_size_y = 1, local_size_z = 1) in;
layout(constant_id = 0) const uint GROUP_SCALE = 1u;

struct VecPayload {
	uvec4 counts;
};
taskPayloadSharedEXT VecPayload payload;

void main() {
	if (gl_LocalInvocationIndex == 0u) {
		payload.counts = uvec4(gl_NumWorkGroups.xyz, GROUP_SCALE);
	}
	EmitMeshTasksEXT(GROUP_SCALE, 1u, 1u);
}
)";

const char *MESH_SPEC_AND_CULL_LINES_GLSL = R"(#version 460
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 8, local_size_y = 1, local_size_z = 1) in;
layout(lines, max_vertices = 2, max_primitives = 1) out;
layout(constant_id = 0) const uint GROUP_SCALE = 1u;

struct VecPayload {
	uvec4 counts;
};
taskPayloadSharedEXT VecPayload payload;

layout(location = 0) out vec4 vertex_color[];

void main() {
	SetMeshOutputsEXT(2, 1);
	if (gl_LocalInvocationIndex == 0u) {
		gl_MeshVerticesEXT[0].gl_Position = vec4(-0.5, 0.0, 0.0, 1.0);
		gl_MeshVerticesEXT[1].gl_Position = vec4(0.5, 0.0, 0.0, 1.0);
		vertex_color[0] = vec4(float(payload.counts.x + GROUP_SCALE), 0.0, 0.0, 1.0);
		vertex_color[1] = vec4(0.0, 1.0, 0.0, 1.0);
		gl_PrimitiveLineIndicesEXT[0] = uvec2(0u, 1u);
		gl_MeshPrimitivesEXT[0].gl_CullPrimitiveEXT = (payload.counts.w == 0u);
	}
}
)";

const char *MESH_SPEC_FRAGMENT_GLSL = R"(#version 460

layout(constant_id = 0) const uint GROUP_SCALE = 1u;
layout(location = 0) in vec4 vertex_color;
layout(location = 0) out vec4 frag_color;

void main() {
	frag_color = vertex_color * float(GROUP_SCALE);
}
)";

TEST_CASE("[ShaderContainer][D3D12] Task and mesh stages compile to SM 6.5 amplification and mesh DXIL with 25.3.1-gn4 or later") {
	HMODULE lib_d3d12 = LoadLibraryW(L"d3d12.dll");
	REQUIRE(lib_d3d12 != nullptr);
	RenderingShaderContainerFormatD3D12 format;
	format.set_lib_d3d12(lib_d3d12);
	// The shader baker's gate follows the linked library's label (drivers/d3d12/SCsub), not the container's own flag.
	CHECK(format.supports_mesh_shaders() == mesa_label_is_at_least(RenderingShaderContainerD3D12::get_mesa_label(), 4));

	const Vector<RDC::ShaderStageSPIRVData> stages = task_mesh_fragment_stages();
	REQUIRE(stages.size() == 3);
	for (const RDC::ShaderStageSPIRVData &stage : stages) {
		REQUIRE_FALSE(stage.spirv.is_empty());
	}

	const DXILValidationStats validation_before = RenderingShaderContainerD3D12::get_dxil_validation_stats();
	Ref<RenderingShaderContainer> container = format.create_container();
	if (!RenderingShaderContainerD3D12::is_mesh_shading_supported()) {
		ErrorCapture capture;
		CHECK_FALSE(container->set_code_from_spirv("task_mesh", stages));
		CHECK(capture.count == 1);
		CHECK_MESSAGE(capture.first_message.contains("Shader 'task_mesh' has task or mesh shader stages"), capture.first_message);
		CHECK(container->shaders.is_empty());
		check_dxil_validation(validation_before, 0);
		return;
	}

	Vector<RDC::ShaderStageSPIRVData> mesh_only_stages;
	mesh_only_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_MESH, MESH_ONLY_GLSL));
	mesh_only_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_FRAGMENT, MESH_FRAGMENT_GLSL));

	Vector<RDC::ShaderStageSPIRVData> spec_cull_stages;
	spec_cull_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_TASK, MESH_SPEC_AND_CULL_TASK_GLSL));
	spec_cull_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_MESH, MESH_SPEC_AND_CULL_LINES_GLSL));
	spec_cull_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_FRAGMENT, MESH_SPEC_FRAGMENT_GLSL));

	Ref<RenderingShaderContainer> mesh_only = format.create_container();
	Ref<RenderingShaderContainer> spec_cull = format.create_container();
	{
		ErrorCapture capture;
		REQUIRE(container->set_code_from_spirv("task_mesh", stages));
		REQUIRE(mesh_only->set_code_from_spirv("mesh_only", mesh_only_stages));
		REQUIRE(spec_cull->set_code_from_spirv("mesh_spec_cull", spec_cull_stages));
		CHECK_MESSAGE(capture.count == 0, capture.first_message);
	}

	// DXIL shader kinds from the public specification: pixel 0, mesh 13, amplification 14, all at Shader Model 6.5.
	REQUIRE(container->shaders.size() == 3);
	REQUIRE(mesh_only->shaders.size() == 2);
	REQUIRE(spec_cull->shaders.size() == 3);
	for (int i = 0; i < container->shaders.size(); i++) {
		const RDC::ShaderStage stage = container->shaders[i].shader_stage;
		const uint32_t expected_kind = stage == RDC::SHADER_STAGE_TASK ? 14u : (stage == RDC::SHADER_STAGE_MESH ? 13u : 0u);
		CHECK_MESSAGE(dxil_program_version(decompressed_code(container, i)) == ((expected_kind << 16) | 0x65), String(RDC::SHADER_STAGE_NAMES[stage]));
	}
	for (int i = 0; i < mesh_only->shaders.size(); i++) {
		const RDC::ShaderStage stage = mesh_only->shaders[i].shader_stage;
		const uint32_t expected_kind = stage == RDC::SHADER_STAGE_MESH ? 13u : 0u;
		CHECK_MESSAGE(dxil_program_version(decompressed_code(mesh_only, i)) == ((expected_kind << 16) | 0x65), String(RDC::SHADER_STAGE_NAMES[stage]));
	}

	Ref<RenderingShaderContainer> loaded = format.create_container();
	REQUIRE(loaded->from_bytes(container->to_bytes()));
	check_task_mesh_reflection(loaded->get_shader_reflection());
	const RenderingShaderContainerD3D12 *loaded_d3d12 = Object::cast_to<RenderingShaderContainerD3D12>(loaded.ptr());
	REQUIRE(loaded_d3d12 != nullptr);
	CHECK((loaded_d3d12->get_shader_reflection_d3d12().dxil_push_constant_stages & RDC::SHADER_STAGE_MESH_BIT) != 0);
	CHECK(!loaded_d3d12->get_shader_reflection_d3d12().root_signature_bytes.is_empty());

	// Specialization constants in task, mesh and fragment stages each get their own bitcode offset, and
	// gl_NumWorkGroups in the task stage requests NIR compute runtime data in the root signature.
	const RenderingShaderContainerD3D12 *spec_d3d12 = Object::cast_to<RenderingShaderContainerD3D12>(spec_cull.ptr());
	REQUIRE(spec_d3d12 != nullptr);
	const RenderingShaderContainerD3D12::ShaderReflectionD3D12 spec_refl = spec_d3d12->get_shader_reflection_d3d12();
	CHECK(spec_refl.nir_runtime_data_root_param_idx != UINT32_MAX);
	REQUIRE(spec_refl.reflection_specialization_data_d3d12.size() == 1);
	const uint64_t (&bit_offsets)[D3D12_BITCODE_OFFSETS_NUM_STAGES] = spec_refl.reflection_specialization_data_d3d12[0].stages_bit_offsets;
	CHECK(bit_offsets[RenderingShaderContainerD3D12::SHADER_STAGES_BIT_OFFSET_INDICES[RDC::SHADER_STAGE_TASK]] != 0);
	CHECK(bit_offsets[RenderingShaderContainerD3D12::SHADER_STAGES_BIT_OFFSET_INDICES[RDC::SHADER_STAGE_MESH]] != 0);
	CHECK(bit_offsets[RenderingShaderContainerD3D12::SHADER_STAGES_BIT_OFFSET_INDICES[RDC::SHADER_STAGE_FRAGMENT]] != 0);
	HashMap<RDC::ShaderStage, Vector<uint8_t>> stage_bytecodes;
	for (int i = 0; i < spec_cull->shaders.size(); i++) {
		stage_bytecodes.insert(spec_cull->shaders[i].shader_stage, decompressed_code(spec_cull, i));
	}
	const int32_t patched_scale = 3;
	const uint32_t patched_mask = RenderingDXIL::patch_specialization_constant(RDC::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT, &patched_scale, bit_offsets, stage_bytecodes, false);
	CHECK(patched_mask == uint32_t(RDC::SHADER_STAGE_TASK_BIT | RDC::SHADER_STAGE_MESH_BIT | RDC::SHADER_STAGE_FRAGMENT_BIT));

	check_dxil_validation(validation_before, 8);

	// gl_NumWorkGroups: a mesh stage without a task stage reads the draw's group counts from the NIR runtime data. Behind
	// a task stage its counts are the task stage's EmitMeshTasksEXT() arguments, so the read is refused with one error.
	{
		const DXILValidationStats num_workgroups_before = RenderingShaderContainerD3D12::get_dxil_validation_stats();
		const String direct_source = String::utf8(MESH_ONLY_GLSL).replace("primitive_id[0] = gl_WorkGroupID.x;", "primitive_id[0] = gl_WorkGroupID.x + gl_NumWorkGroups.x;");
		const String behind_task_source = String::utf8(MESH_MESH_GLSL).replace("+ params.base;", "+ params.base + gl_NumWorkGroups.x;");
		CHECK(direct_source.contains("gl_NumWorkGroups"));
		CHECK(behind_task_source.contains("gl_NumWorkGroups"));

		Vector<RDC::ShaderStageSPIRVData> direct_stages;
		direct_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_MESH, direct_source.utf8().get_data()));
		direct_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_FRAGMENT, MESH_FRAGMENT_GLSL));
		Ref<RenderingShaderContainer> direct = format.create_container();
		{
			ErrorCapture capture;
			CHECK(direct->set_code_from_spirv("mesh_num_workgroups", direct_stages));
			CHECK_MESSAGE(capture.count == 0, capture.first_message);
		}
		const RenderingShaderContainerD3D12 *direct_d3d12 = Object::cast_to<RenderingShaderContainerD3D12>(direct.ptr());
		CHECK(direct_d3d12 != nullptr);
		if (direct_d3d12 != nullptr) {
			CHECK(direct_d3d12->get_shader_reflection_d3d12().nir_runtime_data_root_param_idx != UINT32_MAX);
		}

		Vector<RDC::ShaderStageSPIRVData> behind_task_stages;
		behind_task_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_TASK, MESH_TASK_GLSL));
		behind_task_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_MESH, behind_task_source.utf8().get_data()));
		behind_task_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_FRAGMENT, MESH_FRAGMENT_GLSL));
		Ref<RenderingShaderContainer> behind_task = format.create_container();
		ErrorCapture capture;
		CHECK_FALSE(behind_task->set_code_from_spirv("mesh_num_workgroups_behind_task", behind_task_stages));
		CHECK(capture.count == 1);
		CHECK_MESSAGE(capture.first_message.contains("Shader 'mesh_num_workgroups_behind_task' reads gl_NumWorkGroups in a mesh shader that follows a task shader"), capture.first_message);
		CHECK(behind_task->shaders.is_empty());
		check_dxil_validation(num_workgroups_before, 2);
	}

	// gl_DrawID in a task or mesh stage and a mesh stage that outputs points are refused with one error each: Direct3D 12
	// has no draw index in these stages (an indirect mesh draw record carries only the group counts), and its mesh
	// shaders output lines or triangles only.
	{
		const DXILValidationStats refused_before = RenderingShaderContainerD3D12::get_dxil_validation_stats();
		const String mesh_draw_id_source = String::utf8(MESH_ONLY_GLSL).replace("primitive_id[0] = gl_WorkGroupID.x;", "primitive_id[0] = gl_WorkGroupID.x + uint(gl_DrawID);");
		const String task_draw_id_source = String::utf8(MESH_TASK_GLSL).replace("EmitMeshTasksEXT(32, 1, 1);", "EmitMeshTasksEXT(32u + uint(gl_DrawID), 1u, 1u);");
		const String points_source = String::utf8(MESH_ONLY_GLSL).replace("layout(triangles, max_vertices = 3, max_primitives = 1) out;", "layout(points, max_vertices = 3, max_primitives = 1) out;").replace("gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);", "gl_PrimitivePointIndicesEXT[0] = 0u;");
		CHECK(mesh_draw_id_source.contains("gl_DrawID"));
		CHECK(task_draw_id_source.contains("gl_DrawID"));
		CHECK(points_source.contains("layout(points"));
		CHECK(points_source.contains("gl_PrimitivePointIndicesEXT"));

		Vector<RDC::ShaderStageSPIRVData> mesh_draw_id_stages;
		mesh_draw_id_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_MESH, mesh_draw_id_source.utf8().get_data()));
		mesh_draw_id_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_FRAGMENT, MESH_FRAGMENT_GLSL));
		Vector<RDC::ShaderStageSPIRVData> task_draw_id_stages;
		task_draw_id_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_TASK, task_draw_id_source.utf8().get_data()));
		task_draw_id_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_MESH, MESH_MESH_GLSL));
		task_draw_id_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_FRAGMENT, MESH_FRAGMENT_GLSL));
		Vector<RDC::ShaderStageSPIRVData> points_stages;
		points_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_MESH, points_source.utf8().get_data()));
		points_stages.push_back(compile_mesh_case_stage(RDC::SHADER_STAGE_FRAGMENT, MESH_FRAGMENT_GLSL));

		struct RefusedCase {
			const char *name;
			const Vector<RDC::ShaderStageSPIRVData> *stages;
			const char *message;
		};
		const RefusedCase refused_cases[] = {
			{ "mesh_draw_id", &mesh_draw_id_stages, "Shader 'mesh_draw_id' reads gl_DrawID in a mesh shader, which Direct3D 12 cannot provide." },
			{ "task_draw_id", &task_draw_id_stages, "Shader 'task_draw_id' reads gl_DrawID in a task shader, which Direct3D 12 cannot provide." },
			{ "mesh_points", &points_stages, "Shader 'mesh_points' has a mesh shader that outputs points (layout(points) out), which Direct3D 12 does not support" },
		};
		for (const RefusedCase &refused_case : refused_cases) {
			Ref<RenderingShaderContainer> refused = format.create_container();
			ErrorCapture capture;
			CHECK_FALSE(refused->set_code_from_spirv(refused_case.name, *refused_case.stages));
			CHECK(capture.count == 1);
			CHECK_MESSAGE(capture.first_message.contains(refused_case.message), capture.first_message);
			CHECK(refused->shaders.is_empty());
		}
		check_dxil_validation(refused_before, 0);
	}

#ifdef DEBUG_ENABLED
	// Check the emitted DXIL mesh/amplification operations: setMeshOutputCounts (168), emitIndices (169),
	// getMeshPayload (170), storeVertexOutput (171), storePrimitiveOutput (172) and dispatchMesh (173).
	String error;
	const Vector<uint8_t> as_blob = decompressed_code(container, 0);
	const Vector<uint8_t> ms_blob = decompressed_code(container, 1);
	const String as_disasm = RenderingShaderContainerD3D12::disassemble_dxil(as_blob, &error);
	CHECK_MESSAGE(!as_disasm.is_empty(), error);
	if (!as_disasm.is_empty()) {
		CHECK_MESSAGE(as_disasm.find("@dx.op.dispatchMesh.") >= 0, as_disasm);
		CHECK_MESSAGE(as_disasm.contains("addrspace(3)*"), as_disasm);
	}
	const String ms_disasm = RenderingShaderContainerD3D12::disassemble_dxil(ms_blob, &error);
	CHECK_MESSAGE(!ms_disasm.is_empty(), error);
	if (!ms_disasm.is_empty()) {
		CHECK_MESSAGE(ms_disasm.find("@dx.op.getMeshPayload.") >= 0, ms_disasm);
		CHECK_MESSAGE(ms_disasm.contains("@dx.op.setMeshOutputCounts(i32 168,"), ms_disasm);
		CHECK_MESSAGE(ms_disasm.contains("@dx.op.emitIndices(i32 169,"), ms_disasm);
		CHECK_MESSAGE(ms_disasm.find("@dx.op.storeVertexOutput.") >= 0, ms_disasm);
		CHECK_MESSAGE(ms_disasm.find("@dx.op.storePrimitiveOutput.") >= 0, ms_disasm);
	}
	const String cull_disasm = RenderingShaderContainerD3D12::disassemble_dxil(decompressed_code(spec_cull, 1), &error);
	CHECK_MESSAGE(!cull_disasm.is_empty(), error);
	if (!cull_disasm.is_empty()) {
		CHECK_MESSAGE(cull_disasm.contains("SV_CullPrimitive"), cull_disasm);
	}
#endif
}

// The DXIL disassembly comes from the Windows SDK's dxcompiler.dll, through a hook that exists only with DEBUG_ENABLED.
#ifdef DEBUG_ENABLED
// A ray-query kernel that reads one instance value, candidate and committed. GETTER is replaced by the SPIR-V getter.
const char *RAY_QUERY_INSTANCE_GLSL = R"(
#version 460
#extension GL_EXT_ray_query : require

layout(local_size_x = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Results {
	uint candidate;
	uint committed;
}
results;

void main() {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsNoneEXT, 0xFFu, vec3(0.0), 0.0, vec3(0.0, 0.0, 1.0), 100.0);
	while (rayQueryProceedEXT(rq)) {
		if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
			results.candidate = GETTER(rq, false);
			rayQueryConfirmIntersectionEXT(rq);
		}
	}
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		results.committed = GETTER(rq, true);
	}
}
)";

// A closest-hit stage that returns one instance value. BUILTIN is replaced by the GLSL built-in. The GodotNext Mesa NIR
// library translates a closest-hit or any-hit stage only when it declares both a payload and a hit attribute (the
// DXIL entry function takes both); without the hit attribute, translation fails. So this stage reads its barycentrics.
const char *CLOSEST_HIT_INSTANCE_GLSL = R"(
#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT uint payload;
hitAttributeEXT vec2 barycentrics;

void main() {
	payload = uint(BUILTIN) + (barycentrics.x > 1.0 ? 1u : 0u);
}
)";

// Whether the DXIL disassembly calls the DXIL operation with this opcode. Every dx.op function takes its opcode as the
// first argument, as in "call i32 @dx.op.instanceID.i32(i32 141)".
bool dxil_calls_op(const String &p_disassembly, int p_opcode) {
	const String opcode = vformat("(i32 %d", p_opcode);
	for (const String &line : p_disassembly.split("\n")) {
		const int call = line.find("@dx.op.");
		const int paren = call < 0 ? -1 : line.find_char('(', call);
		if (paren < 0) {
			continue;
		}
		const String arguments = line.substr(paren);
		if (arguments.begins_with(opcode + ",") || arguments.begins_with(opcode + ")")) {
			return true;
		}
	}
	return false;
}

// The disassembly lines that call a DXIL operation reading an instance value, for the failure messages.
String dxil_instance_op_lines(const String &p_disassembly) {
	String lines;
	for (const String &line : p_disassembly.split("\n")) {
		if (line.contains("@dx.op.rayQuery_StateScalar") || line.contains("@dx.op.instance")) {
			lines += line.strip_edges() + "\n";
		}
	}
	return lines;
}

// SPIR-V and DXR name the two instance values differently. The instance custom index (Vulkan instanceCustomIndex, the
// 24-bit value the application stores in the instance) is DXIL InstanceID; the instance id (the instance's position in
// the top-level acceleration structure) is DXIL InstanceIndex. 25.3.1-gn1 swapped them in ray query, so a ray query
// read the TLAS position where it asked for the custom index. DXIL opcodes from the public DXIL operation table.
TEST_CASE("[ShaderContainer][D3D12] Ray query and ray-tracing stages read the instance custom index as DXIL InstanceID and the instance id as InstanceIndex") {
	constexpr int OP_INSTANCE_ID = 141;
	constexpr int OP_INSTANCE_INDEX = 142;
	constexpr int OP_RAY_QUERY_CANDIDATE_INSTANCE_INDEX = 201;
	constexpr int OP_RAY_QUERY_CANDIDATE_INSTANCE_ID = 202;
	constexpr int OP_RAY_QUERY_COMMITTED_INSTANCE_INDEX = 207;
	constexpr int OP_RAY_QUERY_COMMITTED_INSTANCE_ID = 208;

	struct InstanceCase {
		const char *name;
		RDC::ShaderStage stage;
		String source;
		LocalVector<int> expected_ops;
		LocalVector<int> wrong_ops;
	};
	const String ray_query = String::utf8(RAY_QUERY_INSTANCE_GLSL);
	const String closest_hit = String::utf8(CLOSEST_HIT_INSTANCE_GLSL);
	InstanceCase cases[] = {
		{ "ray_query_custom_index", RDC::SHADER_STAGE_COMPUTE, ray_query.replace("GETTER", "rayQueryGetIntersectionInstanceCustomIndexEXT"),
				{ OP_RAY_QUERY_CANDIDATE_INSTANCE_ID, OP_RAY_QUERY_COMMITTED_INSTANCE_ID }, { OP_RAY_QUERY_CANDIDATE_INSTANCE_INDEX, OP_RAY_QUERY_COMMITTED_INSTANCE_INDEX } },
		{ "ray_query_instance_id", RDC::SHADER_STAGE_COMPUTE, ray_query.replace("GETTER", "rayQueryGetIntersectionInstanceIdEXT"),
				{ OP_RAY_QUERY_CANDIDATE_INSTANCE_INDEX, OP_RAY_QUERY_COMMITTED_INSTANCE_INDEX }, { OP_RAY_QUERY_CANDIDATE_INSTANCE_ID, OP_RAY_QUERY_COMMITTED_INSTANCE_ID } },
		{ "closest_hit_custom_index", RDC::SHADER_STAGE_CLOSEST_HIT, closest_hit.replace("BUILTIN", "gl_InstanceCustomIndexEXT"),
				{ OP_INSTANCE_ID }, { OP_INSTANCE_INDEX } },
		{ "closest_hit_instance_id", RDC::SHADER_STAGE_CLOSEST_HIT, closest_hit.replace("BUILTIN", "gl_InstanceID"),
				{ OP_INSTANCE_INDEX }, { OP_INSTANCE_ID } },
	};

	HMODULE lib_d3d12 = LoadLibraryW(L"d3d12.dll");
	REQUIRE(lib_d3d12 != nullptr);
	RenderingShaderContainerFormatD3D12 format;
	format.set_lib_d3d12(lib_d3d12);
	const DXILValidationStats validation_before = RenderingShaderContainerD3D12::get_dxil_validation_stats();

	for (const InstanceCase &instance_case : cases) {
		Vector<RDC::ShaderStageSPIRVData> stages;
		stages.push_back(compile_glsl_stage(format, instance_case.stage, instance_case.source.utf8().get_data()));
		REQUIRE_FALSE(stages[0].spirv.is_empty());
		Ref<RenderingShaderContainer> container = format.create_container();

		if (!RenderingShaderContainerD3D12::is_raytracing_supported()) {
			// The upstream library: refused with exactly one message.
			ErrorCapture capture;
			CHECK_FALSE(container->set_code_from_spirv(instance_case.name, stages));
			CHECK_MESSAGE(capture.count == 1, String(instance_case.name));
			continue;
		}

		// REQUIRE does not stop a test case in builds without exceptions, so each step skips the rest of this
		// shader when it fails.
		bool compiled = false;
		{
			ErrorCapture capture;
			compiled = container->set_code_from_spirv(instance_case.name, stages);
			CHECK_MESSAGE(compiled, vformat("%s: %s", instance_case.name, capture.first_message));
			CHECK_MESSAGE(capture.count == 0, capture.first_message);
		}
		CHECK(container->shaders.size() == (compiled ? 1 : 0));
		if (!compiled || container->shaders.size() != 1) {
			continue;
		}
		String error;
		const String disassembly = RenderingShaderContainerD3D12::disassemble_dxil(decompressed_code(container, 0), &error);
		CHECK_MESSAGE(!disassembly.is_empty(), vformat("%s: no DXIL disassembly, which needs dxil.dll and dxcompiler.dll from the Windows SDK: %s", instance_case.name, error));
		if (disassembly.is_empty()) {
			continue;
		}
		const String instance_lines = dxil_instance_op_lines(disassembly);
		for (int op : instance_case.expected_ops) {
			CHECK_MESSAGE(dxil_calls_op(disassembly, op), vformat("%s: expected DXIL operation %d, found:\n%s", instance_case.name, op, instance_lines));
		}
		for (int op : instance_case.wrong_ops) {
			CHECK_MESSAGE(!dxil_calls_op(disassembly, op), vformat("%s: DXIL operation %d reads the other instance value:\n%s", instance_case.name, op, instance_lines));
		}
	}

	check_dxil_validation(validation_before, RenderingShaderContainerD3D12::is_raytracing_supported() ? 4 : 0);
}

// GodotNext: dxil_atomics (#279). Storage-buffer and image atomics whose result is unused, as counters use them.
const char *UNUSED_RESULT_ATOMICS_GLSL = R"(
#version 450

layout(local_size_x = 1) in;

layout(set = 0, binding = 0, std430) restrict buffer Counters {
	uint added;
	uint swapped;
}
counters;

layout(set = 0, binding = 1, r32ui) uniform restrict uimage2D counter_image;

void main() {
	atomicAdd(counters.added, 1u);
	atomicCompSwap(counters.swapped, 0u, 1u);
	imageAtomicAdd(counter_image, ivec2(0), 1u);
}
)";

// The attribute list of the DXIL function that p_function names in a disassembly ("declare ... @dx.op.atomicBinOp.i32(...)
// #0" and "attributes #0 = { nounwind }"), or an empty string if the disassembly does not declare it.
String dxil_declaration_attributes(const String &p_disassembly, const String &p_function) {
	String group;
	for (const String &line : p_disassembly.split("\n")) {
		if (line.begins_with("declare ") && line.contains("@" + p_function + "(") && line.rfind_char('#') >= 0) {
			group = line.substr(line.rfind_char('#')).strip_edges();
			break;
		}
	}
	if (!group.begins_with("#")) {
		return String();
	}
	for (const String &line : p_disassembly.split("\n")) {
		if (line.begins_with("attributes " + group + " = ")) {
			return line.get_slice("=", 1).strip_edges();
		}
	}
	return String();
}

// 25.3.1-gn3 and later declare dx.op.atomicBinOp and dx.op.atomicCompareExchange "nounwind", as DXC does; older
// libraries declare them "nounwind readnone", and D3D12 GPU-based validation then drops such atomics when their
// result is unused (#277). This case pins which of the two the linked library writes.
TEST_CASE("[ShaderContainer][D3D12] Storage-buffer and image atomics are declared without readnone with 25.3.1-gn3 or later") {
	HMODULE lib_d3d12 = LoadLibraryW(L"d3d12.dll");
	REQUIRE(lib_d3d12 != nullptr);
	RenderingShaderContainerFormatD3D12 format;
	format.set_lib_d3d12(lib_d3d12);
	const DXILValidationStats validation_before = RenderingShaderContainerD3D12::get_dxil_validation_stats();
	const bool atomics_without_readnone = RenderingShaderContainerD3D12::declares_atomics_without_readnone();

	Vector<RDC::ShaderStageSPIRVData> stages;
	stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_COMPUTE, UNUSED_RESULT_ATOMICS_GLSL));
	REQUIRE_FALSE(stages[0].spirv.is_empty());
	Ref<RenderingShaderContainer> container = format.create_container();
	bool compiled = false;
	{
		ErrorCapture capture;
		compiled = container->set_code_from_spirv("unused_result_atomics", stages);
		CHECK_MESSAGE(compiled, capture.first_message);
		CHECK_MESSAGE(capture.count == 0, capture.first_message);
	}
	// REQUIRE does not stop a test case in builds without exceptions.
	if (!compiled || container->shaders.size() != 1) {
		CHECK(container->shaders.size() == 1);
		return;
	}
	String error;
	const String disassembly = RenderingShaderContainerD3D12::disassemble_dxil(decompressed_code(container, 0), &error);
	CHECK_MESSAGE(!disassembly.is_empty(), vformat("No DXIL disassembly, which needs dxil.dll and dxcompiler.dll from the Windows SDK: %s", error));
	if (disassembly.is_empty()) {
		return;
	}

	const char *functions[] = { "dx.op.atomicBinOp.i32", "dx.op.atomicCompareExchange.i32" };
	for (const char *function : functions) {
		const String attributes = dxil_declaration_attributes(disassembly, function);
		CHECK_MESSAGE(attributes.contains("nounwind"), vformat("%s: attributes \"%s\"", function, attributes));
		if (atomics_without_readnone) {
			CHECK_MESSAGE(!attributes.contains("readnone"), vformat("%s is declared \"%s\"; 25.3.1-gn3 declares it without readnone", function, attributes));
			CHECK_MESSAGE(!attributes.contains("readonly"), vformat("%s is declared \"%s\"; an atomic writes memory", function, attributes));
		} else {
			CHECK_MESSAGE(attributes.contains("readnone"), vformat("%s is declared \"%s\": this library no longer declares it readnone, so #277's limit and the D3D12 format version need a new look", function, attributes));
		}
	}

	// The buffer add and the image add both go through dx.op.atomicBinOp (opcode 78), the compare-and-swap through
	// dx.op.atomicCompareExchange (79), all three with their results unused.
	CHECK(dxil_calls_op(disassembly, 78));
	CHECK(dxil_calls_op(disassembly, 79));
	check_dxil_validation(validation_before, 1);
}

// GodotNext: dxil_nonuniform (#293). Bindless tables (texture, sampler and storage-buffer runtime arrays, each its
// set's only binding) plus a bounded storage-image descriptor array, read at an index that varies per invocation
// (nonuniformEXT) or at a push-constant index that is the same for the whole draw. INDEX is replaced by the index
// expression.
const char *TABLE_READS_GLSL = R"(
#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, std430) restrict writeonly buffer Result {
	uint words[];
}
result;
layout(set = 0, binding = 1, r32ui) uniform restrict readonly uimage2D images[4];

layout(set = 1, binding = 0) uniform texture2D textures[];
layout(set = 2, binding = 0) uniform sampler samplers[];
layout(set = 3, binding = 0, std430) restrict readonly buffer Words {
	uint data[];
}
words[];

layout(push_constant, std430) uniform Params {
	uint slot;
}
params;

void main() {
	uint slot = gl_GlobalInvocationID.x ^ gl_GlobalInvocationID.y;
	vec4 color = textureLod(sampler2D(textures[INDEX(slot)], samplers[INDEX(slot & 1u)]), vec2(0.5), 0.0);
	uint word = words[INDEX(slot)].data[gl_GlobalInvocationID.x];
	uint texel = imageLoad(images[INDEX(slot & 3u)], ivec2(gl_GlobalInvocationID.xy)).x;
	result.words[gl_GlobalInvocationID.y * 8u + gl_GlobalInvocationID.x] = floatBitsToUint(color.x) ^ word ^ texel;
}
)";

// The handle-creating DXIL calls of a disassembly: dx.op.createHandle(i32 57, i8 class, i32 range, i32 index,
// i1 nonUniformIndex) below shader model 6.6, dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind {...}, i32 index,
// i1 nonUniformIndex) from 6.6. Each entry is the index operand and whether nonUniformIndex is true.
struct DXILHandleCall {
	String index;
	bool non_uniform = false;
};

LocalVector<DXILHandleCall> dxil_handle_calls(const String &p_disassembly) {
	LocalVector<DXILHandleCall> calls;
	for (const String &full_line : p_disassembly.split("\n")) {
		// The disassembler appends a comment that names the operands: "; CreateHandle(resourceClass,...)".
		const String line = full_line.get_slice(";", 0);
		const int call = MAX(line.find("@dx.op.createHandle("), line.find("@dx.op.createHandleFromBinding("));
		if (call < 0 || line.strip_edges().begins_with("declare")) {
			continue;
		}
		const int open = line.find_char('(', call);
		const int close = line.rfind_char(')');
		if (open < 0 || close <= open) {
			continue;
		}
		// Split the arguments at the commas outside the ResBind braces.
		PackedStringArray arguments;
		String argument;
		int depth = 0;
		for (int i = open + 1; i < close; i++) {
			const char32_t c = line[i];
			if (c == '{') {
				depth++;
			} else if (c == '}') {
				depth--;
			}
			if (c == ',' && depth == 0) {
				arguments.push_back(argument.strip_edges());
				argument = String();
			} else {
				argument += c;
			}
		}
		arguments.push_back(argument.strip_edges());
		if (arguments.size() < 2) {
			continue;
		}
		DXILHandleCall handle_call;
		handle_call.index = arguments[arguments.size() - 2];
		handle_call.non_uniform = arguments[arguments.size() - 1] == "i1 true";
		calls.push_back(handle_call);
	}
	return calls;
}

// GodotNext: dxil_nonuniform (#293). The compute shader of godotnext/testbed/tests/rd_parity/bindless.gd (its SOURCE,
// keep the two the same), which reads the texture, sampler and storage-buffer tables at a different index in every
// pixel. DEFINES is replaced as the fixture fills its %s: SAMPLER_TABLE and BUFFER_TABLE for --tables=all,
// SAMPLER_TABLE for --tables=textures, nothing for --tables=fetch.
const char *BINDLESS_FIXTURE_GLSL = R"(#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require
DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct Probe {
	uint texture_slot;
	uint lookup; // Texel x (2 bits), y (2 bits), offset x + 1 (2 bits), offset y + 1 (2 bits), sampler slot (1 bit).
	uint buffer_slot;
	uint element;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Probes {
	Probe data[];
}
probes;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Results {
	uint data[];
}
results;

layout(set = 1, binding = 0) uniform texture2D textures[];
#ifdef SAMPLER_TABLE
layout(set = 2, binding = 0) uniform sampler samplers[];
#endif
#ifdef BUFFER_TABLE
layout(set = 3, binding = 0, std430) restrict readonly buffer Words {
	uint data[];
}
words[];
#endif

layout(push_constant, std430) uniform Params {
	uint width;
	uint height;
	uint pad0;
	uint pad1;
}
params;

void main() {
	uvec2 pixel = gl_GlobalInvocationID.xy;
	if (pixel.x >= params.width || pixel.y >= params.height) {
		return;
	}
	uint index = pixel.y * params.width + pixel.x;
	Probe probe = probes.data[index];
	vec2 texel = vec2(float(probe.lookup & 3u), float((probe.lookup >> 2u) & 3u));
	vec2 offset = vec2(float((probe.lookup >> 4u) & 3u), float((probe.lookup >> 6u) & 3u)) - 1.0;
	uint sampler_slot = (probe.lookup >> 8u) & 1u;
	vec2 uv = (texel + 0.5) / 4.0 + offset;
#ifdef SAMPLER_TABLE
	vec4 color = textureLod(sampler2D(textures[nonuniformEXT(probe.texture_slot)], samplers[nonuniformEXT(sampler_slot)]), uv, 0.0);
#else
	vec4 color = texelFetch(textures[nonuniformEXT(probe.texture_slot)], ivec2(texel), 0);
#endif
#ifdef BUFFER_TABLE
	uint word = words[nonuniformEXT(probe.buffer_slot)].data[probe.element];
#else
	uint word = 0u;
#endif
	results.data[index] = packUnorm4x8(color) ^ word;
}
)";

// GLSL nonuniformEXT (SPIR-V NonUniform) must reach DXIL as createHandle's nonUniformIndex, as DXC writes it for
// NonUniformResourceIndex(): an index that varies across a wave is otherwise undefined, and some GPUs give the whole
// wave one invocation's descriptor. 25.3.1-gn4 and later set it for the texture, sampler, storage-buffer and image
// tables read at a non-uniform index, and keep it false for constant indices and for indices without nonuniformEXT.
// Older libraries set it nowhere. The bindless fixture's own shader is checked the same way, in its three variants.
TEST_CASE("[ShaderContainer][D3D12] Table reads at a nonuniformEXT index set createHandle's nonUniformIndex with 25.3.1-gn4 or later") {
	HMODULE lib_d3d12 = LoadLibraryW(L"d3d12.dll");
	REQUIRE(lib_d3d12 != nullptr);
	RenderingShaderContainerFormatD3D12 format;
	format.set_lib_d3d12(lib_d3d12);
	const DXILValidationStats validation_before = RenderingShaderContainerD3D12::get_dxil_validation_stats();
	const bool marks = RenderingShaderContainerD3D12::marks_non_uniform_resource_indices();

	struct TableCase {
		const char *name;
		String source;
		bool expect_non_uniform;
		// Handle calls at a non-constant index: the tables the shader reads.
		uint32_t varying_indices;
		// Handle calls at a constant index: the buffers in set 0.
		uint32_t constant_indices;
	};
	const String fixture = String::utf8(BINDLESS_FIXTURE_GLSL);
	const TableCase cases[] = {
		{ "non_uniform_table_reads", String::utf8(TABLE_READS_GLSL).replace("INDEX", "nonuniformEXT"), true, 4, 1 },
		{ "uniform_table_reads", String::utf8(TABLE_READS_GLSL).replace("INDEX", "params.slot + 0u * "), false, 4, 1 },
		{ "bindless_fixture_all", fixture.replace("DEFINES", "#define BUFFER_TABLE\n#define SAMPLER_TABLE\n"), true, 3, 2 },
		{ "bindless_fixture_textures", fixture.replace("DEFINES", "#define SAMPLER_TABLE\n"), true, 2, 2 },
		{ "bindless_fixture_fetch", fixture.replace("DEFINES", ""), true, 1, 2 },
	};
	uint32_t compiled_count = 0;
	for (const TableCase &table_case : cases) {
		Vector<RDC::ShaderStageSPIRVData> stages;
		stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_COMPUTE, table_case.source.utf8().get_data()));
		REQUIRE_FALSE(stages[0].spirv.is_empty());
		Ref<RenderingShaderContainer> container = format.create_container();
		bool compiled = false;
		{
			ErrorCapture capture;
			compiled = container->set_code_from_spirv(table_case.name, stages);
			CHECK_MESSAGE(compiled, vformat("%s: %s", table_case.name, capture.first_message));
			CHECK_MESSAGE(capture.count == 0, capture.first_message);
		}
		// REQUIRE does not stop a test case in builds without exceptions.
		if (!compiled || container->shaders.size() != 1) {
			CHECK(container->shaders.size() == 1);
			continue;
		}
		compiled_count++;
		String error;
		const String disassembly = RenderingShaderContainerD3D12::disassemble_dxil(decompressed_code(container, 0), &error);
		CHECK_MESSAGE(!disassembly.is_empty(), vformat("%s: no DXIL disassembly, which needs dxil.dll and dxcompiler.dll from the Windows SDK: %s", table_case.name, error));
		if (disassembly.is_empty()) {
			continue;
		}
		// The buffers in set 0 (a constant index) and the tables (a varying index).
		const LocalVector<DXILHandleCall> calls = dxil_handle_calls(disassembly);
		uint32_t constant_indices = 0;
		uint32_t varying_indices = 0;
		for (const DXILHandleCall &call : calls) {
			const bool constant = call.index.begins_with("i32 ") && call.index.substr(4).is_valid_int();
			if (constant) {
				constant_indices++;
				CHECK_MESSAGE(!call.non_uniform, vformat("%s: a constant index is marked non-uniform (%s)", table_case.name, call.index));
			} else {
				varying_indices++;
				const bool expected = marks && table_case.expect_non_uniform;
				CHECK_MESSAGE(call.non_uniform == expected, vformat("%s: createHandle for index %s has nonUniformIndex %s, expected %s", table_case.name, call.index, call.non_uniform ? "true" : "false", expected ? "true" : "false"));
			}
		}
		CHECK_MESSAGE(constant_indices >= table_case.constant_indices, vformat("%s: %d handle calls, %d at a constant index", table_case.name, calls.size(), constant_indices));
		CHECK_MESSAGE(varying_indices >= table_case.varying_indices, vformat("%s: %d handle calls, %d at a varying index", table_case.name, calls.size(), varying_indices));
	}
	check_dxil_validation(validation_before, compiled_count);
}
#endif // DEBUG_ENABLED

// GodotNext: d3d12_dxr (#118). Ray query in a pixel shader, as rd_smoke's fixture F runs it: a fullscreen triangle
// and a fragment shader that traces one ray per pixel.
const char *PIXEL_RAY_QUERY_VERTEX_GLSL = R"(
#version 450

void main() {
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char *PIXEL_RAY_QUERY_FRAGMENT_GLSL = R"(
#version 460
#extension GL_EXT_ray_query : require

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(location = 0) out vec4 frag_color;

void main() {
	vec2 xy = (gl_FragCoord.xy - 0.25) / 8.0;
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, vec3(xy, 0.0), 0.001, vec3(0.0, 0.0, -1.0), 100.0);
	rayQueryProceedEXT(rq);
	frag_color = vec4(-1.0, 0.0, 0.0, -1.0);
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
		uint front = rayQueryGetIntersectionFrontFaceEXT(rq, true) ? 1u : 0u;
		uint custom_index = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
		uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
		frag_color = vec4(rayQueryGetIntersectionTEXT(rq, true), bary, float((primitive << 9u) | (custom_index << 1u) | front));
	}
}
)";

TEST_CASE("[ShaderContainer][D3D12] GLSL ray query in a pixel shader compiles to DXIL") {
	HMODULE lib_d3d12 = LoadLibraryW(L"d3d12.dll");
	REQUIRE(lib_d3d12 != nullptr);
	RenderingShaderContainerFormatD3D12 format;
	format.set_lib_d3d12(lib_d3d12);

	Vector<RDC::ShaderStageSPIRVData> stages;
	stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_VERTEX, PIXEL_RAY_QUERY_VERTEX_GLSL));
	stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_FRAGMENT, PIXEL_RAY_QUERY_FRAGMENT_GLSL));
	for (const RDC::ShaderStageSPIRVData &stage : stages) {
		REQUIRE_FALSE(stage.spirv.is_empty());
	}

	Ref<RenderingShaderContainer> container = format.create_container();
	if (RenderingShaderContainerD3D12::is_raytracing_supported()) {
		{
			ErrorCapture capture;
			REQUIRE(container->set_code_from_spirv("pixel_ray_query", stages));
			CHECK_MESSAGE(capture.count == 0, capture.first_message);
		}
		// The shader baker bakes ray-query versions for this format (ShaderBakerExportPlugin::should_bake_version).
		CHECK(format.supports_acceleration_structures());
		// A DXIL pixel shader (program kind 0) at shader model 6.5, and its acceleration structure is an SRV in the
		// root signature's descriptor table.
		REQUIRE(container->shaders.size() == 2);
		bool fragment_found = false;
		for (int i = 0; i < container->shaders.size(); i++) {
			if (container->shaders[i].shader_stage == RDC::SHADER_STAGE_FRAGMENT) {
				fragment_found = true;
				CHECK(dxil_program_version(decompressed_code(container, i)) == ((0u << 16) | 0x65));
			}
		}
		CHECK(fragment_found);
		const RenderingShaderContainerD3D12 *container_d3d12 = Object::cast_to<RenderingShaderContainerD3D12>(container.ptr());
		REQUIRE(container_d3d12 != nullptr);
		const RenderingShaderContainerD3D12::ShaderReflectionD3D12 reflection = container_d3d12->get_shader_reflection_d3d12();
		REQUIRE(reflection.reflection_binding_set_uniforms_d3d12.size() == 1);
		REQUIRE(reflection.reflection_binding_set_uniforms_d3d12[0].size() == 1);
		CHECK(reflection.reflection_binding_set_uniforms_d3d12[0][0].resource_class == (uint32_t)RES_CLASS_SRV);
		CHECK(reflection.reflection_binding_sets_d3d12[0].resource_descriptor_count == 1);
		CHECK(reflection.reflection_binding_sets_d3d12[0].resource_root_param_idx != UINT32_MAX);
	} else {
		ErrorCapture capture;
		CHECK_FALSE(container->set_code_from_spirv("pixel_ray_query", stages));
		CHECK(capture.count == 1);
		CHECK_MESSAGE(capture.first_message.contains("uses ray query"), capture.first_message);
		// The shader baker skips them instead of failing the export.
		CHECK_FALSE(format.supports_acceleration_structures());
	}
}

// GodotNext: d3d12_rt_pipelines (#121). The stages of godotnext/testbed/tests/rd_parity/rt_pipeline.gd: a ray generation
// and a miss shader that declare (but do not all use) storage buffers, an acceleration structure, a bindless texture
// table and push constants; an any-hit shader without a ray payload; hit shaders that read a shader record; and a
// closest-hit shader without hit attributes. Then miss shaders without a payload beside ray generation shaders that
// trace one payload type and two.
const char *RT_COMMON_GLSL = R"(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, std430) restrict readonly buffer Rays {
	vec4 data[];
}
rays;
layout(set = 0, binding = 2, std430) restrict writeonly buffer Results {
	uvec4 data[];
}
results;
layout(set = 1, binding = 0) uniform texture2D alpha_textures[];
layout(push_constant, std430) uniform Params {
	uint width;
	uint height;
	uint pad0;
	uint pad1;
}
params;
uint alpha_at(uint slot, vec2 bary) {
	uvec2 texel = min(uvec2(bary * 16.0), uvec2(15u));
	return uint(texelFetch(alpha_textures[nonuniformEXT(slot)], ivec2(texel), 0).a * 255.0 + 0.5);
}
)";

const char *RT_RAYGEN_GLSL = R"(
layout(location = 0) rayPayloadEXT uvec4 payload;
void main() {
	uint index = gl_LaunchIDEXT.y * gl_LaunchSizeEXT.x + gl_LaunchIDEXT.x;
	traceRayEXT(tlas, gl_RayFlagsNoneEXT, 0xFF, 0, 1, 0, rays.data[index * 2u].xyz, 0.0, rays.data[index * 2u + 1u].xyz, 100.0, 0);
	results.data[index] = payload;
}
)";

const char *RT_MISS_GLSL = R"(
layout(location = 0) rayPayloadInEXT uvec4 payload;
void main() {
	payload = uvec4(0xFFFFFFFFu);
}
)";

const char *RT_RECORD_GLSL = R"(
layout(shaderRecordEXT, std430) buffer HitRecord {
	uint material;
	uint slot;
}
record;
hitAttributeEXT vec2 bary;
)";

const char *RT_ANY_HIT_GLSL = R"(
layout(constant_id = 0) const uint ALPHA_CUTOFF = 128u;
void main() {
	if (alpha_at(record.slot, bary) < ALPHA_CUTOFF) {
		ignoreIntersectionEXT;
	}
}
)";

const char *RT_CLOSEST_HIT_GLSL = R"(
layout(location = 0) rayPayloadInEXT uvec4 payload;
void main() {
	payload = uvec4(uint(gl_InstanceID), uint(gl_PrimitiveID), floatBitsToUint(gl_HitTEXT), record.material | (alpha_at(record.slot, bary) << 24u));
}
)";

const char *RT_CLOSEST_HIT_WITHOUT_ATTRIBUTES_GLSL = R"(#version 460
#extension GL_EXT_ray_tracing : require
layout(location = 0) rayPayloadInEXT uvec4 payload;
void main() {
	payload = uvec4(uint(gl_PrimitiveID));
}
)";

// A miss stage that uses no payload (a shadow miss that leaves the value the ray generation stage set): beside
// RT_RAYGEN_GLSL, which traces one 16-byte payload type, its stand-in is that type.
const char *RT_MISS_WITHOUT_PAYLOAD_GLSL = R"(
void main() {
}
)";

// A ray generation stage that traces two payload types, so a miss stage beside it that uses no payload gets the
// 4-byte uint stand-in.
const char *RT_RAYGEN_TWO_PAYLOADS_GLSL = R"(
layout(location = 0) rayPayloadEXT uvec4 payload;
layout(location = 1) rayPayloadEXT float shadow;
void main() {
	uint index = gl_LaunchIDEXT.y * gl_LaunchSizeEXT.x + gl_LaunchIDEXT.x;
	traceRayEXT(tlas, gl_RayFlagsNoneEXT, 0xFF, 0, 1, 0, rays.data[index * 2u].xyz, 0.0, rays.data[index * 2u + 1u].xyz, 100.0, 0);
	traceRayEXT(tlas, gl_RayFlagsNoneEXT, 0xFF, 0, 1, 0, rays.data[index * 2u].xyz, 0.0, rays.data[index * 2u + 1u].xyz, 100.0, 1);
	results.data[index] = payload + uvec4(floatBitsToUint(shadow));
}
)";

// The ray-tracing data of one stage of a container.
RenderingShaderContainerD3D12::RaytracingStageDataD3D12 raytracing_stage_data(const Ref<RenderingShaderContainer> &p_container, RDC::ShaderStage p_stage) {
	const RenderingShaderContainerD3D12 *container = Object::cast_to<RenderingShaderContainerD3D12>(p_container.ptr());
	if (container == nullptr) {
		return RenderingShaderContainerD3D12::RaytracingStageDataD3D12();
	}
	return container->get_shader_reflection_d3d12().raytracing_stages[p_stage - RDC::SHADER_STAGE_RAYGEN];
}

// The DXIL of one stage of a container, or an empty blob.
Vector<uint8_t> stage_code(const Ref<RenderingShaderContainer> &p_container, RDC::ShaderStage p_stage) {
	for (int i = 0; i < p_container->shaders.size(); i++) {
		if (p_container->shaders[i].shader_stage == p_stage) {
			return decompressed_code(p_container, i);
		}
	}
	return Vector<uint8_t>();
}

// Run it with GODOT_D3D12_DXIL_VALIDATE=1 set, and every blob also goes through the DXIL validator: the miss stage, which
// declares the ray generation stage's storage buffers but uses none, failed it before #121 ("Internal declaration ...
// is unused.").
TEST_CASE("[ShaderContainer][D3D12] RT pipeline stages: shader records, payload and attribute stand-ins, and no unused resources") {
	// Ray generation through intersection have ray-tracing stage data; no other stage has, also none after them.
	CHECK(RenderingShaderContainerD3D12::is_raytracing_stage(RDC::SHADER_STAGE_RAYGEN));
	CHECK(RenderingShaderContainerD3D12::is_raytracing_stage(RDC::SHADER_STAGE_INTERSECTION));
	CHECK_FALSE(RenderingShaderContainerD3D12::is_raytracing_stage(RDC::SHADER_STAGE_COMPUTE));
	CHECK_FALSE(RenderingShaderContainerD3D12::is_raytracing_stage(RDC::SHADER_STAGE_INTERSECTION + 1));

	HMODULE lib_d3d12 = LoadLibraryW(L"d3d12.dll");
	REQUIRE(lib_d3d12 != nullptr);
	RenderingShaderContainerFormatD3D12 format;
	format.set_lib_d3d12(lib_d3d12);

	const String common = RT_COMMON_GLSL;
	Vector<RDC::ShaderStageSPIRVData> raygen_stages;
	raygen_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_RAYGEN, (common + RT_RAYGEN_GLSL).utf8().get_data()));
	raygen_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_MISS, (common + RT_MISS_GLSL).utf8().get_data()));
	Vector<RDC::ShaderStageSPIRVData> hit_stages;
	hit_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_ANY_HIT, (common + RT_RECORD_GLSL + RT_ANY_HIT_GLSL).utf8().get_data()));
	hit_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_CLOSEST_HIT, (common + RT_RECORD_GLSL + RT_CLOSEST_HIT_GLSL).utf8().get_data()));
	Vector<RDC::ShaderStageSPIRVData> small_hit_stages;
	small_hit_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_CLOSEST_HIT, RT_CLOSEST_HIT_WITHOUT_ATTRIBUTES_GLSL));
	Vector<RDC::ShaderStageSPIRVData> shadow_miss_stages;
	shadow_miss_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_RAYGEN, (common + RT_RAYGEN_GLSL).utf8().get_data()));
	shadow_miss_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_MISS, (common + RT_MISS_WITHOUT_PAYLOAD_GLSL).utf8().get_data()));
	Vector<RDC::ShaderStageSPIRVData> two_payloads_stages;
	two_payloads_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_RAYGEN, (common + RT_RAYGEN_TWO_PAYLOADS_GLSL).utf8().get_data()));
	two_payloads_stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_MISS, (common + RT_MISS_WITHOUT_PAYLOAD_GLSL).utf8().get_data()));
	for (const Vector<RDC::ShaderStageSPIRVData> &stages : { raygen_stages, hit_stages, small_hit_stages, shadow_miss_stages, two_payloads_stages }) {
		for (const RDC::ShaderStageSPIRVData &stage : stages) {
			REQUIRE_FALSE(stage.spirv.is_empty());
		}
	}

	const DXILValidationStats validation_before = RenderingShaderContainerD3D12::get_dxil_validation_stats();
	Ref<RenderingShaderContainer> raygen = format.create_container();
	Ref<RenderingShaderContainer> hit = format.create_container();
	Ref<RenderingShaderContainer> small_hit = format.create_container();
	Ref<RenderingShaderContainer> shadow_miss = format.create_container();
	Ref<RenderingShaderContainer> two_payloads = format.create_container();
	struct ContainerStages {
		Ref<RenderingShaderContainer> container;
		const char *name = nullptr;
		const Vector<RDC::ShaderStageSPIRVData> *stages = nullptr;
	};
	const ContainerStages containers[] = {
		{ raygen, "rt raygen", &raygen_stages },
		{ hit, "rt hit", &hit_stages },
		{ small_hit, "rt small hit", &small_hit_stages },
		{ shadow_miss, "rt shadow miss", &shadow_miss_stages },
		{ two_payloads, "rt two payloads", &two_payloads_stages },
	};
	if (!RenderingShaderContainerD3D12::is_raytracing_supported()) {
		// The upstream library refuses ray-tracing stages, with one message each.
		for (const ContainerStages &entry : containers) {
			ErrorCapture capture;
			CHECK_FALSE(entry.container->set_code_from_spirv(entry.name, *entry.stages));
			CHECK(capture.count == 1);
		}
		check_dxil_validation(validation_before, 0);
		return;
	}

	{
		// Before #121 the any-hit stage (no payload), the hit stages (a shader record), the closest-hit stage without
		// hit attributes and the miss stages without a payload failed to translate.
		ErrorCapture capture;
		for (const ContainerStages &entry : containers) {
			REQUIRE_MESSAGE(entry.container->set_code_from_spirv(entry.name, *entry.stages), entry.name);
		}
		CHECK_MESSAGE(capture.count == 0, capture.first_message);
	}
	using StageData = RenderingShaderContainerD3D12::RaytracingStageDataD3D12;
	constexpr uint32_t PRESENT = RenderingShaderContainerD3D12::RAYTRACING_STAGE_PRESENT;
	constexpr uint32_t PAYLOAD = RenderingShaderContainerD3D12::RAYTRACING_STAGE_PAYLOAD_DECLARED;
	constexpr uint32_t ATTRIBUTES = RenderingShaderContainerD3D12::RAYTRACING_STAGE_ATTRIBUTES_DECLARED;

	// The ray generation stage passes a 16-byte payload; the miss stage takes it. Neither reads a record.
	const StageData raygen_data = raytracing_stage_data(raygen, RDC::SHADER_STAGE_RAYGEN);
	CHECK(raygen_data.flags == PRESENT);
	CHECK(raygen_data.payload_size == 16);
	CHECK(raygen_data.spirv_payload_size == 16);
	CHECK(raygen_data.incoming_payload_size == 0);
	CHECK(raygen_data.record_size == 0);
	const StageData miss_data = raytracing_stage_data(raygen, RDC::SHADER_STAGE_MISS);
	CHECK(miss_data.flags == (PRESENT | PAYLOAD));
	CHECK(miss_data.incoming_payload_size == 16);
	CHECK(miss_data.record_size == 0);

	// The any-hit stage declares no payload: it gets the closest-hit stage's 16-byte type. Both read the 8-byte record,
	// a 16-byte constant buffer row, and declare the triangle's two-float attributes.
	const StageData any_hit_data = raytracing_stage_data(hit, RDC::SHADER_STAGE_ANY_HIT);
	CHECK(any_hit_data.flags == (PRESENT | ATTRIBUTES));
	CHECK(any_hit_data.incoming_payload_size == 16);
	CHECK(any_hit_data.attribute_size == 8);
	CHECK(any_hit_data.record_size == 16);
	const StageData closest_hit_data = raytracing_stage_data(hit, RDC::SHADER_STAGE_CLOSEST_HIT);
	CHECK(closest_hit_data.flags == (PRESENT | PAYLOAD | ATTRIBUTES));
	CHECK(closest_hit_data.incoming_payload_size == 16);
	CHECK(closest_hit_data.attribute_size == 8);
	CHECK(closest_hit_data.record_size == 16);

	// A closest-hit stage without hit attributes gets the barycentrics' two floats.
	const StageData small_hit_data = raytracing_stage_data(small_hit, RDC::SHADER_STAGE_CLOSEST_HIT);
	CHECK(small_hit_data.flags == (PRESENT | PAYLOAD));
	CHECK(small_hit_data.attribute_size == 8);
	CHECK(small_hit_data.record_size == 0);

	// A miss stage without a payload beside a ray generation stage that traces one payload type gets that type; beside
	// one that traces two, the uint.
	const StageData shadow_miss_data = raytracing_stage_data(shadow_miss, RDC::SHADER_STAGE_MISS);
	CHECK(shadow_miss_data.flags == PRESENT);
	CHECK(shadow_miss_data.incoming_payload_size == 16);
	CHECK(shadow_miss_data.payload_size == 16);
	const StageData two_payloads_raygen_data = raytracing_stage_data(two_payloads, RDC::SHADER_STAGE_RAYGEN);
	CHECK(two_payloads_raygen_data.payload_size == 16);
	const StageData two_payloads_miss_data = raytracing_stage_data(two_payloads, RDC::SHADER_STAGE_MISS);
	CHECK(two_payloads_miss_data.flags == PRESENT);
	CHECK(two_payloads_miss_data.incoming_payload_size == 4);

	// The data survives a round trip through bytes.
	{
		Ref<RenderingShaderContainer> loaded = format.create_container();
		REQUIRE(loaded->from_bytes(hit->to_bytes()));
		const StageData loaded_data = raytracing_stage_data(loaded, RDC::SHADER_STAGE_ANY_HIT);
		CHECK(memcmp(&loaded_data, &any_hit_data, sizeof(StageData)) == 0);
	}

	// Three containers of a ray generation and a miss blob each, two hit blobs and one closest-hit blob.
	check_dxil_validation(validation_before, 9);

#ifdef DEBUG_ENABLED
	// The record is the constant buffer of the local root signature (register 2 of the root constants' space 1), and the
	// miss stage no longer declares the storage buffers it does not use (registers 100000 and 200000: set 0, bindings 1
	// and 2), which the ray generation stage uses.
	String error;
	const String closest_hit_disassembly = RenderingShaderContainerD3D12::disassemble_dxil(stage_code(hit, RDC::SHADER_STAGE_CLOSEST_HIT), &error);
	REQUIRE_MESSAGE(!closest_hit_disassembly.is_empty(), error);
	CHECK_MESSAGE(closest_hit_disassembly.contains("cb2,space1"), closest_hit_disassembly);
	const String miss_disassembly = RenderingShaderContainerD3D12::disassemble_dxil(stage_code(raygen, RDC::SHADER_STAGE_MISS), &error);
	REQUIRE_MESSAGE(!miss_disassembly.is_empty(), error);
	CHECK_FALSE_MESSAGE(miss_disassembly.contains("100000"), miss_disassembly);
	CHECK_FALSE_MESSAGE(miss_disassembly.contains("200000"), miss_disassembly);
	const String raygen_disassembly = RenderingShaderContainerD3D12::disassemble_dxil(stage_code(raygen, RDC::SHADER_STAGE_RAYGEN), &error);
	REQUIRE_MESSAGE(!raygen_disassembly.is_empty(), error);
	CHECK_MESSAGE(raygen_disassembly.contains("100000"), raygen_disassembly);
	CHECK_MESSAGE(raygen_disassembly.contains("200000"), raygen_disassembly);
#endif
}

#ifdef DEBUG_ENABLED
// The getelementptr lines of a DXIL disassembly, and how many of them carry !dx.nonuniform.
struct DXILGEPLines {
	uint32_t geps = 0;
	uint32_t non_uniform = 0;
};

DXILGEPLines dxil_gep_lines(const String &p_disassembly) {
	DXILGEPLines lines;
	for (const String &line : p_disassembly.split("\n")) {
		if (!line.contains(" getelementptr ")) {
			continue;
		}
		lines.geps++;
		if (line.contains("!dx.nonuniform !")) {
			lines.non_uniform++;
		}
	}
	return lines;
}

// GodotNext: dxil_nonuniform (#293). Ray-tracing stages are DXIL libraries, which read a resource with
// dx.op.createHandleForLib on a load from the resource's global. That call has no nonUniformIndex operand: DXC marks a
// NonUniformResourceIndex() read by attaching !dx.nonuniform !{i32 1} to the getelementptr into the resource array
// (lib_6_3 to lib_6_6). 25.3.1-gn4 and later do the same for nonuniformEXT reads, so rt_pipeline.gd's any-hit and
// closest-hit stages, whose alpha_at() reads alpha_textures[nonuniformEXT(record.slot)], carry the mark; the same
// stages reading alpha_textures[record.slot] (no nonuniformEXT) do not, and older libraries mark nothing.
TEST_CASE("[ShaderContainer][D3D12] Table reads at a nonuniformEXT index in ray-tracing stages carry !dx.nonuniform with 25.3.1-gn4 or later") {
	HMODULE lib_d3d12 = LoadLibraryW(L"d3d12.dll");
	REQUIRE(lib_d3d12 != nullptr);
	RenderingShaderContainerFormatD3D12 format;
	format.set_lib_d3d12(lib_d3d12);
	const bool marks = RenderingShaderContainerD3D12::marks_non_uniform_resource_indices();

	const String non_uniform_common = RT_COMMON_GLSL;
	const String uniform_common = non_uniform_common.replace("alpha_textures[nonuniformEXT(slot)]", "alpha_textures[slot]");
	REQUIRE(non_uniform_common.contains("alpha_textures[nonuniformEXT(slot)]"));
	REQUIRE(uniform_common != non_uniform_common);
	struct HitCase {
		const char *name;
		String common;
		bool expect_non_uniform;
	};
	const HitCase cases[] = {
		{ "rt_hit_non_uniform_table", non_uniform_common, true },
		{ "rt_hit_uniform_table", uniform_common, false },
	};
	const DXILValidationStats validation_before = RenderingShaderContainerD3D12::get_dxil_validation_stats();
	uint32_t blobs = 0;
	for (const HitCase &hit_case : cases) {
		Vector<RDC::ShaderStageSPIRVData> stages;
		stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_ANY_HIT, (hit_case.common + RT_RECORD_GLSL + RT_ANY_HIT_GLSL).utf8().get_data()));
		stages.push_back(compile_glsl_stage(format, RDC::SHADER_STAGE_CLOSEST_HIT, (hit_case.common + RT_RECORD_GLSL + RT_CLOSEST_HIT_GLSL).utf8().get_data()));
		REQUIRE_FALSE(stages[0].spirv.is_empty());
		REQUIRE_FALSE(stages[1].spirv.is_empty());
		Ref<RenderingShaderContainer> container = format.create_container();
		if (!RenderingShaderContainerD3D12::is_raytracing_supported()) {
			// The upstream library refuses ray-tracing stages, with one message.
			ErrorCapture capture;
			CHECK_FALSE(container->set_code_from_spirv(hit_case.name, stages));
			CHECK(capture.count == 1);
			CHECK_FALSE(marks);
			continue;
		}
		bool compiled = false;
		{
			ErrorCapture capture;
			compiled = container->set_code_from_spirv(hit_case.name, stages);
			CHECK_MESSAGE(compiled, vformat("%s: %s", hit_case.name, capture.first_message));
			CHECK_MESSAGE(capture.count == 0, capture.first_message);
		}
		// REQUIRE does not stop a test case in builds without exceptions.
		if (!compiled || container->shaders.size() != 2) {
			CHECK(container->shaders.size() == 2);
			continue;
		}
		blobs += 2;
		for (RDC::ShaderStage stage : { RDC::SHADER_STAGE_ANY_HIT, RDC::SHADER_STAGE_CLOSEST_HIT }) {
			String error;
			const String disassembly = RenderingShaderContainerD3D12::disassemble_dxil(stage_code(container, stage), &error);
			CHECK_MESSAGE(!disassembly.is_empty(), vformat("%s %s: no DXIL disassembly, which needs dxil.dll and dxcompiler.dll from the Windows SDK: %s", hit_case.name, RDC::SHADER_STAGE_NAMES[stage], error));
			if (disassembly.is_empty()) {
				continue;
			}
			// A DXIL library reads the table through createHandleForLib, not createHandle.
			CHECK_MESSAGE(disassembly.contains("@dx.op.createHandleForLib."), disassembly);
			CHECK_MESSAGE(dxil_handle_calls(disassembly).is_empty(), disassembly);
			const DXILGEPLines lines = dxil_gep_lines(disassembly);
			CHECK_MESSAGE(lines.geps >= 1, disassembly);
			if (marks && hit_case.expect_non_uniform) {
				CHECK_MESSAGE(lines.non_uniform >= 1, vformat("%s %s: no getelementptr carries !dx.nonuniform:\n%s", hit_case.name, RDC::SHADER_STAGE_NAMES[stage], disassembly));
				CHECK_MESSAGE(disassembly.contains("= !{i32 1}"), disassembly);
			} else {
				CHECK_MESSAGE(lines.non_uniform == 0, vformat("%s %s: %d getelementptr lines carry !dx.nonuniform:\n%s", hit_case.name, RDC::SHADER_STAGE_NAMES[stage], lines.non_uniform, disassembly));
			}
		}
	}
	check_dxil_validation(validation_before, blobs);
}
#endif // DEBUG_ENABLED
#endif // MODULE_GLSLANG_ENABLED
#endif // D3D12_ENABLED

} // namespace TestRenderingShaderContainer
