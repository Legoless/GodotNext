/**************************************************************************/
/*  next_gi_sdf.cpp                                                       */
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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,         */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "next_gi_sdf.h"

NextGISDFPool::NextGISDFPool(uint32_t p_size_mb) {
	size_mb = MAX(p_size_mb, 1u);
	const uint64_t total = get_size_bytes() / BRICK_BYTES;
	capacity = uint32_t(MIN(total, uint64_t(UINT32_MAX)));
	free_list.resize(capacity);
	for (uint32_t index = 0; index < capacity; index++) {
		free_list[index] = capacity - 1 - index; // Pop from the end.
	}
	slots.resize(capacity);
	memset(slots.ptr(), 0, capacity);
}

bool NextGISDFPool::allocate(uint32_t p_count, LocalVector<uint32_t> &r_slots) {
	r_slots.clear();
	if (p_count == 0) {
		return true;
	}
	if (p_count > get_free_count()) {
		return false;
	}
	r_slots.resize(p_count);
	for (uint32_t index = 0; index < p_count; index++) {
		const uint32_t slot = free_list[free_list.size() - 1];
		free_list.remove_at(free_list.size() - 1);
		slots[slot] = 1;
		r_slots[index] = slot;
	}
	used += p_count;
	return true;
}

void NextGISDFPool::free(const LocalVector<uint32_t> &p_slots) {
	for (const uint32_t &slot : p_slots) {
		free_slot(slot);
	}
}

void NextGISDFPool::free_slot(uint32_t p_slot) {
	if (p_slot >= capacity || slots[p_slot] == 0) {
		return;
	}
	slots[p_slot] = 0;
	free_list.push_back(p_slot);
	used--;
}

bool NextGISDFPool::is_allocated(uint32_t p_slot) const {
	return p_slot < capacity && slots[p_slot] != 0;
}
