/**************************************************************************/
/*  rendering_device_bindless.h                                           */
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
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "servers/rendering/rendering_device_commons.h"

// GodotNext: rd_bindless (#176). Bindless tables through RenderingDevice (plan §21.1 C4). A table is a
// runtime array of read-only textures, samplers or storage buffers that a shader indexes at run time.
// RenderingDevice keeps the slots and the lifetime rules here, so they can be tested without a GPU:
// - A released slot is reused only after the frame ring has wrapped once (frames.size() frames), so
//   work already recorded with the old entry has finished before the slot is written again.
// - Freeing a resource that a slot still holds is deferred until its last slot is released.
namespace RDBindless {

// Texture and storage-buffer tables guarantee at least this many slots on a device with
// SUPPORTS_BINDLESS. Sampler tables are capped lower, by the API (LIMIT_MAX_BINDLESS_SAMPLERS).
static constexpr uint32_t MIN_SLOTS = 65536;
// The fewest slots a sampler table must offer for SUPPORTS_BINDLESS.
static constexpr uint32_t MIN_SAMPLER_SLOTS = 1024;
// The slot RenderingDevice::bindless_table_allocate() returns when it fails.
static constexpr uint32_t INVALID_SLOT = UINT32_MAX;
// The budget line of plan §21.1.4: a table of MIN_SLOTS slots takes at most this much memory.
static constexpr uint64_t MAX_TABLE_MEMORY = 4 * 1024 * 1024;

// Whether p_type can be the entry type of a table: UNIFORM_TYPE_TEXTURE, UNIFORM_TYPE_SAMPLER or
// UNIFORM_TYPE_STORAGE_BUFFER.
bool is_table_type(RenderingDeviceCommons::UniformType p_type);
// The limit that caps the capacity of a table of p_type, or LIMIT_MAX_BINDLESS_TEXTURES for a type
// that is not a table type.
RenderingDeviceCommons::Limit get_capacity_limit(RenderingDeviceCommons::UniformType p_type);
// The entry type's name, for errors.
const char *get_type_name(RenderingDeviceCommons::UniformType p_type);
// Whether a SPIR-V module declares the RuntimeDescriptorArray capability, which a runtime array of
// descriptors (a bindless table) needs. Read from the capabilities at the start of the module, so it is
// cheap enough to check before the driver translates a shader. False for data that is not SPIR-V.
bool spirv_uses_runtime_descriptor_arrays(const uint8_t *p_spirv, uint64_t p_size);
// The one error a shader with a bindless table gives on a device without SUPPORTS_BINDLESS.
String get_unsupported_shader_error(const String &p_shader_name);

// The slots of one table.
class SlotAllocator {
	enum SlotState : uint8_t {
		SLOT_FREE,
		SLOT_ALLOCATED,
		SLOT_RELEASED, // Released, waiting for the frame ring to wrap.
	};

	struct ReleasedSlot {
		uint32_t slot = 0;
		uint64_t frame = 0;
	};

	uint32_t capacity = 0;
	// Slots from here on were never allocated.
	uint32_t next_unused = 0;
	uint32_t allocated_count = 0;
	LocalVector<uint8_t> states;
	// Slots that can be allocated again.
	LocalVector<uint32_t> free_slots;
	// Released slots in release order (so in frame order), from released_head on.
	LocalVector<ReleasedSlot> released;
	uint32_t released_head = 0;

public:
	void initialize(uint32_t p_capacity);
	// The most recently reclaimed slot first, then the next slot never used. INVALID_SLOT when every
	// slot is allocated or still waiting after a release.
	uint32_t allocate();
	// p_frame is the device's frame counter (RenderingDevice::get_frames_drawn()). Returns false, and
	// changes nothing, when p_slot is not allocated.
	bool release(uint32_t p_slot, uint64_t p_frame);
	// Makes the slots released at frame f reusable once p_frame >= f + p_frame_count: by then the
	// device has waited for the frame that recorded the release.
	void reclaim(uint64_t p_frame, uint32_t p_frame_count);

	bool is_allocated(uint32_t p_slot) const { return p_slot < capacity && states[p_slot] == SLOT_ALLOCATED; }
	uint32_t get_capacity() const { return capacity; }
	uint32_t get_allocated_count() const { return allocated_count; }
	uint32_t get_released_count() const { return released.size() - released_head; }
	// The CPU memory of the bookkeeping at full use: one state byte, one free-list entry and one
	// release record per slot.
	static uint64_t get_memory_usage(uint32_t p_capacity);
};

// Which resources the slots of every table hold, and the frees deferred until no slot holds them.
class Residency {
	struct Entry {
		uint32_t slots = 0;
		bool free_pending = false;
	};

	HashMap<RID, Entry> entries;

public:
	// One more slot holds p_resource.
	void add(RID p_resource);
	// One slot less holds p_resource. Returns true when no slot holds it any more and its free was
	// deferred: the caller frees it now.
	bool remove(RID p_resource);
	// Called by free_rid(). Returns true, and marks the free as pending, when a slot holds
	// p_resource, so the free waits.
	bool defer_free(RID p_resource);

	bool is_resident(RID p_resource) const { return entries.has(p_resource); }
	bool is_free_pending(RID p_resource) const;
	uint32_t get_slot_count(RID p_resource) const;
	uint32_t get_resource_count() const { return entries.size(); }
};

} // namespace RDBindless
