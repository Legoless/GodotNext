/**************************************************************************/
/*  test_rd_bindless.cpp                                                  */
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

TEST_FORCE_LINK(test_rd_bindless)

// Bindless tables through RenderingDevice (#176, plan §21.1 C4) without a GPU: the slot and lifetime
// rules, the render graph's dirty list and barriers, the reflection of runtime arrays, and the device's
// behavior without SUPPORTS_BINDLESS. godotnext/testbed/tests/rd_parity/bindless.gd runs tables on a GPU.

#ifdef RD_ENABLED

#include "core/error/error_macros.h"
#include "core/math/random_number_generator.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_bindless.h"
#include "servers/rendering/rendering_device_graph.h"
#include "servers/rendering/rendering_shader_container.h"

#include <thirdparty/spirv-headers/include/spirv/unified1/spirv.h>

namespace TestRDBindless {

using RDG = RenderingDeviceGraph;

// Records the errors printed while it is alive.
struct BindlessErrorCapture {
	ErrorHandlerList handler;
	int count = 0;
	String last_message;

	BindlessErrorCapture() {
		handler.errfunc = _capture;
		handler.userdata = this;
		add_error_handler(&handler);
	}

	~BindlessErrorCapture() {
		remove_error_handler(&handler);
	}

	static void _capture(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
		BindlessErrorCapture *self = static_cast<BindlessErrorCapture *>(p_self);
		self->count++;
		self->last_message = String::utf8((p_message != nullptr && *p_message != 0) ? p_message : p_error);
	}
};

/* Graph helpers. */

// A tracker for a whole 2D texture with one mip and one layer.
RDG::ResourceTracker *texture_tracker(uint64_t p_id) {
	RDG::ResourceTracker *texture = RDG::resource_tracker_create();
	texture->texture_driver_id = RDD::TextureID(p_id);
	texture->texture_subresources.aspect = RDD::TEXTURE_ASPECT_COLOR_BIT;
	texture->texture_subresources.mipmap_count = 1;
	texture->texture_subresources.layer_count = 1;
	texture->texture_usage = RDD::TEXTURE_USAGE_SAMPLING_BIT | RDD::TEXTURE_USAGE_STORAGE_BIT;
	return texture;
}

RDG::ResourceTracker *buffer_tracker(uint64_t p_id) {
	RDG::ResourceTracker *buffer = RDG::resource_tracker_create();
	buffer->buffer_driver_id = RDD::BufferID(p_id);
	return buffer;
}

// A graph that records barriers the way it does on Vulkan and D3D12 (image layouts).
void set_layout_traits(RDG &p_graph) {
	p_graph.set_driver_traits_for_test(true, true);
}

// A compute list that binds p_table (when given) and uses p_tracker with p_usage (when given).
void add_compute(RDG &p_graph, RDG::BindlessTableTracker *p_table, RDG::ResourceTracker *p_tracker = nullptr, RDG::ResourceUsage p_usage = RDG::RESOURCE_USAGE_NONE) {
	p_graph.add_compute_list_begin();
	p_graph.add_compute_list_bind_pipeline(RDD::PipelineID());
	if (p_table != nullptr) {
		p_graph.add_compute_list_bindless_table(p_table);
	}
	if (p_tracker != nullptr) {
		p_graph.add_compute_list_usage(p_tracker, p_usage);
	}
	p_graph.add_compute_list_dispatch(1, 1, 1);
	p_graph.add_compute_list_end();
}

// A draw list that binds p_table and draws into p_color.
void add_draw(RDG &p_graph, RDG::BindlessTableTracker *p_table, RDG::ResourceTracker *p_color) {
	const RDG::AttachmentOperation operation = RDG::ATTACHMENT_OPERATION_CLEAR;
	const RDD::RenderPassClearValue clear_value;
	p_graph.add_draw_list_begin(RDD::RenderPassID(), RDD::FramebufferID(), Rect2i(0, 0, 64, 64), VectorView<RDG::AttachmentOperation>(&operation, 1), VectorView<RDD::RenderPassClearValue>(&clear_value, 1), RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	p_graph.add_draw_list_bind_pipeline(RDD::PipelineID(), RDD::PIPELINE_STAGE_VERTEX_SHADER_BIT | RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	p_graph.add_draw_list_bindless_table(p_table);
	p_graph.add_draw_list_usage(p_color, RDG::RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE);
	p_graph.add_draw_list_draw(3, 1);
	p_graph.add_draw_list_end();
}

struct CommandBarriers {
	LocalVector<RDD::TextureBarrier> textures;
	LocalVector<RDD::BufferBarrier> buffers;
	RDD::MemoryAccessBarrier memory;
};

CommandBarriers barriers_of(const RDG &p_graph, int32_t p_command_index) {
	CommandBarriers barriers;
	p_graph.command_barriers_for_test(p_command_index, barriers.textures, barriers.buffers, barriers.memory);
	return barriers;
}

uint32_t texture_barrier_count(const CommandBarriers &p_barriers, uint64_t p_texture) {
	uint32_t count = 0;
	for (const RDD::TextureBarrier &barrier : p_barriers.textures) {
		count += barrier.texture.id == p_texture ? 1 : 0;
	}
	return count;
}

BitField<RDD::PipelineStageBits> previous_stages(const RDG &p_graph, int32_t p_command_index) {
	BitField<RDD::PipelineStageBits> self_stages;
	BitField<RDD::PipelineStageBits> previous;
	BitField<RDD::PipelineStageBits> next_stages;
	p_graph.command_get_stages(p_command_index, self_stages, previous, next_stages);
	return previous;
}

// The position of p_command_index in the order the graph would run the frame in.
int32_t order_of(const RDG &p_graph, int32_t p_command_index) {
	LocalVector<int32_t> order;
	RDG::AsyncPlan plan;
	p_graph.plan_for_test(order, plan, false);
	for (uint32_t i = 0; i < order.size(); i++) {
		if (order[i] == p_command_index) {
			return int32_t(i);
		}
	}
	return -1;
}

/* SPIR-V helpers. */

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

enum TableKind {
	TABLE_TEXTURES,
	TABLE_SAMPLERS,
	TABLE_READ_ONLY_BUFFERS,
	TABLE_WRITABLE_BUFFERS,
};

// A compute shader with a runtime array of p_kind at p_set, binding p_binding. With p_extra_binding, the
// set also has a uniform buffer at binding p_binding + 1.
Vector<uint8_t> make_table_module(TableKind p_kind, uint32_t p_set, uint32_t p_binding, bool p_extra_binding = false) {
	SpirvAssembler a;
	const uint32_t main_id = a.id();
	const uint32_t void_type = a.id();
	const uint32_t function_type = a.id();
	const uint32_t entry_type = a.id();
	const uint32_t array_type = a.id();
	const uint32_t pointer_type = a.id();
	const uint32_t table = a.id();
	const uint32_t label = a.id();
	const bool buffers = p_kind == TABLE_READ_ONLY_BUFFERS || p_kind == TABLE_WRITABLE_BUFFERS;
	const uint32_t uint_type = buffers || p_extra_binding ? a.id() : 0;
	const uint32_t uint_array = buffers ? a.id() : 0;
	const uint32_t float_type = p_kind == TABLE_TEXTURES ? a.id() : 0;
	const uint32_t extra_struct = p_extra_binding ? a.id() : 0;
	const uint32_t extra_pointer = p_extra_binding ? a.id() : 0;
	const uint32_t extra = p_extra_binding ? a.id() : 0;
	const SpvStorageClass storage = buffers ? SpvStorageClassStorageBuffer : SpvStorageClassUniformConstant;

	a.op(SpvOpCapability, { SpvCapabilityShader });
	a.op(SpvOpCapability, { SpvCapabilityRuntimeDescriptorArray });
	a.op_string(SpvOpExtension, {}, "SPV_EXT_descriptor_indexing");
	a.op(SpvOpMemoryModel, { SpvAddressingModelLogical, SpvMemoryModelGLSL450 });
	a.op_string(SpvOpEntryPoint, { SpvExecutionModelGLCompute, main_id }, "main");
	a.op(SpvOpExecutionMode, { main_id, SpvExecutionModeLocalSize, 1, 1, 1 });
	a.op(SpvOpDecorate, { table, SpvDecorationDescriptorSet, p_set });
	a.op(SpvOpDecorate, { table, SpvDecorationBinding, p_binding });
	if (buffers) {
		a.op(SpvOpDecorate, { uint_array, SpvDecorationArrayStride, 4 });
		a.op(SpvOpDecorate, { entry_type, SpvDecorationBlock });
		a.op(SpvOpMemberDecorate, { entry_type, 0, SpvDecorationOffset, 0 });
		if (p_kind == TABLE_READ_ONLY_BUFFERS) {
			a.op(SpvOpMemberDecorate, { entry_type, 0, SpvDecorationNonWritable });
			a.op(SpvOpDecorate, { table, SpvDecorationNonWritable });
		}
	}
	if (p_extra_binding) {
		a.op(SpvOpDecorate, { extra_struct, SpvDecorationBlock });
		a.op(SpvOpMemberDecorate, { extra_struct, 0, SpvDecorationOffset, 0 });
		a.op(SpvOpDecorate, { extra, SpvDecorationDescriptorSet, p_set });
		a.op(SpvOpDecorate, { extra, SpvDecorationBinding, p_binding + 1 });
	}

	a.op(SpvOpTypeVoid, { void_type });
	a.op(SpvOpTypeFunction, { function_type, void_type });
	switch (p_kind) {
		case TABLE_TEXTURES: {
			a.op(SpvOpTypeFloat, { float_type, 32 });
			// A sampled 2D image: OpTypeImage result sampled-type Dim Depth Arrayed MS Sampled Format.
			a.op(SpvOpTypeImage, { entry_type, float_type, SpvDim2D, 0, 0, 0, 1, SpvImageFormatUnknown });
		} break;
		case TABLE_SAMPLERS: {
			a.op(SpvOpTypeSampler, { entry_type });
		} break;
		case TABLE_READ_ONLY_BUFFERS:
		case TABLE_WRITABLE_BUFFERS: {
			a.op(SpvOpTypeInt, { uint_type, 32, 0 });
			a.op(SpvOpTypeRuntimeArray, { uint_array, uint_type });
			a.op(SpvOpTypeStruct, { entry_type, uint_array });
		} break;
	}
	if (p_extra_binding && !buffers) {
		a.op(SpvOpTypeInt, { uint_type, 32, 0 });
	}
	a.op(SpvOpTypeRuntimeArray, { array_type, entry_type });
	a.op(SpvOpTypePointer, { pointer_type, uint32_t(storage), array_type });
	a.op(SpvOpVariable, { pointer_type, table, uint32_t(storage) });
	if (p_extra_binding) {
		a.op(SpvOpTypeStruct, { extra_struct, uint_type });
		a.op(SpvOpTypePointer, { extra_pointer, SpvStorageClassUniform, extra_struct });
		a.op(SpvOpVariable, { extra_pointer, extra, SpvStorageClassUniform });
	}

	a.op(SpvOpFunction, { void_type, main_id, SpvFunctionControlMaskNone, function_type });
	a.op(SpvOpLabel, { label });
	a.op(SpvOpReturn, {});
	a.op(SpvOpFunctionEnd, {});
	return a.bytes();
}

// A compute shader with only a uniform buffer at set 0, binding 0.
Vector<uint8_t> make_plain_module() {
	SpirvAssembler a;
	const uint32_t main_id = a.id();
	const uint32_t void_type = a.id();
	const uint32_t function_type = a.id();
	const uint32_t uint_type = a.id();
	const uint32_t block_type = a.id();
	const uint32_t pointer_type = a.id();
	const uint32_t block = a.id();
	const uint32_t label = a.id();
	a.op(SpvOpCapability, { SpvCapabilityShader });
	a.op(SpvOpMemoryModel, { SpvAddressingModelLogical, SpvMemoryModelGLSL450 });
	a.op_string(SpvOpEntryPoint, { SpvExecutionModelGLCompute, main_id }, "main");
	a.op(SpvOpExecutionMode, { main_id, SpvExecutionModeLocalSize, 1, 1, 1 });
	a.op(SpvOpDecorate, { block_type, SpvDecorationBlock });
	a.op(SpvOpMemberDecorate, { block_type, 0, SpvDecorationOffset, 0 });
	a.op(SpvOpDecorate, { block, SpvDecorationDescriptorSet, 0 });
	a.op(SpvOpDecorate, { block, SpvDecorationBinding, 0 });
	a.op(SpvOpTypeVoid, { void_type });
	a.op(SpvOpTypeFunction, { function_type, void_type });
	a.op(SpvOpTypeInt, { uint_type, 32, 0 });
	a.op(SpvOpTypeStruct, { block_type, uint_type });
	a.op(SpvOpTypePointer, { pointer_type, SpvStorageClassUniform, block_type });
	a.op(SpvOpVariable, { pointer_type, block, SpvStorageClassUniform });
	a.op(SpvOpFunction, { void_type, main_id, SpvFunctionControlMaskNone, function_type });
	a.op(SpvOpLabel, { label });
	a.op(SpvOpReturn, {});
	a.op(SpvOpFunctionEnd, {});
	return a.bytes();
}

Vector<RenderingDeviceCommons::ShaderStageSPIRVData> compute_stages(const Vector<uint8_t> &p_module) {
	RenderingDeviceCommons::ShaderStageSPIRVData stage;
	stage.shader_stage = RenderingDeviceCommons::SHADER_STAGE_COMPUTE;
	stage.spirv = p_module;
	Vector<RenderingDeviceCommons::ShaderStageSPIRVData> stages;
	stages.push_back(stage);
	return stages;
}

class BindlessTestContainer : public RenderingShaderContainer {
	GDSOFTCLASS(BindlessTestContainer, RenderingShaderContainer);

protected:
	virtual uint32_t _format() const override { return 0x534c4442; }
	virtual uint32_t _format_version() const override { return 1; }
	virtual bool _set_code_from_spirv(const ReflectShader &p_shader) override { return true; }

public:
	// The reflection alone, so a failure prints only the reflection's own error.
	Error reflect_for_test(const String &p_shader_name, Vector<RenderingDeviceCommons::ShaderStageSPIRVData> p_spirv) {
		ReflectShader shader;
		return reflect_spirv(p_shader_name, p_spirv, shader);
	}
};

} // namespace TestRDBindless

using namespace TestRDBindless;

TEST_CASE("[RD][Bindless] A released slot is reused only after the frame ring has wrapped once") {
	const uint32_t frame_count = 3;
	RDBindless::SlotAllocator slots;
	slots.initialize(4);
	CHECK_EQ(slots.get_capacity(), 4u);
	for (uint32_t i = 0; i < 4; i++) {
		CHECK_EQ(slots.allocate(), i);
	}
	CHECK_EQ(slots.get_allocated_count(), 4u);
	CHECK_EQ(slots.allocate(), RDBindless::INVALID_SLOT);

	// Released at frame 10: still waiting at frames 10, 11 and 12, free at 13.
	CHECK(slots.release(2, 10));
	CHECK_FALSE(slots.is_allocated(2));
	CHECK_EQ(slots.get_released_count(), 1u);
	for (uint64_t frame = 10; frame < 10 + frame_count; frame++) {
		slots.reclaim(frame, frame_count);
		CHECK_EQ(slots.allocate(), RDBindless::INVALID_SLOT);
	}
	slots.reclaim(10 + frame_count, frame_count);
	CHECK_EQ(slots.get_released_count(), 0u);
	CHECK_EQ(slots.allocate(), 2u);

	// Only allocated slots can be released.
	CHECK(slots.release(1, 20));
	CHECK_FALSE(slots.release(1, 20));
	CHECK_FALSE(slots.release(4, 20));
	CHECK_FALSE(slots.release(RDBindless::INVALID_SLOT, 20));
	CHECK_EQ(slots.get_allocated_count(), 3u);
}

TEST_CASE("[RD][Bindless] A churn of 512 slots per frame never reuses a slot early") {
	// The CPU half of the GR-2 churn soak: 512 releases and 512 allocations a frame in a 65,536-slot
	// table, with a frame ring of 3. A slot is never allocated again before 3 frames have passed.
	const uint32_t frame_count = 3;
	const uint32_t churn = 512;
	RDBindless::SlotAllocator slots;
	slots.initialize(RDBindless::MIN_SLOTS);
	LocalVector<uint64_t> released_at;
	released_at.resize(RDBindless::MIN_SLOTS);
	for (uint64_t &frame : released_at) {
		frame = UINT64_MAX;
	}
	LocalVector<uint32_t> live;

	RandomNumberGenerator rng;
	rng.set_seed(0x42494e44);
	uint32_t early_reuses = 0;
	uint32_t failed_allocations = 0;
	uint32_t failed_releases = 0;
	for (uint64_t frame = 1; frame <= 1000; frame++) {
		slots.reclaim(frame, frame_count);
		for (uint32_t i = 0; i < churn && !live.is_empty(); i++) {
			const uint32_t index = rng.randi() % live.size();
			const uint32_t slot = live[index];
			live.remove_at_unordered(index);
			failed_releases += slots.release(slot, frame) ? 0 : 1;
			released_at[slot] = frame;
		}
		for (uint32_t i = 0; i < churn; i++) {
			const uint32_t slot = slots.allocate();
			if (slot == RDBindless::INVALID_SLOT) {
				failed_allocations++;
				continue;
			}
			if (released_at[slot] != UINT64_MAX && released_at[slot] + frame_count > frame) {
				early_reuses++;
			}
			live.push_back(slot);
		}
	}
	CHECK_EQ(early_reuses, 0u);
	CHECK_EQ(failed_allocations, 0u);
	CHECK_EQ(failed_releases, 0u);
	CHECK_EQ(slots.get_allocated_count(), live.size());
}

TEST_CASE("[RD][Bindless] A resource that a slot holds is freed when its last slot is released") {
	RDBindless::Residency residency;
	const RID texture = RID::from_uint64(1000);
	const RID other = RID::from_uint64(1001);

	CHECK_FALSE(residency.defer_free(other));

	residency.add(texture);
	residency.add(texture);
	CHECK(residency.is_resident(texture));
	CHECK_EQ(residency.get_slot_count(texture), 2u);

	// free_rid() while two slots hold it: deferred.
	CHECK(residency.defer_free(texture));
	CHECK(residency.is_free_pending(texture));
	// The first release keeps it; the last one frees it.
	CHECK_FALSE(residency.remove(texture));
	CHECK(residency.is_free_pending(texture));
	CHECK(residency.remove(texture));
	CHECK_FALSE(residency.is_resident(texture));
	CHECK_EQ(residency.get_resource_count(), 0u);

	// Without a free, the last release only forgets it.
	residency.add(other);
	CHECK_FALSE(residency.remove(other));
	CHECK_FALSE(residency.is_resident(other));
}

TEST_CASE("[RD][Bindless] The CPU bookkeeping of a 65,536-slot table stays under 4 MiB") {
	// Only the CPU side of a table: the slot allocator at full use and one RID per slot. The 4 MiB line of
	// plan §21.1.4 is about the GPU side, the table's entries: bindless_table_get_memory_usage() computes it
	// from the slot count and the driver's entry size, and rd_parity/bindless.gd checks it on each driver.
	const uint64_t bookkeeping = RDBindless::SlotAllocator::get_memory_usage(RDBindless::MIN_SLOTS) + uint64_t(RDBindless::MIN_SLOTS) * sizeof(RID);
	CHECK(bookkeeping <= RDBindless::MAX_TABLE_MEMORY);
}

TEST_CASE("[RD][Bindless] A write followed by a sample through the table emits exactly one barrier") {
	RDG graph;
	set_layout_traits(graph);
	RDG::BindlessTableTracker *table = RDG::bindless_table_tracker_create(RDG::RESOURCE_USAGE_TEXTURE_SAMPLE);
	RDG::ResourceTracker *textures[3] = { texture_tracker(11), texture_tracker(12), texture_tracker(13) };
	for (RDG::ResourceTracker *texture : textures) {
		RDG::bindless_table_add_resident(table, texture);
	}
	CHECK_EQ(table->dirty.size(), 3u);

	// The first bind makes every new resident ready, one transition each.
	graph.begin();
	add_compute(graph, table);
	CHECK_EQ(barriers_of(graph, 0).textures.size(), 3u);
	CHECK(table->dirty.is_empty());

	// Next frame: a compute list writes texture 11 as a storage image, then a compute list samples the
	// table. Only texture 11 gets a barrier, from the storage layout back to the sampled one.
	graph.begin();
	add_compute(graph, nullptr, textures[0], RDG::RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE);
	CHECK_EQ(table->dirty.size(), 1u);
	add_compute(graph, table);
	const CommandBarriers sample = barriers_of(graph, 1);
	REQUIRE_EQ(sample.textures.size(), 1u);
	CHECK_EQ(sample.textures[0].texture.id, 11u);
	CHECK_EQ(sample.textures[0].prev_layout, RDD::TEXTURE_LAYOUT_STORAGE_OPTIMAL);
	CHECK_EQ(sample.textures[0].next_layout, RDD::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	CHECK(sample.textures[0].dst_access.has_flag(RDD::BARRIER_ACCESS_SHADER_READ_BIT));
	CHECK(table->dirty.is_empty());
	// The sample runs after the write.
	CHECK(order_of(graph, 1) > order_of(graph, 0));

	// A bind with nothing dirty adds no barrier at all.
	graph.begin();
	add_compute(graph, table);
	CHECK_EQ(barriers_of(graph, 0).textures.size(), 0u);

	for (RDG::ResourceTracker *texture : textures) {
		RDG::bindless_table_remove_resident(table, texture);
		RDG::resource_tracker_free(texture);
	}
	RDG::bindless_table_tracker_free(table);
}

TEST_CASE("[RD][Bindless] The table orders its reads against writes it does not track as usages") {
	RDG graph;
	set_layout_traits(graph);
	RDG::BindlessTableTracker *table = RDG::bindless_table_tracker_create(RDG::RESOURCE_USAGE_TEXTURE_SAMPLE);
	RDG::ResourceTracker *texture = texture_tracker(21);
	RDG::ResourceTracker *color = texture_tracker(22);
	color->texture_usage = RDD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
	RDG::bindless_table_add_resident(table, texture);
	graph.begin();
	add_compute(graph, table);

	SUBCASE("Write after read: a write to a ready resident waits for the table's earlier reads") {
		// The writer is a texture clear, which the graph runs before a compute list when nothing orders
		// them (priority 2 against 4), so only the table's own ordering puts it after the read.
		graph.begin();
		add_compute(graph, table); // 0: reads the texture without a usage (it is ready).
		graph.add_texture_clear_color(RDD::TextureID(21), texture, Color(0, 0, 0, 0), texture->texture_subresources); // 1: writes it.
		CHECK(order_of(graph, 1) > order_of(graph, 0));
		CHECK(previous_stages(graph, 1).has_flag(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT));
		CHECK_EQ(table->dirty.size(), 1u);
	}
	SUBCASE("Read after write: a later bind waits for the bind that made the write ready") {
		graph.begin();
		add_compute(graph, nullptr, texture, RDG::RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE); // 0: writes it.
		add_compute(graph, table); // 1: makes it ready (one barrier).
		add_draw(graph, table, color); // 2: reads it in a fragment shader without a usage.
		CHECK_EQ(texture_barrier_count(barriers_of(graph, 1), 21), 1u);
		CHECK_EQ(texture_barrier_count(barriers_of(graph, 2), 21), 0u);
		CHECK(order_of(graph, 2) > order_of(graph, 1));
		CHECK(order_of(graph, 1) > order_of(graph, 0));
	}
	SUBCASE("Reads from earlier frames: their stages are waited for") {
		graph.begin();
		add_draw(graph, table, color); // 0: a fragment-stage read.
		graph.begin();
		add_compute(graph, nullptr, texture, RDG::RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE); // 0: writes it.
		CHECK(previous_stages(graph, 0).has_flag(RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT));
	}
	SUBCASE("A slice of a resident texture leaves its parent unready") {
		RDG::ResourceTracker *slice = texture_tracker(23);
		slice->parent = texture;
		slice->texture_slice_or_dirty_rect = Rect2i(0, 0, 1, 1);
		graph.begin();
		add_compute(graph, nullptr, slice, RDG::RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE);
		CHECK_EQ(table->dirty.size(), 1u);
		RDG::resource_tracker_free(slice);
	}

	RDG::bindless_table_remove_resident(table, texture);
	RDG::bindless_table_tracker_free(table);
	RDG::resource_tracker_free(texture);
	RDG::resource_tracker_free(color);
}

TEST_CASE("[RD][Bindless] Storage-buffer tables track writes, not other reads") {
	RDG graph;
	set_layout_traits(graph);
	RDG::BindlessTableTracker *table = RDG::bindless_table_tracker_create(RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
	RDG::ResourceTracker *buffers[2] = { buffer_tracker(31), buffer_tracker(32) };
	for (RDG::ResourceTracker *buffer : buffers) {
		RDG::bindless_table_add_resident(table, buffer);
	}
	graph.begin();
	add_compute(graph, table);
	CHECK(table->dirty.is_empty());

	// Read as a uniform buffer: still ready.
	graph.begin();
	add_compute(graph, nullptr, buffers[0], RDG::RESOURCE_USAGE_UNIFORM_BUFFER_READ);
	CHECK(table->dirty.is_empty());

	// Written by a copy: one buffer barrier at the next bind, for that buffer.
	graph.begin();
	graph.add_buffer_update(RDD::BufferID(31), buffers[0], VectorView<RDG::RecordedBufferCopy>());
	CHECK_EQ(table->dirty.size(), 1u);
	add_compute(graph, table);
	const CommandBarriers bind = barriers_of(graph, 1);
	REQUIRE_EQ(bind.buffers.size(), 1u);
	CHECK_EQ(bind.buffers[0].buffer.id, 31u);
	CHECK(bind.buffers[0].src_access.has_flag(RDD::BARRIER_ACCESS_COPY_WRITE_BIT));
	CHECK(bind.buffers[0].dst_access.has_flag(RDD::BARRIER_ACCESS_SHADER_READ_BIT));
	CHECK(table->dirty.is_empty());

	for (RDG::ResourceTracker *buffer : buffers) {
		RDG::bindless_table_remove_resident(table, buffer);
		RDG::resource_tracker_free(buffer);
	}
	RDG::bindless_table_tracker_free(table);
}

TEST_CASE("[RD][Bindless] A resource in two tables, or in two slots, is tracked once per table") {
	RDG::BindlessTableTracker *first = RDG::bindless_table_tracker_create(RDG::RESOURCE_USAGE_TEXTURE_SAMPLE);
	RDG::BindlessTableTracker *second = RDG::bindless_table_tracker_create(RDG::RESOURCE_USAGE_TEXTURE_SAMPLE);
	RDG::ResourceTracker *texture = texture_tracker(41);

	RDG::bindless_table_add_resident(first, texture);
	RDG::bindless_table_add_resident(first, texture);
	RDG::bindless_table_add_resident(second, texture);
	CHECK_EQ(texture->bindless_residency.size(), 2u);
	CHECK_EQ(first->dirty.size(), 1u);
	CHECK_EQ(second->dirty.size(), 1u);

	// Leaving one slot keeps it resident; leaving the last one takes it off the dirty list.
	RDG::bindless_table_remove_resident(first, texture);
	CHECK_EQ(first->dirty.size(), 1u);
	RDG::bindless_table_remove_resident(first, texture);
	CHECK(first->dirty.is_empty());
	CHECK_EQ(texture->bindless_residency.size(), 1u);

	// A bind of the second table makes it ready there only.
	RDG graph;
	set_layout_traits(graph);
	graph.begin();
	add_compute(graph, second);
	CHECK(second->dirty.is_empty());

	RDG::bindless_table_remove_resident(second, texture);
	CHECK(texture->bindless_residency.is_empty());
	RDG::resource_tracker_free(texture);
	RDG::bindless_table_tracker_free(first);
	RDG::bindless_table_tracker_free(second);
}

TEST_CASE("[RD][Bindless] A runtime array reflects as a bindless table") {
	struct Case {
		TableKind kind;
		RenderingDeviceCommons::UniformType entry_type;
	};
	const Case cases[] = {
		{ TABLE_TEXTURES, RenderingDeviceCommons::UNIFORM_TYPE_TEXTURE },
		{ TABLE_SAMPLERS, RenderingDeviceCommons::UNIFORM_TYPE_SAMPLER },
		{ TABLE_READ_ONLY_BUFFERS, RenderingDeviceCommons::UNIFORM_TYPE_STORAGE_BUFFER },
	};
	for (const Case &c : cases) {
		CAPTURE(c.kind);
		Ref<BindlessTestContainer> container;
		container.instantiate();
		REQUIRE(container->set_code_from_spirv("bindless_table", compute_stages(make_table_module(c.kind, 1, 0))));
		const RenderingDeviceCommons::ShaderReflection reflection = container->get_shader_reflection();
		REQUIRE_EQ(reflection.uniform_sets.size(), 2);
		CHECK(reflection.uniform_sets[0].is_empty());
		REQUIRE_EQ(reflection.uniform_sets[1].size(), 1);
		const RenderingDeviceCommons::ShaderUniform &uniform = reflection.uniform_sets[1][0];
		CHECK_EQ(uniform.type, RenderingDeviceCommons::UNIFORM_TYPE_BINDLESS_TABLE);
		CHECK_EQ(uniform.length, uint32_t(c.entry_type));
		CHECK_EQ(uniform.binding, 0u);
		CHECK_FALSE(uniform.writable);
		CHECK(container->uses_bindless_tables());

		// The entry type survives the container's serialization.
		Ref<BindlessTestContainer> loaded;
		loaded.instantiate();
		REQUIRE(loaded->from_bytes(container->to_bytes()));
		const RenderingDeviceCommons::ShaderReflection loaded_reflection = loaded->get_shader_reflection();
		REQUIRE_EQ(loaded_reflection.uniform_sets.size(), 2);
		REQUIRE_EQ(loaded_reflection.uniform_sets[1].size(), 1);
		CHECK_EQ(loaded_reflection.uniform_sets[1][0].type, RenderingDeviceCommons::UNIFORM_TYPE_BINDLESS_TABLE);
		CHECK_EQ(loaded_reflection.uniform_sets[1][0].length, uint32_t(c.entry_type));
		CHECK(loaded->uses_bindless_tables());
	}

	// A shader without a runtime array has no table, and RenderingDevice creates it without
	// SUPPORTS_BINDLESS.
	Ref<BindlessTestContainer> plain;
	plain.instantiate();
	REQUIRE(plain->set_code_from_spirv("plain", compute_stages(make_plain_module())));
	CHECK_FALSE(plain->uses_bindless_tables());
	const RenderingDeviceCommons::ShaderReflection plain_reflection = plain->get_shader_reflection();
	REQUIRE_EQ(plain_reflection.uniform_sets.size(), 1);
	REQUIRE_EQ(plain_reflection.uniform_sets[0].size(), 1);
	CHECK_EQ(plain_reflection.uniform_sets[0][0].type, RenderingDeviceCommons::UNIFORM_TYPE_UNIFORM_BUFFER);
}

TEST_CASE("[RD][Bindless] Shaders that break the table rules fail reflection with one named error") {
	BindlessErrorCapture errors;
	Ref<BindlessTestContainer> container;
	container.instantiate();

	SUBCASE("A table that shares its set") {
		ERR_PRINT_OFF;
		const bool ok = container->reflect_for_test("bindless_shared_set", compute_stages(make_table_module(TABLE_TEXTURES, 0, 0, true))) == OK;
		ERR_PRINT_ON;
		CHECK_FALSE(ok);
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("Shader 'bindless_shared_set': the bindless table in set 0 must be the set's only binding, at binding 0."));
	}
	SUBCASE("A table at a binding other than 0") {
		ERR_PRINT_OFF;
		const bool ok = container->reflect_for_test("bindless_binding_2", compute_stages(make_table_module(TABLE_TEXTURES, 1, 2))) == OK;
		ERR_PRINT_ON;
		CHECK_FALSE(ok);
		CHECK_EQ(errors.count, 1);
		CHECK(errors.last_message.contains("must be the set's only binding, at binding 0"));
	}
	SUBCASE("A table of writable storage buffers") {
		ERR_PRINT_OFF;
		const bool ok = container->reflect_for_test("bindless_writable", compute_stages(make_table_module(TABLE_WRITABLE_BUFFERS, 1, 0))) == OK;
		ERR_PRINT_ON;
		CHECK_FALSE(ok);
		CHECK_EQ(errors.count, 1);
		CHECK(errors.last_message.contains("bindless storage buffers are read-only"));
	}
}

TEST_CASE("[RD][Bindless] Without SUPPORTS_BINDLESS, a shader with a table fails with one named error before the driver") {
	const Vector<uint8_t> table_module = make_table_module(TABLE_TEXTURES, 1, 0);
	const Vector<uint8_t> plain_module = make_plain_module();
	CHECK(RDBindless::spirv_uses_runtime_descriptor_arrays(table_module.ptr(), table_module.size()));
	CHECK_FALSE(RDBindless::spirv_uses_runtime_descriptor_arrays(plain_module.ptr(), plain_module.size()));
	CHECK_FALSE(RDBindless::spirv_uses_runtime_descriptor_arrays(nullptr, 0));
	CHECK_FALSE(RDBindless::spirv_uses_runtime_descriptor_arrays(table_module.ptr(), table_module.size() - 2));
	Vector<uint8_t> not_spirv = table_module;
	not_spirv.write[0] ^= 0xff;
	CHECK_FALSE(RDBindless::spirv_uses_runtime_descriptor_arrays(not_spirv.ptr(), not_spirv.size()));

	// The device has no driver, so reaching the translation would crash.
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, 0);
	BindlessErrorCapture errors;

	SUBCASE("shader_create_from_spirv") {
		ERR_PRINT_OFF;
		const RID shader = rd->shader_create_from_spirv(compute_stages(table_module), "bindless_unsupported");
		ERR_PRINT_ON;
		CHECK_FALSE(shader.is_valid());
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("Shader 'bindless_unsupported' uses a bindless table, which this device does not support (SUPPORTS_BINDLESS)."));
	}
	SUBCASE("shader_compile_binary_from_spirv (the ShaderRD path)") {
		ERR_PRINT_OFF;
		const Vector<uint8_t> binary = rd->shader_compile_binary_from_spirv(compute_stages(table_module), "");
		ERR_PRINT_ON;
		CHECK(binary.is_empty());
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("A shader uses a bindless table, which this device does not support (SUPPORTS_BINDLESS)."));
	}
	memdelete(rd);
}

