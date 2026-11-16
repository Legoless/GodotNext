/**************************************************************************/
/*  d3d12_command_signature_cache.h                                       */
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

// GodotNext: rd_indirect_count (#177). ExecuteIndirect reads records with the byte stride of its command
// signature, so the driver keeps one signature per (argument type, stride). The cache has no Direct3D 12
// types, so the doctests check it on every platform. Lookups are locked, so command buffers may be
// recorded on any thread. T must not overload the address-of operator: HashMap takes its values'
// addresses, and taking the address of a WRL ComPtr releases it (wrap one in a struct).

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"

template <typename T>
class D3D12CommandSignatureCache {
	HashMap<uint64_t, T> signatures;
	mutable Mutex mutex;

public:
	// The stride the signature needs: p_stride, or p_record_size when the records are tightly packed
	// (p_stride 0) or there is at most one record, whose stride ExecuteIndirect never uses.
	static uint32_t get_stride(uint32_t p_stride, uint32_t p_record_size, uint32_t p_max_count) {
		return (p_stride == 0 || p_max_count <= 1) ? p_record_size : p_stride;
	}

	static uint64_t get_key(uint32_t p_type, uint32_t p_stride) {
		return (uint64_t(p_type) << 32) | p_stride;
	}

	// The signature for (p_type, p_stride). The first request creates it with
	// p_create(p_type, p_stride, T &r_signature), which returns false on failure. r_valid is false when the
	// creation failed; a failure is not cached, so the next request tries again.
	template <typename F>
	T get(uint32_t p_type, uint32_t p_stride, F &&p_create, bool &r_valid) {
		MutexLock lock(mutex);
		const uint64_t key = get_key(p_type, p_stride);
		if (const T *signature = signatures.getptr(key)) {
			r_valid = true;
			return *signature;
		}
		T signature = T();
		r_valid = p_create(p_type, p_stride, signature);
		if (r_valid) {
			signatures.insert(key, signature);
		}
		return signature;
	}

	uint32_t size() const {
		MutexLock lock(mutex);
		return signatures.size();
	}

	void clear() {
		MutexLock lock(mutex);
		signatures.clear();
	}
};
