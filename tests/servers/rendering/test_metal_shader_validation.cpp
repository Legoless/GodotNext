/**************************************************************************/
/*  test_metal_shader_validation.cpp                                      */
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

#include "drivers/metal/metal_shader_validation.h"
#include "tests/test_macros.h"

TEST_FORCE_LINK(test_metal_shader_validation)

TEST_CASE("[Metal][ShaderValidation] Argument-buffer stores are declared read-write") {
	CHECK_EQ(metal_shader_validation_usage(0u), 0u);
	CHECK_EQ(metal_shader_validation_usage(METAL_RESOURCE_USAGE_READ), METAL_RESOURCE_USAGE_READ);
	CHECK_EQ(metal_shader_validation_usage(METAL_RESOURCE_USAGE_WRITE), METAL_RESOURCE_USAGE_READ | METAL_RESOURCE_USAGE_WRITE);
	CHECK_EQ(metal_shader_validation_usage(METAL_RESOURCE_USAGE_READ | METAL_RESOURCE_USAGE_WRITE), METAL_RESOURCE_USAGE_READ | METAL_RESOURCE_USAGE_WRITE);
}

TEST_CASE("[Metal][ShaderValidation] SPIR-V NoContraction scanner toggles safe math mode") {
	constexpr uint32_t MAGIC = 0x07230203u;
	constexpr uint32_t OP_FUNCTION = 54u;
	constexpr uint32_t OP_DECORATE = 71u;
	constexpr uint32_t OP_MEMBER_DECORATE = 72u;
	constexpr uint32_t DECORATION_NO_CONTRACTION = 42u;
	constexpr uint32_t DECORATION_INVARIANT = 11u;

	// Null / short / bad magic.
	CHECK_FALSE(metal_spirv_uses_no_contraction(nullptr, 0));
	const uint32_t bad_magic[] = { 0x12345678u, 0x00010600u, 0, 10, 0 };
	CHECK_FALSE(metal_spirv_uses_no_contraction(bad_magic, 5));

	// Shader without NoContraction keeps fast math.
	const uint32_t plain[] = {
		MAGIC,
		0x00010600u,
		0,
		10,
		0,
		(3u << 16) | OP_DECORATE,
		5u,
		DECORATION_INVARIANT,
		(5u << 16) | OP_FUNCTION,
		1u,
		2u,
		0u,
		3u,
	};
	CHECK_FALSE(metal_spirv_uses_no_contraction(plain, sizeof(plain) / sizeof(plain[0])));

	// OpDecorate NoContraction (GLSL `precise`) disables fast math.
	const uint32_t precise_var[] = {
		MAGIC,
		0x00010600u,
		0,
		10,
		0,
		(3u << 16) | OP_DECORATE,
		7u,
		DECORATION_NO_CONTRACTION,
		(5u << 16) | OP_FUNCTION,
		1u,
		2u,
		0u,
		3u,
	};
	CHECK(metal_spirv_uses_no_contraction(precise_var, sizeof(precise_var) / sizeof(precise_var[0])));

	// OpMemberDecorate NoContraction also disables fast math.
	const uint32_t precise_member[] = {
		MAGIC,
		0x00010600u,
		0,
		10,
		0,
		(4u << 16) | OP_MEMBER_DECORATE,
		4u,
		2u,
		DECORATION_NO_CONTRACTION,
		(5u << 16) | OP_FUNCTION,
		1u,
		2u,
		0u,
		3u,
	};
	CHECK(metal_spirv_uses_no_contraction(precise_member, sizeof(precise_member) / sizeof(precise_member[0])));
}
