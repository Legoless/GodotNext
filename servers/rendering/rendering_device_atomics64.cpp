/**************************************************************************/
/*  rendering_device_atomics64.cpp                                        */
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

#include "rendering_device_atomics64.h"

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"

#include <thirdparty/spirv-headers/include/spirv/unified1/spirv.h>

namespace RDAtomics64 {

static uint32_t _int_width(const HashMap<uint32_t, uint32_t> &p_int_widths, uint32_t p_type) {
	const uint32_t *width = p_int_widths.getptr(p_type);
	return width != nullptr ? *width : 0;
}

uint32_t get_spirv_usage(const uint8_t *p_spirv, uint64_t p_size) {
	const uint64_t header_size = 5 * sizeof(uint32_t);
	if (p_spirv == nullptr || p_size < header_size || p_size % sizeof(uint32_t) != 0) {
		return 0;
	}
	LocalVector<uint32_t> words;
	words.resize(p_size / sizeof(uint32_t));
	memcpy(words.ptr(), p_spirv, p_size);
	if (words[0] != SpvMagicNumber) {
		return 0;
	}

	const uint32_t count = words.size();
	uint32_t usage = 0;
	uint32_t i = 5;

	// Capabilities come first in a module, and without Int64Atomics it has no 64-bit integer atomic.
	while (i < count) {
		const uint32_t length = words[i] >> SpvWordCountShift;
		if (length == 0 || i + length > count) {
			return usage;
		}
		if ((words[i] & SpvOpCodeMask) != SpvOpCapability) {
			break;
		}
		if (length >= 2) {
			if (words[i + 1] == SpvCapabilityInt64Atomics) {
				usage |= USAGE_INT64_ATOMICS;
			} else if (words[i + 1] == SpvCapabilityInt64ImageEXT) {
				usage |= USAGE_INT64_IMAGE_ATOMICS;
			}
		}
		i += length;
	}
	if (!(usage & USAGE_INT64_ATOMICS)) {
		return usage;
	}

	// Types and pointers are declared before the functions that use them, and a pointer result comes
	// before its uses, so one pass sees every type an atomic refers to.
	HashMap<uint32_t, uint32_t> int_widths; // OpTypeInt result -> width.
	HashMap<uint32_t, uint32_t> pointees; // OpTypePointer result -> pointee type.
	HashMap<uint32_t, uint32_t> storage_classes; // OpTypePointer result -> storage class.
	HashMap<uint32_t, uint32_t> pointer_types; // Result of a pointer-producing instruction -> its type.
	while (i < count) {
		const uint32_t length = words[i] >> SpvWordCountShift;
		if (length == 0 || i + length > count) {
			return usage;
		}
		const uint32_t *op = &words[i];
		switch (op[0] & SpvOpCodeMask) {
			case SpvOpTypeInt: {
				if (length >= 4) {
					int_widths.insert(op[1], op[2]);
				}
			} break;
			case SpvOpTypePointer: {
				if (length >= 4) {
					pointees.insert(op[1], op[3]);
					storage_classes.insert(op[1], op[2]);
				}
			} break;
			case SpvOpTypeImage: {
				// The Metal translation (SPIRV-Cross) has 64-bit image atomics on plain 2D images only, so
				// any other 64-bit image counts as the non-portable kind.
				if (length >= 9 && _int_width(int_widths, op[2]) == 64 && (op[3] != SpvDim2D || op[5] != 0 || op[6] != 0)) {
					usage |= USAGE_INT64_ATOMIC_ARITH;
				}
			} break;
			case SpvOpFunctionParameter:
			case SpvOpVariable:
			case SpvOpImageTexelPointer:
			case SpvOpAccessChain:
			case SpvOpInBoundsAccessChain:
			case SpvOpPtrAccessChain:
			case SpvOpInBoundsPtrAccessChain:
			case SpvOpCopyObject: {
				if (length >= 3) {
					pointer_types.insert(op[2], op[1]);
				}
			} break;
			case SpvOpAtomicUMin:
			case SpvOpAtomicUMax: {
				// The portable operations, except on shared memory, which Metal has no 64-bit atomics for.
				if (length >= 4 && _int_width(int_widths, op[1]) == 64) {
					const uint32_t *pointer_type = pointer_types.getptr(op[3]);
					const uint32_t *storage_class = pointer_type != nullptr ? storage_classes.getptr(*pointer_type) : nullptr;
					if (storage_class != nullptr && *storage_class == SpvStorageClassWorkgroup) {
						usage |= USAGE_INT64_ATOMIC_ARITH;
					}
				}
			} break;
			case SpvOpAtomicLoad:
			case SpvOpAtomicExchange:
			case SpvOpAtomicCompareExchange:
			case SpvOpAtomicCompareExchangeWeak:
			case SpvOpAtomicIIncrement:
			case SpvOpAtomicIDecrement:
			case SpvOpAtomicIAdd:
			case SpvOpAtomicISub:
			case SpvOpAtomicSMin:
			case SpvOpAtomicSMax:
			case SpvOpAtomicAnd:
			case SpvOpAtomicOr:
			case SpvOpAtomicXor: {
				// An integer atomic's result type is the type it operates on.
				if (length >= 2 && _int_width(int_widths, op[1]) == 64) {
					usage |= USAGE_INT64_ATOMIC_ARITH;
				}
			} break;
			case SpvOpAtomicStore: {
				if (length >= 2) {
					const uint32_t *pointer_type = pointer_types.getptr(op[1]);
					const uint32_t *pointee = pointer_type != nullptr ? pointees.getptr(*pointer_type) : nullptr;
					if (pointee != nullptr && _int_width(int_widths, *pointee) == 64) {
						usage |= USAGE_INT64_ATOMIC_ARITH;
					}
				}
			} break;
			default: {
			} break;
		}
		i += length;
	}
	return usage;
}

String get_missing_features_error(const String &p_shader_name, uint32_t p_missing_usage) {
	Vector<String> features;
	if (p_missing_usage & USAGE_INT64_ATOMICS) {
		features.push_back("SUPPORTS_BUFFER_INT64_ATOMICS");
	}
	if (p_missing_usage & USAGE_INT64_IMAGE_ATOMICS) {
		features.push_back("SUPPORTS_IMAGE_INT64_ATOMICS");
	}
	if (p_missing_usage & USAGE_INT64_ATOMIC_ARITH) {
		features.push_back("SUPPORTS_INT64_ATOMIC_ARITH");
	}
	const String shader = p_shader_name.is_empty() ? String("A shader") : "Shader '" + p_shader_name + "'";
	return shader + " uses 64-bit integer atomics that this device does not support. Missing RenderingDevice features: " + String(", ").join(features) + ".";
}

} // namespace RDAtomics64
