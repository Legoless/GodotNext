/**************************************************************************/
/*  rendering_device_bindless.cpp                                         */
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

#include "rendering_device_bindless.h"

#include <thirdparty/spirv-headers/include/spirv/unified1/spirv.h>

namespace RDBindless {

bool is_table_type(RenderingDeviceCommons::UniformType p_type) {
	return p_type == RenderingDeviceCommons::UNIFORM_TYPE_TEXTURE || p_type == RenderingDeviceCommons::UNIFORM_TYPE_SAMPLER || p_type == RenderingDeviceCommons::UNIFORM_TYPE_STORAGE_BUFFER;
}

RenderingDeviceCommons::Limit get_capacity_limit(RenderingDeviceCommons::UniformType p_type) {
	switch (p_type) {
		case RenderingDeviceCommons::UNIFORM_TYPE_SAMPLER:
			return RenderingDeviceCommons::LIMIT_MAX_BINDLESS_SAMPLERS;
		case RenderingDeviceCommons::UNIFORM_TYPE_STORAGE_BUFFER:
			return RenderingDeviceCommons::LIMIT_MAX_BINDLESS_STORAGE_BUFFERS;
		default:
			return RenderingDeviceCommons::LIMIT_MAX_BINDLESS_TEXTURES;
	}
}

const char *get_type_name(RenderingDeviceCommons::UniformType p_type) {
	switch (p_type) {
		case RenderingDeviceCommons::UNIFORM_TYPE_TEXTURE:
			return "UNIFORM_TYPE_TEXTURE";
		case RenderingDeviceCommons::UNIFORM_TYPE_SAMPLER:
			return "UNIFORM_TYPE_SAMPLER";
		case RenderingDeviceCommons::UNIFORM_TYPE_STORAGE_BUFFER:
			return "UNIFORM_TYPE_STORAGE_BUFFER";
		default:
			return "an unsupported type";
	}
}

bool spirv_uses_runtime_descriptor_arrays(const uint8_t *p_spirv, uint64_t p_size) {
	const uint64_t header_words = 5;
	if (p_spirv == nullptr || p_size < header_words * sizeof(uint32_t) || p_size % sizeof(uint32_t) != 0) {
		return false;
	}
	const uint64_t count = p_size / sizeof(uint32_t);
	auto word = [p_spirv](uint64_t p_index) -> uint32_t {
		uint32_t value;
		memcpy(&value, p_spirv + p_index * sizeof(uint32_t), sizeof(uint32_t));
		return value;
	};
	if (word(0) != SpvMagicNumber) {
		return false;
	}

	// Capabilities come first in a module.
	uint64_t i = header_words;
	while (i < count) {
		const uint32_t instruction = word(i);
		const uint32_t length = instruction >> SpvWordCountShift;
		if (length == 0 || i + length > count || (instruction & SpvOpCodeMask) != SpvOpCapability) {
			return false;
		}
		if (length >= 2 && word(i + 1) == SpvCapabilityRuntimeDescriptorArray) {
			return true;
		}
		i += length;
	}
	return false;
}

String get_unsupported_shader_error(const String &p_shader_name) {
	const String shader = p_shader_name.is_empty() ? String("A shader") : "Shader '" + p_shader_name + "'";
	return shader + " uses a bindless table, which this device does not support (SUPPORTS_BINDLESS).";
}

void SlotAllocator::initialize(uint32_t p_capacity) {
	capacity = p_capacity;
	next_unused = 0;
	allocated_count = 0;
	states.resize(p_capacity);
	if (p_capacity > 0) {
		memset(states.ptr(), SLOT_FREE, p_capacity);
	}
	free_slots.clear();
	released.clear();
	released_head = 0;
}

uint32_t SlotAllocator::allocate() {
	uint32_t slot = INVALID_SLOT;
	if (!free_slots.is_empty()) {
		slot = free_slots[free_slots.size() - 1];
		free_slots.resize(free_slots.size() - 1);
	} else if (next_unused < capacity) {
		slot = next_unused++;
	} else {
		return INVALID_SLOT;
	}

	DEV_ASSERT(states[slot] == SLOT_FREE);
	states[slot] = SLOT_ALLOCATED;
	allocated_count++;
	return slot;
}

bool SlotAllocator::release(uint32_t p_slot, uint64_t p_frame) {
	if (!is_allocated(p_slot)) {
		return false;
	}

	states[p_slot] = SLOT_RELEASED;
	allocated_count--;
	ReleasedSlot released_slot;
	released_slot.slot = p_slot;
	released_slot.frame = p_frame;
	released.push_back(released_slot);
	return true;
}

void SlotAllocator::reclaim(uint64_t p_frame, uint32_t p_frame_count) {
	while (released_head < released.size()) {
		const ReleasedSlot &released_slot = released[released_head];
		if (released_slot.frame + p_frame_count > p_frame) {
			// Later releases are not older, so they wait too.
			break;
		}

		DEV_ASSERT(states[released_slot.slot] == SLOT_RELEASED);
		states[released_slot.slot] = SLOT_FREE;
		free_slots.push_back(released_slot.slot);
		released_head++;
	}

	if (released_head == released.size()) {
		released.clear();
		released_head = 0;
	} else if (released_head > 1024 && released_head * 2 > released.size()) {
		// Drop the reclaimed records at the front once they are most of the list.
		const uint32_t remaining = released.size() - released_head;
		memmove(released.ptr(), released.ptr() + released_head, remaining * sizeof(ReleasedSlot));
		released.resize(remaining);
		released_head = 0;
	}
}

uint64_t SlotAllocator::get_memory_usage(uint32_t p_capacity) {
	return uint64_t(p_capacity) * (sizeof(uint8_t) + sizeof(uint32_t) + sizeof(ReleasedSlot));
}

void Residency::add(RID p_resource) {
	entries[p_resource].slots++;
}

bool Residency::remove(RID p_resource) {
	HashMap<RID, Entry>::Iterator E = entries.find(p_resource);
	ERR_FAIL_COND_V(!E, false);
	DEV_ASSERT(E->value.slots > 0);
	E->value.slots--;
	if (E->value.slots > 0) {
		return false;
	}

	const bool free_pending = E->value.free_pending;
	entries.remove(E);
	return free_pending;
}

bool Residency::defer_free(RID p_resource) {
	HashMap<RID, Entry>::Iterator E = entries.find(p_resource);
	if (!E) {
		return false;
	}

	E->value.free_pending = true;
	return true;
}

bool Residency::is_free_pending(RID p_resource) const {
	const Entry *entry = entries.getptr(p_resource);
	return entry != nullptr && entry->free_pending;
}

uint32_t Residency::get_slot_count(RID p_resource) const {
	const Entry *entry = entries.getptr(p_resource);
	return entry != nullptr ? entry->slots : 0;
}

} // namespace RDBindless
