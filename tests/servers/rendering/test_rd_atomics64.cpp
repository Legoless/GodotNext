/**************************************************************************/
/*  test_rd_atomics64.cpp                                                 */
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

TEST_FORCE_LINK(test_rd_atomics64)

// 64-bit atomics through RenderingDevice (#175, plan §21.1 C2 and C3) without a GPU. The SPIR-V
// modules are assembled here, so no shader compiler is involved. The RenderingDevice cases use a
// driverless device whose features are mocked; godotnext/testbed/tests/rd_parity/atomics64.gd runs
// the same shaders' operations on a GPU.

#ifdef RD_ENABLED

#include "core/error/error_macros.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_atomics64.h"
#include "servers/rendering/rendering_shader_container.h"

#include <thirdparty/spirv-headers/include/spirv/unified1/spirv.h>

namespace TestRDAtomics64 {

// Records the errors printed while it is alive.
struct Atomics64ErrorCapture {
	ErrorHandlerList handler;
	int count = 0;
	String last_message;

	Atomics64ErrorCapture() {
		handler.errfunc = _capture;
		handler.userdata = this;
		add_error_handler(&handler);
	}

	~Atomics64ErrorCapture() {
		remove_error_handler(&handler);
	}

	static void _capture(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
		Atomics64ErrorCapture *self = static_cast<Atomics64ErrorCapture *>(p_self);
		self->count++;
		self->last_message = String::utf8((p_message != nullptr && *p_message != 0) ? p_message : p_error);
	}
};

// Assembles a SPIR-V 1.3 module one instruction at a time.
class SpirvAssembler {
	LocalVector<uint32_t> words = { SpvMagicNumber, 0x00010300, 0, 0, 0 };
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

enum ModuleKind {
	MODULE_BUFFER_UMAX,
	MODULE_BUFFER_UMIN,
	MODULE_BUFFER_IADD,
	MODULE_BUFFER_STORE,
	MODULE_IMAGE_UMAX,
	MODULE_IMAGE_STORE,
	MODULE_BUFFER_32_BIT_IADD,
	MODULE_IMAGE_3D_UMAX,
	MODULE_IMAGE_ARRAY_UMAX,
	MODULE_SHARED_UMAX,
	MODULE_BUFFER_UMAX_32_BIT_IADD,
};

// A compute shader with one 64-bit atomic (32-bit for MODULE_BUFFER_32_BIT_IADD) on element 0 of a
// storage buffer at set 0, binding 0, on texel (0, 0) of an r64ui storage image there (2D unless the
// kind says 3D or array), or on a shared uint64 variable (MODULE_SHARED_UMAX).
// MODULE_BUFFER_UMAX_32_BIT_IADD adds a 32-bit atomic add on element 0 of a uint storage buffer at
// binding 1.
Vector<uint8_t> make_module(ModuleKind p_kind) {
	const bool image_3d = p_kind == MODULE_IMAGE_3D_UMAX;
	const bool image_array = p_kind == MODULE_IMAGE_ARRAY_UMAX;
	const bool image = p_kind == MODULE_IMAGE_UMAX || p_kind == MODULE_IMAGE_STORE || image_3d || image_array;
	const bool shared = p_kind == MODULE_SHARED_UMAX;
	const bool counter = p_kind == MODULE_BUFFER_UMAX_32_BIT_IADD;
	const bool wide = p_kind != MODULE_BUFFER_32_BIT_IADD;
	SpirvAssembler a;
	const uint32_t main_fn = a.id();
	const uint32_t void_type = a.id();
	const uint32_t fn_type = a.id();
	const uint32_t uint_type = a.id();
	const uint32_t int_type = a.id();
	const uint32_t ulong_type = a.id();
	const uint32_t value_type = wide ? ulong_type : uint_type;

	a.op(SpvOpCapability, { SpvCapabilityShader });
	a.op(SpvOpCapability, { SpvCapabilityInt64 });
	if (wide) {
		a.op(SpvOpCapability, { SpvCapabilityInt64Atomics });
	}
	if (image) {
		a.op(SpvOpCapability, { SpvCapabilityInt64ImageEXT });
		a.op_string(SpvOpExtension, {}, "SPV_EXT_shader_image_int64");
	}
	a.op(SpvOpMemoryModel, { SpvAddressingModelLogical, SpvMemoryModelGLSL450 });
	a.op_string(SpvOpEntryPoint, { SpvExecutionModelGLCompute, main_fn }, "main");
	a.op(SpvOpExecutionMode, { main_fn, SpvExecutionModeLocalSize, 1, 1, 1 });

	const uint32_t resource = a.id();
	if (!shared) {
		a.op(SpvOpDecorate, { resource, SpvDecorationDescriptorSet, 0 });
		a.op(SpvOpDecorate, { resource, SpvDecorationBinding, 0 });
	}
	const uint32_t array_type = a.id();
	const uint32_t block_type = a.id();
	if (!image && !shared) {
		a.op(SpvOpDecorate, { array_type, SpvDecorationArrayStride, wide ? 8u : 4u });
		a.op(SpvOpMemberDecorate, { block_type, 0, SpvDecorationOffset, 0 });
		a.op(SpvOpDecorate, { block_type, SpvDecorationBlock });
	}
	const uint32_t counter_resource = a.id();
	const uint32_t counter_array_type = a.id();
	const uint32_t counter_block_type = a.id();
	if (counter) {
		a.op(SpvOpDecorate, { counter_resource, SpvDecorationDescriptorSet, 0 });
		a.op(SpvOpDecorate, { counter_resource, SpvDecorationBinding, 1 });
		a.op(SpvOpDecorate, { counter_array_type, SpvDecorationArrayStride, 4 });
		a.op(SpvOpMemberDecorate, { counter_block_type, 0, SpvDecorationOffset, 0 });
		a.op(SpvOpDecorate, { counter_block_type, SpvDecorationBlock });
	}

	a.op(SpvOpTypeVoid, { void_type });
	a.op(SpvOpTypeFunction, { fn_type, void_type });
	a.op(SpvOpTypeInt, { uint_type, 32, 0 });
	a.op(SpvOpTypeInt, { int_type, 32, 1 });
	a.op(SpvOpTypeInt, { ulong_type, 64, 0 });

	const uint32_t zero = a.id();
	const uint32_t scope = a.id();
	const uint32_t semantics = a.id();
	const uint32_t value = a.id();
	const uint32_t counter_value = a.id();
	a.op(SpvOpConstant, { uint_type, zero, 0 });
	a.op(SpvOpConstant, { uint_type, counter_value, 1 });
	a.op(SpvOpConstant, { uint_type, scope, SpvScopeDevice });
	a.op(SpvOpConstant, { uint_type, semantics, SpvMemorySemanticsMaskNone });
	if (wide) {
		a.op(SpvOpConstant, { ulong_type, value, 0x89abcdefu, 0x01234567u });
	} else {
		a.op(SpvOpConstant, { uint_type, value, 0x89abcdefu });
	}

	const uint32_t element_pointer_type = a.id();
	uint32_t coord = 0;
	if (image) {
		const uint32_t image_type = a.id();
		const uint32_t image_pointer_type = a.id();
		const uint32_t coord_type = a.id();
		const uint32_t int_zero = a.id();
		coord = a.id();
		a.op(SpvOpTypeImage, { image_type, ulong_type, uint32_t(image_3d ? SpvDim3D : SpvDim2D), 0, image_array ? 1u : 0u, 0, 2, SpvImageFormatR64ui });
		a.op(SpvOpTypePointer, { image_pointer_type, SpvStorageClassUniformConstant, image_type });
		a.op(SpvOpVariable, { image_pointer_type, resource, SpvStorageClassUniformConstant });
		a.op(SpvOpTypePointer, { element_pointer_type, SpvStorageClassImage, ulong_type });
		a.op(SpvOpConstant, { int_type, int_zero, 0 });
		if (image_3d || image_array) {
			a.op(SpvOpTypeVector, { coord_type, int_type, 3 });
			a.op(SpvOpConstantComposite, { coord_type, coord, int_zero, int_zero, int_zero });
		} else {
			a.op(SpvOpTypeVector, { coord_type, int_type, 2 });
			a.op(SpvOpConstantComposite, { coord_type, coord, int_zero, int_zero });
		}
	} else if (shared) {
		a.op(SpvOpTypePointer, { element_pointer_type, SpvStorageClassWorkgroup, value_type });
		a.op(SpvOpVariable, { element_pointer_type, resource, SpvStorageClassWorkgroup });
	} else {
		const uint32_t block_pointer_type = a.id();
		a.op(SpvOpTypeRuntimeArray, { array_type, value_type });
		a.op(SpvOpTypeStruct, { block_type, array_type });
		a.op(SpvOpTypePointer, { block_pointer_type, SpvStorageClassStorageBuffer, block_type });
		a.op(SpvOpVariable, { block_pointer_type, resource, SpvStorageClassStorageBuffer });
		a.op(SpvOpTypePointer, { element_pointer_type, SpvStorageClassStorageBuffer, value_type });
	}
	const uint32_t counter_element_pointer_type = a.id();
	if (counter) {
		const uint32_t counter_block_pointer_type = a.id();
		a.op(SpvOpTypeRuntimeArray, { counter_array_type, uint_type });
		a.op(SpvOpTypeStruct, { counter_block_type, counter_array_type });
		a.op(SpvOpTypePointer, { counter_block_pointer_type, SpvStorageClassStorageBuffer, counter_block_type });
		a.op(SpvOpVariable, { counter_block_pointer_type, counter_resource, SpvStorageClassStorageBuffer });
		a.op(SpvOpTypePointer, { counter_element_pointer_type, SpvStorageClassStorageBuffer, uint_type });
	}

	a.op(SpvOpFunction, { void_type, main_fn, SpvFunctionControlMaskNone, fn_type });
	a.op(SpvOpLabel, { a.id() });
	// A shared variable is the atomic's pointer itself.
	const uint32_t pointer = shared ? resource : a.id();
	if (image) {
		a.op(SpvOpImageTexelPointer, { element_pointer_type, pointer, resource, coord, zero });
	} else if (!shared) {
		a.op(SpvOpAccessChain, { element_pointer_type, pointer, resource, zero, zero });
	}
	switch (p_kind) {
		case MODULE_BUFFER_UMAX:
		case MODULE_IMAGE_UMAX:
		case MODULE_IMAGE_3D_UMAX:
		case MODULE_IMAGE_ARRAY_UMAX:
		case MODULE_SHARED_UMAX:
		case MODULE_BUFFER_UMAX_32_BIT_IADD:
			a.op(SpvOpAtomicUMax, { value_type, a.id(), pointer, scope, semantics, value });
			break;
		case MODULE_BUFFER_UMIN:
			a.op(SpvOpAtomicUMin, { value_type, a.id(), pointer, scope, semantics, value });
			break;
		case MODULE_BUFFER_IADD:
		case MODULE_BUFFER_32_BIT_IADD:
			a.op(SpvOpAtomicIAdd, { value_type, a.id(), pointer, scope, semantics, value });
			break;
		case MODULE_BUFFER_STORE:
		case MODULE_IMAGE_STORE:
			a.op(SpvOpAtomicStore, { pointer, scope, semantics, value });
			break;
	}
	if (counter) {
		const uint32_t counter_pointer = a.id();
		a.op(SpvOpAccessChain, { counter_element_pointer_type, counter_pointer, counter_resource, zero, zero });
		a.op(SpvOpAtomicIAdd, { uint_type, a.id(), counter_pointer, scope, semantics, counter_value });
	}
	a.op(SpvOpReturn, {});
	a.op(SpvOpFunctionEnd, {});
	return a.bytes();
}

uint32_t scan(const Vector<uint8_t> &p_module) {
	return RDAtomics64::get_spirv_usage(p_module.ptr(), p_module.size());
}

Vector<RenderingDeviceCommons::ShaderStageSPIRVData> compute_stages(const Vector<uint8_t> &p_module) {
	RenderingDeviceCommons::ShaderStageSPIRVData stage;
	stage.shader_stage = RenderingDeviceCommons::SHADER_STAGE_COMPUTE;
	stage.spirv = p_module;
	Vector<RenderingDeviceCommons::ShaderStageSPIRVData> stages;
	stages.push_back(stage);
	return stages;
}

uint64_t feature_bit(RenderingDevice::Features p_feature) {
	return uint64_t(1) << p_feature;
}

const uint64_t ALL_INT64_ATOMIC_FEATURES = feature_bit(RenderingDevice::SUPPORTS_BUFFER_INT64_ATOMICS) | feature_bit(RenderingDevice::SUPPORTS_IMAGE_INT64_ATOMICS) | feature_bit(RenderingDevice::SUPPORTS_INT64_ATOMIC_ARITH);

// A container format with no native code: it keeps the reflection, which is what these cases check.
class Atomics64TestContainer : public RenderingShaderContainer {
	GDSOFTCLASS(Atomics64TestContainer, RenderingShaderContainer);

protected:
	virtual uint32_t _format() const override { return 0x34365441; }
	virtual uint32_t _format_version() const override { return 1; }
	virtual bool _set_code_from_spirv(const ReflectShader &p_shader) override { return true; }
};

TEST_CASE("[RD][Atomics64] The SPIR-V scan finds 64-bit atomics and which feature each needs") {
	const uint32_t buffer = RDAtomics64::USAGE_INT64_ATOMICS;
	const uint32_t image = RDAtomics64::USAGE_INT64_IMAGE_ATOMICS;
	const uint32_t arith = RDAtomics64::USAGE_INT64_ATOMIC_ARITH;

	// uint64 max and min are the portable operations.
	CHECK_EQ(scan(make_module(MODULE_BUFFER_UMAX)), buffer);
	CHECK_EQ(scan(make_module(MODULE_BUFFER_UMIN)), buffer);
	CHECK_EQ(scan(make_module(MODULE_IMAGE_UMAX)), buffer | image);
	// Add is found from the result type; a store from the type its pointer points to, through an
	// access chain or an image texel pointer.
	CHECK_EQ(scan(make_module(MODULE_BUFFER_IADD)), buffer | arith);
	CHECK_EQ(scan(make_module(MODULE_BUFFER_STORE)), buffer | arith);
	CHECK_EQ(scan(make_module(MODULE_IMAGE_STORE)), buffer | image | arith);
	// 64-bit types without 64-bit atomics need nothing.
	CHECK_EQ(scan(make_module(MODULE_BUFFER_32_BIT_IADD)), 0u);
	// Only 64-bit operations count: a 32-bit add next to a 64-bit max (the software rasterizer's
	// pattern) needs nothing more than the max.
	CHECK_EQ(scan(make_module(MODULE_BUFFER_UMAX_32_BIT_IADD)), buffer);
	// Max and min on shared memory, or with a 64-bit image that is not a plain 2D image, are not
	// portable: the Metal translation has neither.
	CHECK_EQ(scan(make_module(MODULE_SHARED_UMAX)), buffer | arith);
	CHECK_EQ(scan(make_module(MODULE_IMAGE_3D_UMAX)), buffer | image | arith);
	CHECK_EQ(scan(make_module(MODULE_IMAGE_ARRAY_UMAX)), buffer | image | arith);
}

TEST_CASE("[RD][Atomics64] The SPIR-V scan ignores data that is not a SPIR-V module") {
	CHECK_EQ(RDAtomics64::get_spirv_usage(nullptr, 0), 0u);
	const Vector<uint8_t> module = make_module(MODULE_BUFFER_IADD);
	// Shorter than a header, not whole words, or not starting with the magic number.
	CHECK_EQ(RDAtomics64::get_spirv_usage(module.ptr(), 16), 0u);
	CHECK_EQ(RDAtomics64::get_spirv_usage(module.ptr(), module.size() - 1), 0u);
	Vector<uint8_t> swapped = module;
	swapped.write[0] = 0x07;
	CHECK_EQ(scan(swapped), 0u);
	// A module cut inside its atomic (the last 3 words are the add's value, OpReturn and
	// OpFunctionEnd) keeps what was found before the cut: here, the capabilities.
	CHECK_EQ(RDAtomics64::get_spirv_usage(module.ptr(), module.size() - 12), uint32_t(RDAtomics64::USAGE_INT64_ATOMICS));
	CHECK_EQ(RDAtomics64::get_spirv_usage(module.ptr(), module.size() - 8), uint32_t(RDAtomics64::USAGE_INT64_ATOMICS | RDAtomics64::USAGE_INT64_ATOMIC_ARITH));
}

TEST_CASE("[RD][Atomics64] A device without 64-bit atomics reports the three features as false") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, 0);
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_BUFFER_INT64_ATOMICS));
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_IMAGE_INT64_ATOMICS));
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_INT64_ATOMIC_ARITH));
	CHECK_EQ(rd->get_missing_int64_atomics(scan(make_module(MODULE_IMAGE_STORE))), uint32_t(RDAtomics64::USAGE_INT64_ATOMICS | RDAtomics64::USAGE_INT64_IMAGE_ATOMICS | RDAtomics64::USAGE_INT64_ATOMIC_ARITH));

	rd->set_feature_mock(true, ALL_INT64_ATOMIC_FEATURES);
	CHECK(rd->has_feature(RenderingDevice::SUPPORTS_BUFFER_INT64_ATOMICS));
	CHECK(rd->has_feature(RenderingDevice::SUPPORTS_IMAGE_INT64_ATOMICS));
	CHECK(rd->has_feature(RenderingDevice::SUPPORTS_INT64_ATOMIC_ARITH));
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_ASYNC_COMPUTE));
	CHECK_EQ(rd->get_missing_int64_atomics(scan(make_module(MODULE_IMAGE_STORE))), 0u);
	memdelete(rd);
}

