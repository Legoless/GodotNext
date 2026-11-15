/**************************************************************************/
/*  rendering_device_atomics64.h                                          */
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

#include "core/string/ustring.h"

// GodotNext: rd_atomics64 (#175). 64-bit integer atomics through RenderingDevice (plan §21.1 C2 and
// C3). The portable operations are uint64 atomicMax and atomicMin on storage buffers and on 2D R64_UINT
// storage images. RenderingDevice scans the SPIR-V of every shader it creates for the 64-bit atomics
// it uses, and fails shader creation with one named error when the device lacks a matching feature.
namespace RDAtomics64 {

enum Usage : uint32_t {
	// The Int64Atomics capability: any 64-bit integer atomic. Needs SUPPORTS_BUFFER_INT64_ATOMICS.
	// An image-only shader declares it too, which is why drivers report the image feature only
	// together with the buffer one.
	USAGE_INT64_ATOMICS = 1 << 0,
	// The Int64ImageEXT capability: 64-bit atomics on R64 storage images. Needs
	// SUPPORTS_IMAGE_INT64_ATOMICS.
	USAGE_INT64_IMAGE_ATOMICS = 1 << 1,
	// A 64-bit integer atomic other than OpAtomicUMax and OpAtomicUMin, a 64-bit OpAtomicUMax or
	// OpAtomicUMin on shared (Workgroup) memory, or, in a module with 64-bit atomics, a 64-bit image
	// that is not a plain 2D image (arrayed, multisampled or another dimension). Needs
	// SUPPORTS_INT64_ATOMIC_ARITH.
	USAGE_INT64_ATOMIC_ARITH = 1 << 2,
};

// Scans one SPIR-V module. Returns 0 for anything that is not a little-endian SPIR-V module, and the
// usage found so far when the module is cut short. The arithmetic bit is found from each atomic's
// result type, or for OpAtomicStore from the type of its pointer, and the storage class of a max or
// min from the type of its pointer, when that pointer comes from a variable, an access chain, an image
// texel pointer, a function parameter or a copy.
uint32_t get_spirv_usage(const uint8_t *p_spirv, uint64_t p_size);

// The error shader creation fails with. It names the shader (when it has a name) and every feature
// in p_missing_usage.
String get_missing_features_error(const String &p_shader_name, uint32_t p_missing_usage);

} // namespace RDAtomics64