TEST_CASE("[RD][Bindless] A device without SUPPORTS_BINDLESS fails table creation with one named error") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, 0);
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_BINDLESS));

	BindlessErrorCapture errors;
	ERR_PRINT_OFF;
	const RID table = rd->bindless_table_create(RenderingDevice::UNIFORM_TYPE_TEXTURE, RDBindless::MIN_SLOTS);
	ERR_PRINT_ON;
	CHECK_FALSE(table.is_valid());
	CHECK_EQ(errors.count, 1);
	CHECK_EQ(errors.last_message, String("This device does not support bindless tables (SUPPORTS_BINDLESS)."));

	ERR_PRINT_OFF;
	const uint32_t slot = rd->bindless_table_allocate(RID(), RID());
	ERR_PRINT_ON;
	CHECK_EQ(slot, RDBindless::INVALID_SLOT);
	CHECK_EQ(errors.count, 2);
	CHECK_EQ(errors.last_message, String("Invalid bindless table."));

	// The mocked feature reads back where a device has it.
	rd->set_feature_mock(true, uint64_t(1) << RenderingDevice::SUPPORTS_BINDLESS);
	CHECK(rd->has_feature(RenderingDevice::SUPPORTS_BINDLESS));
	memdelete(rd);
}

#endif // RD_ENABLED