TEST_CASE("[RD][Atomics64] shader_create_from_spirv gives exactly one named error for a missing feature") {
	RenderingDevice *rd = memnew(RenderingDevice);
	Atomics64ErrorCapture errors;

	SUBCASE("No 64-bit atomics: a buffer max shader") {
		rd->set_feature_mock(true, 0);
		ERR_PRINT_OFF;
		const RID shader = rd->shader_create_from_spirv(compute_stages(make_module(MODULE_BUFFER_UMAX)), "atomics64_buffer");
		ERR_PRINT_ON;
		CHECK_FALSE(shader.is_valid());
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("Shader 'atomics64_buffer' uses 64-bit integer atomics that this device does not support. Missing RenderingDevice features: SUPPORTS_BUFFER_INT64_ATOMICS."));
	}
	SUBCASE("Buffer atomics only: an image max shader names the image feature") {
		rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_BUFFER_INT64_ATOMICS));
		ERR_PRINT_OFF;
		const RID shader = rd->shader_create_from_spirv(compute_stages(make_module(MODULE_IMAGE_UMAX)), "atomics64_image");
		ERR_PRINT_ON;
		CHECK_FALSE(shader.is_valid());
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, RDAtomics64::get_missing_features_error("atomics64_image", RDAtomics64::USAGE_INT64_IMAGE_ATOMICS));
		CHECK(errors.last_message.contains("SUPPORTS_IMAGE_INT64_ATOMICS"));
	}
	SUBCASE("Max and min but no arithmetic (Metal): a 64-bit add names the arithmetic feature") {
		rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_BUFFER_INT64_ATOMICS) | feature_bit(RenderingDevice::SUPPORTS_IMAGE_INT64_ATOMICS));
		ERR_PRINT_OFF;
		const RID shader = rd->shader_create_from_spirv(compute_stages(make_module(MODULE_BUFFER_IADD)), "");
		ERR_PRINT_ON;
		CHECK_FALSE(shader.is_valid());
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("A shader uses 64-bit integer atomics that this device does not support. Missing RenderingDevice features: SUPPORTS_INT64_ATOMIC_ARITH."));
	}
	SUBCASE("Nothing at all: every missing feature is in the one error") {
		rd->set_feature_mock(true, 0);
		ERR_PRINT_OFF;
		const RID shader = rd->shader_create_from_spirv(compute_stages(make_module(MODULE_IMAGE_STORE)), "atomics64_all");
		ERR_PRINT_ON;
		CHECK_FALSE(shader.is_valid());
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("Shader 'atomics64_all' uses 64-bit integer atomics that this device does not support. Missing RenderingDevice features: SUPPORTS_BUFFER_INT64_ATOMICS, SUPPORTS_IMAGE_INT64_ATOMICS, SUPPORTS_INT64_ATOMIC_ARITH."));
	}
	SUBCASE("shader_compile_binary_from_spirv (the ShaderRD path) checks before the driver translates") {
		// The device has no driver, so reaching the translation would crash.
		rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_BUFFER_INT64_ATOMICS) | feature_bit(RenderingDevice::SUPPORTS_IMAGE_INT64_ATOMICS));
		ERR_PRINT_OFF;
		const Vector<uint8_t> binary = rd->shader_compile_binary_from_spirv(compute_stages(make_module(MODULE_SHARED_UMAX)), "atomics64_shared");
		ERR_PRINT_ON;
		CHECK(binary.is_empty());
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("Shader 'atomics64_shared' uses 64-bit integer atomics that this device does not support. Missing RenderingDevice features: SUPPORTS_INT64_ATOMIC_ARITH."));
	}
	memdelete(rd);
}

