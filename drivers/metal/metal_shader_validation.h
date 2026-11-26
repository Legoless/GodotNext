/**************************************************************************/
/*  metal_shader_validation.h                                             */
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

#include <cstddef>
#include <cstdint>

// MTLResourceUsage values. Kept free of Metal headers so the policy can be
// tested on every platform. metal3_objects.cpp static_asserts these against
// MTL::ResourceUsageRead and MTL::ResourceUsageWrite.
static constexpr uint32_t METAL_RESOURCE_USAGE_READ = 1u;
static constexpr uint32_t METAL_RESOURCE_USAGE_WRITE = 2u;

// GodotNext: metal_shader_validation
// useHeaps marks every placement-heap resource read-only. Argument-buffer
// stores (buffer atomics, imageStore) are then invalid unless useResource
// includes Write. A write-only binding is checked as read-write, so a store
// also needs Read. Read-only bindings are left unchanged: sampled textures
// already match the heap's read residency.
inline uint32_t metal_shader_validation_usage(uint32_t p_usage) {
	if ((p_usage & METAL_RESOURCE_USAGE_WRITE) != 0) {
		p_usage |= METAL_RESOURCE_USAGE_READ;
	}
	return p_usage;
}

// Returns true when a SPIR-V binary carries DecorationNoContraction (GLSL `precise`),
// indicating the shader must compile with MTL::MathModeSafe rather than MTL::MathModeFast.
// Kept free of SPIR-V/Metal headers so the scanner is unit-tested on every platform.
inline bool metal_spirv_uses_no_contraction(const uint32_t *p_words, size_t p_word_count) {
	constexpr uint32_t SPIRV_MAGIC = 0x07230203u;
	constexpr uint32_t OP_FUNCTION = 54u;
	constexpr uint32_t OP_DECORATE = 71u;
	constexpr uint32_t OP_MEMBER_DECORATE = 72u;
	constexpr uint32_t DECORATION_NO_CONTRACTION = 42u;
	if (p_words == nullptr || p_word_count < 5 || p_words[0] != SPIRV_MAGIC) {
		return false;
	}
	size_t i = 5;
	while (i < p_word_count) {
		const uint32_t word_count = p_words[i] >> 16;
		const uint32_t opcode = p_words[i] & 0xFFFFu;
		if (word_count == 0 || i + word_count > p_word_count) {
			break;
		}
		if (opcode == OP_FUNCTION) {
			break;
		}
		if (opcode == OP_DECORATE && word_count >= 3 && p_words[i + 2] == DECORATION_NO_CONTRACTION) {
			return true;
		}
		if (opcode == OP_MEMBER_DECORATE && word_count >= 4 && p_words[i + 3] == DECORATION_NO_CONTRACTION) {
			return true;
		}
		i += word_count;
	}
	return false;
}