TEST_CASE("[RD][Atomics64] The shader container keeps the usage and rejects older versions") {
	Ref<Atomics64TestContainer> container;
	container.instantiate();
	REQUIRE(container->set_code_from_spirv("atomics64_image_store", compute_stages(make_module(MODULE_IMAGE_STORE))));
	const uint32_t expected = RDAtomics64::USAGE_INT64_ATOMICS | RDAtomics64::USAGE_INT64_IMAGE_ATOMICS | RDAtomics64::USAGE_INT64_ATOMIC_ARITH;
	CHECK_EQ(container->get_int64_atomics_usage(), expected);
	CHECK_EQ(container->get_shader_reflection().int64_atomics, expected);
	// The image keeps its 64-bit format in the reflection.
	const RenderingDeviceCommons::ShaderReflection reflection = container->get_shader_reflection();
	REQUIRE_EQ(reflection.uniform_sets.size(), 1);
	REQUIRE_EQ(reflection.uniform_sets[0].size(), 1);
	CHECK_EQ(reflection.uniform_sets[0][0].type, RenderingDeviceCommons::UNIFORM_TYPE_IMAGE);
	CHECK_EQ(reflection.uniform_sets[0][0].texture_format, RenderingDeviceCommons::DATA_FORMAT_R64_UINT);

	const PackedByteArray bytes = container->to_bytes();
	Ref<Atomics64TestContainer> loaded;
	loaded.instantiate();
	REQUIRE(loaded->from_bytes(bytes));
	CHECK_EQ(loaded->get_int64_atomics_usage(), expected);

	Ref<Atomics64TestContainer> plain;
	plain.instantiate();
	REQUIRE(plain->set_code_from_spirv("atomics64_32_bit", compute_stages(make_module(MODULE_BUFFER_32_BIT_IADD))));
	CHECK_EQ(plain->get_int64_atomics_usage(), 0u);

	// A version-2 container has the shorter reflection layout: it is rejected, not misread.
	PackedByteArray old_bytes = bytes;
	const uint32_t old_version = 2;
	memcpy(old_bytes.ptrw() + sizeof(uint32_t), &old_version, sizeof(old_version));
	Ref<Atomics64TestContainer> old;
	old.instantiate();
	Atomics64ErrorCapture errors;
	ERR_PRINT_OFF;
	const bool old_loaded = old->from_bytes(old_bytes);
	ERR_PRINT_ON;
	CHECK_FALSE(old_loaded);
	CHECK_EQ(errors.count, 1);
	CHECK_EQ(errors.last_message, String("Unsupported version in shader container (2, expected 3); it must be rebuilt."));
}

} // namespace TestRDAtomics64

#endif // RD_ENABLED
