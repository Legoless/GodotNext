/**************************************************************************/
/*  test_rd_indirect_count.cpp                                            */
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

TEST_FORCE_LINK(test_rd_indirect_count)

// GPU-count indirect draws through RenderingDevice (#177, plan §21.1, GR-6) without a GPU: the Direct3D 12
// command signature cache, the two features, the named errors of a device without them, and the render
// graph's handling of the count buffer and of buffers a draw list uses two ways.
// godotnext/testbed/tests/rd_parity/indirect_count.gd runs the draws.

#ifdef RD_ENABLED

#include "core/error/error_macros.h"
#include "core/object/class_db.h"
#include "drivers/d3d12/d3d12_command_signature_cache.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_device_graph.h"

namespace TestRDIndirectCount {

// Records the errors printed while it is alive.
struct IndirectCountErrorCapture {
	ErrorHandlerList handler;
	int count = 0;
	String last_message;

	IndirectCountErrorCapture() {
		handler.errfunc = _capture;
		handler.userdata = this;
		add_error_handler(&handler);
	}

	~IndirectCountErrorCapture() {
		remove_error_handler(&handler);
	}

	static void _capture(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
		IndirectCountErrorCapture *self = static_cast<IndirectCountErrorCapture *>(p_self);
		self->count++;
		self->last_message = String::utf8((p_message != nullptr && *p_message != 0) ? p_message : p_error);
	}
};

uint64_t feature_bit(RenderingDevice::Features p_feature) {
	return uint64_t(1) << p_feature;
}

// The D3D12_INDIRECT_ARGUMENT_TYPE values and record sizes the driver uses.
enum : uint32_t {
	ARGUMENT_DRAW = 0,
	ARGUMENT_DRAW_INDEXED = 1,
	ARGUMENT_DISPATCH = 2,
	DRAW_SIZE = 16,
	DRAW_INDEXED_SIZE = 20,
};

// Stands in for ID3D12Device::CreateCommandSignature: each signature is a new number.
struct SignatureFactory {
	int created = 0;
	bool fail = false;

	bool operator()(uint32_t p_type, uint32_t p_stride, int &r_signature) {
		if (fail) {
			return false;
		}
		r_signature = 1000 + ++created;
		return true;
	}
};

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

// A draw list whose pipeline reads in the vertex shader.
void begin_draw(RDG &p_graph) {
	p_graph.add_draw_list_begin(RDD::RenderPassID(), RDD::FramebufferID(), Rect2i(0, 0, 64, 64), VectorView<RDG::AttachmentOperation>(), VectorView<RDD::RenderPassClearValue>(), RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	p_graph.add_draw_list_bind_pipeline(RDD::PipelineID(), RDD::PIPELINE_STAGE_VERTEX_SHADER_BIT);
}

// Whether command p_command_index has a barrier that makes buffer p_buffer readable by shaders.
bool has_shader_read_barrier(const RDG &p_graph, int32_t p_command_index, uint64_t p_buffer) {
	LocalVector<RDD::TextureBarrier> textures;
	LocalVector<RDD::BufferBarrier> buffers;
	RDD::MemoryAccessBarrier memory;
	p_graph.command_barriers_for_test(p_command_index, textures, buffers, memory);
	for (const RDD::BufferBarrier &barrier : buffers) {
		if (barrier.buffer.id == p_buffer && barrier.dst_access.has_flag(RDD::BARRIER_ACCESS_SHADER_READ_BIT)) {
			return true;
		}
	}
	return false;
}

} // namespace TestRDIndirectCount

using namespace TestRDIndirectCount;

TEST_CASE("[RD][IndirectCount] The D3D12 command signature cache keeps one signature per type and stride") {
	using Cache = D3D12CommandSignatureCache<int>;
	Cache cache;
	SignatureFactory factory;
	bool valid = false;

	// The three tightly packed signatures the driver makes with the device.
	const int draw = cache.get(ARGUMENT_DRAW, Cache::get_stride(0, DRAW_SIZE, 1), factory, valid);
	CHECK(valid);
	const int draw_indexed = cache.get(ARGUMENT_DRAW_INDEXED, Cache::get_stride(0, DRAW_INDEXED_SIZE, 1), factory, valid);
	CHECK(valid);
	const int dispatch = cache.get(ARGUMENT_DISPATCH, Cache::get_stride(0, 12, 1), factory, valid);
	CHECK(valid);
	CHECK_EQ(factory.created, 3);
	CHECK_EQ(cache.size(), 3u);
	CHECK_NE(draw, draw_indexed);
	CHECK_NE(draw_indexed, dispatch);

	// The fixture's strides: each (type, stride) gets its own signature, made once.
	const uint32_t strides[] = { 20, 32, 64 };
	int indexed_signatures[3] = {};
	for (uint32_t i = 0; i < 3; i++) {
		indexed_signatures[i] = cache.get(ARGUMENT_DRAW_INDEXED, Cache::get_stride(strides[i], DRAW_INDEXED_SIZE, 4096), factory, valid);
		CHECK(valid);
	}
	// 20 bytes is the tightly packed indexed record: the signature made with the device.
	CHECK_EQ(indexed_signatures[0], draw_indexed);
	CHECK_NE(indexed_signatures[1], draw_indexed);
	CHECK_NE(indexed_signatures[2], indexed_signatures[1]);
	CHECK_EQ(factory.created, 5);
	for (uint32_t i = 0; i < 3; i++) {
		CHECK_EQ(cache.get(ARGUMENT_DRAW_INDEXED, strides[i], factory, valid), indexed_signatures[i]);
	}
	CHECK_EQ(factory.created, 5);

	// The same stride for another type is another signature.
	const int draw_32 = cache.get(ARGUMENT_DRAW, 32, factory, valid);
	CHECK(valid);
	CHECK_NE(draw_32, indexed_signatures[1]);
	CHECK_EQ(factory.created, 6);
	CHECK_EQ(cache.size(), 6u);
	CHECK_NE(Cache::get_key(ARGUMENT_DRAW, 32), Cache::get_key(ARGUMENT_DRAW_INDEXED, 32));
	CHECK_NE(Cache::get_key(ARGUMENT_DRAW, 32), Cache::get_key(ARGUMENT_DRAW, 64));

	// A failed creation is not cached: the next request tries again.
	factory.fail = true;
	cache.get(ARGUMENT_DRAW, 48, factory, valid);
	CHECK_FALSE(valid);
	CHECK_EQ(cache.size(), 6u);
	factory.fail = false;
	cache.get(ARGUMENT_DRAW, 48, factory, valid);
	CHECK(valid);
	CHECK_EQ(cache.size(), 7u);

	cache.clear();
	CHECK_EQ(cache.size(), 0u);
}

TEST_CASE("[RD][IndirectCount] The signature stride follows the records") {
	using Cache = D3D12CommandSignatureCache<int>;
	// Tightly packed records (stride 0) use the record size.
	CHECK_EQ(Cache::get_stride(0, DRAW_SIZE, 4096), uint32_t(DRAW_SIZE));
	CHECK_EQ(Cache::get_stride(0, DRAW_INDEXED_SIZE, 4096), uint32_t(DRAW_INDEXED_SIZE));
	// Several records keep their stride, even when it is not the record size.
	CHECK_EQ(Cache::get_stride(20, DRAW_SIZE, 2), 20u);
	CHECK_EQ(Cache::get_stride(32, DRAW_INDEXED_SIZE, 4096), 32u);
	CHECK_EQ(Cache::get_stride(64, DRAW_INDEXED_SIZE, 4096), 64u);
	// One record (or none) never steps by the stride, so it shares the tightly packed signature.
	CHECK_EQ(Cache::get_stride(64, DRAW_INDEXED_SIZE, 1), uint32_t(DRAW_INDEXED_SIZE));
	CHECK_EQ(Cache::get_stride(64, DRAW_SIZE, 0), uint32_t(DRAW_SIZE));
}

TEST_CASE("[RD][IndirectCount] The features are appended to the bound Features enum") {
	// After SUPPORTS_BINDLESS (21), so no bound value moves (plan §21.1 C2).
	CHECK_EQ(int(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT), 22);
	CHECK_EQ(int(RenderingDevice::SUPPORTS_DRAW_INDIRECT_FIRST_INSTANCE), 23);
	CHECK_EQ(int(RenderingDevice::SUPPORTS_INT64_ATOMIC_ARITH), 20);
	CHECK_EQ(int(RenderingDevice::SUPPORTS_BINDLESS), 21);
	bool valid = false;
	CHECK_EQ(ClassDB::get_integer_constant("RenderingDevice", "SUPPORTS_DRAW_INDIRECT_COUNT", &valid), 22);
	CHECK(valid);
	CHECK_EQ(ClassDB::get_integer_constant("RenderingDevice", "SUPPORTS_DRAW_INDIRECT_FIRST_INSTANCE", &valid), 23);
	CHECK(valid);
	CHECK(ClassDB::has_method("RenderingDevice", "draw_list_draw_indirect_count"));
	CHECK(ClassDB::has_property("RDShaderSPIRV", "base_instance_native"));

	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, 0);
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT));
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_DRAW_INDIRECT_FIRST_INSTANCE));
	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT));
	CHECK(rd->has_feature(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT));
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_DRAW_INDIRECT_FIRST_INSTANCE));
	memdelete(rd);
}

TEST_CASE("[RD][IndirectCount] A count draw on a device without SUPPORTS_DRAW_INDIRECT_COUNT gives one named error") {
	RenderingDevice *rd = memnew(RenderingDevice);
	IndirectCountErrorCapture errors;

	SUBCASE("Without the feature") {
		rd->set_feature_mock(true, 0);
		ERR_PRINT_OFF;
		rd->draw_list_draw_indirect_count(0, true, RID(), 0, RID(), 0, 4096, 32);
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("This device does not support indirect draws with a GPU count (RenderingDevice feature SUPPORTS_DRAW_INDIRECT_COUNT)."));
	}
	SUBCASE("With the feature, the draw list is checked next") {
		rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT));
		ERR_PRINT_OFF;
		rd->draw_list_draw_indirect_count(0, true, RID(), 0, RID(), 0, 4096, 32);
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 1);
		CHECK_FALSE(errors.last_message.contains("SUPPORTS_DRAW_INDIRECT_COUNT"));
	}
	memdelete(rd);
}

TEST_CASE("[RD][IndirectCount] A base_instance_native shader needs SUPPORTS_DRAW_INDIRECT_FIRST_INSTANCE") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT));
	IndirectCountErrorCapture errors;

	// The check comes before the SPIR-V is read or the driver is reached (this device has none).
	Vector<uint8_t> module;
	module.resize(32);
	module.fill(0);
	RenderingDeviceCommons::ShaderStageSPIRVData stage;
	stage.shader_stage = RenderingDeviceCommons::SHADER_STAGE_VERTEX;
	stage.spirv = module;
	Vector<RenderingDeviceCommons::ShaderStageSPIRVData> stages;
	stages.push_back(stage);

	SUBCASE("shader_compile_binary_from_spirv (the ShaderRD path)") {
		ERR_PRINT_OFF;
		const Vector<uint8_t> binary = rd->shader_compile_binary_from_spirv(stages, "indirect_count_native", true);
		ERR_PRINT_ON;
		CHECK(binary.is_empty());
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("Shader 'indirect_count_native' is compiled with base_instance_native, which this device does not support. Missing RenderingDevice feature: SUPPORTS_DRAW_INDIRECT_FIRST_INSTANCE."));
	}
	SUBCASE("shader_create_from_spirv, without a name") {
		ERR_PRINT_OFF;
		const RID shader = rd->shader_create_from_spirv(stages, "", true);
		ERR_PRINT_ON;
		CHECK_FALSE(shader.is_valid());
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("A shader is compiled with base_instance_native, which this device does not support. Missing RenderingDevice feature: SUPPORTS_DRAW_INDIRECT_FIRST_INSTANCE."));
	}
	SUBCASE("The bound method reads RDShaderSPIRV.base_instance_native") {
		Ref<RDShaderSPIRV> spirv;
		spirv.instantiate();
		spirv->set_stage_bytecode(RenderingDeviceCommons::SHADER_STAGE_VERTEX, module);
		spirv->set_base_instance_native(true);
		ERR_PRINT_OFF;
		const RID shader = rd->call("shader_create_from_spirv", spirv, "bound");
		ERR_PRINT_ON;
		CHECK_FALSE(shader.is_valid());
		CHECK_EQ(errors.count, 1);
		CHECK(errors.last_message.begins_with("Shader 'bound' is compiled with base_instance_native"));
	}
	memdelete(rd);
}

TEST_CASE("[RD][IndirectCount] The render graph reads the count buffer as an indirect argument") {
	RDG graph;
	RDG::ResourceTracker *records = RDG::resource_tracker_create();
	RDG::ResourceTracker *count = RDG::resource_tracker_create();
	RDG::ResourceTracker *unrelated = RDG::resource_tracker_create();
	records->buffer_driver_id = RDD::BufferID(11);
	count->buffer_driver_id = RDD::BufferID(12);
	unrelated->buffer_driver_id = RDD::BufferID(13);

	// A compute pass writes the count; command 0.
	graph.add_compute_list_begin(RDD::BreadcrumbMarker::NONE, 0, RDG::QUEUE_MAIN);
	graph.add_compute_list_bind_pipeline(RDD::PipelineID());
	graph.add_compute_list_usage(count, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE);
	graph.add_compute_list_dispatch(1, 1, 1);
	graph.add_compute_list_end();

	// A compute pass writes a buffer no draw reads; command 1.
	graph.add_compute_list_begin(RDD::BreadcrumbMarker::NONE, 0, RDG::QUEUE_MAIN);
	graph.add_compute_list_bind_pipeline(RDD::PipelineID());
	graph.add_compute_list_usage(unrelated, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE);
	graph.add_compute_list_dispatch(1, 1, 1);
	graph.add_compute_list_end();

	// The draw list with the count draw; command 2. Only the count ties it to command 0.
	graph.add_draw_list_begin(RDD::RenderPassID(), RDD::FramebufferID(), Rect2i(0, 0, 64, 64), VectorView<RDG::AttachmentOperation>(), VectorView<RDD::RenderPassClearValue>(), RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	graph.add_draw_list_draw(3, 1);
	CHECK(graph.add_draw_list_draw_indirect_count(true, RDD::BufferID(11), records, 64, RDD::BufferID(12), count, 16, 4096, 32));
	CHECK(graph.add_draw_list_draw_indirect_count(false, RDD::BufferID(11), records, 0, RDD::BufferID(12), count, 20, 7, 0));
	graph.add_draw_list_end();

	// Both buffers are used as indirect arguments, so their barriers carry the indirect-command read access.
	CHECK_EQ(records->usage, RDG::RESOURCE_USAGE_INDIRECT_BUFFER_READ);
	CHECK_EQ(count->usage, RDG::RESOURCE_USAGE_INDIRECT_BUFFER_READ);
	CHECK_EQ(unrelated->usage, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE);

	BitField<RDD::PipelineStageBits> self_stages;
	BitField<RDD::PipelineStageBits> previous_stages;
	BitField<RDD::PipelineStageBits> next_stages;
	graph.command_get_stages(2, self_stages, previous_stages, next_stages);
	// The draws read their records and count in the draw-indirect stage, after the compute pass that wrote
	// the count.
	CHECK(self_stages.has_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT));
	CHECK(previous_stages.has_flag(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT));
	graph.command_get_stages(0, self_stages, previous_stages, next_stages);
	CHECK(next_stages.has_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT));
	graph.command_get_stages(1, self_stages, previous_stages, next_stages);
	CHECK_FALSE(next_stages.has_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT));

	// Both draws are listed for the driver to prepare before the render pass, in order, as recorded.
	const LocalVector<RDG::IndirectCountDraw> draws = graph.indirect_count_draws_for_test(2);
	REQUIRE_EQ(draws.size(), 2u);
	CHECK(draws[0].indexed);
	CHECK_EQ(draws[0].buffer.id, 11u);
	CHECK_EQ(draws[0].offset, 64u);
	CHECK_EQ(draws[0].count_buffer.id, 12u);
	CHECK_EQ(draws[0].count_buffer_offset, 16u);
	CHECK_EQ(draws[0].max_draw_count, 4096u);
	CHECK_EQ(draws[0].stride, 32u);
	CHECK_FALSE(draws[1].indexed);
	CHECK_EQ(draws[1].count_buffer_offset, 20u);
	CHECK_EQ(draws[1].max_draw_count, 7u);
	CHECK_EQ(draws[1].stride, 0u);

	RDG::resource_tracker_free(records);
	RDG::resource_tracker_free(count);
	RDG::resource_tracker_free(unrelated);
}

TEST_CASE("[RD][IndirectCount] A count draw whose buffers the draw list also uses another way gives one named error") {
	RDG graph;
	RDG::ResourceTracker *records = RDG::resource_tracker_create();
	RDG::ResourceTracker *count = RDG::resource_tracker_create();
	records->buffer_driver_id = RDD::BufferID(11);
	count->buffer_driver_id = RDD::BufferID(12);
	IndirectCountErrorCapture errors;

	graph.add_draw_list_begin(RDD::RenderPassID(), RDD::FramebufferID(), Rect2i(0, 0, 64, 64), VectorView<RDG::AttachmentOperation>(), VectorView<RDD::RenderPassClearValue>(), RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	// Draw 1 reads the records and the count, each from the other buffer, as indirect arguments.
	CHECK(graph.add_draw_list_draw_indirect_count(true, RDD::BufferID(11), records, 0, RDD::BufferID(12), count, 0, 16, 32));
	CHECK_EQ(errors.count, 0);

	SUBCASE("The records buffer bound in a uniform set") {
		RDG::ResourceTracker *other_records = RDG::resource_tracker_create();
		graph.add_draw_list_usage(other_records, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
		ERR_PRINT_OFF;
		CHECK_FALSE(graph.add_draw_list_draw_indirect_count(true, RDD::BufferID(13), other_records, 0, RDD::BufferID(12), count, 0, 16, 32));
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("The indirect buffer of an indirect count draw is also used in this draw list (Storage Buffer Read). A buffer has one usage per draw list, so it would not be synchronized for the indirect read; bind it in none of this draw list's uniform sets."));
		CHECK_EQ(other_records->draw_list_usage, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
		RDG::resource_tracker_free(other_records);
	}
	SUBCASE("The count buffer bound in a uniform set") {
		RDG::ResourceTracker *other_count = RDG::resource_tracker_create();
		graph.add_draw_list_usage(other_count, RDG::RESOURCE_USAGE_UNIFORM_BUFFER_READ);
		ERR_PRINT_OFF;
		CHECK_FALSE(graph.add_draw_list_draw_indirect_count(false, RDD::BufferID(11), records, 0, RDD::BufferID(14), other_count, 0, 16, 0));
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 1);
		CHECK(errors.last_message.begins_with("The count buffer of an indirect count draw is also used in this draw list (Uniform Buffer Read)."));
		RDG::resource_tracker_free(other_count);
	}
	SUBCASE("The same buffers again, and one buffer for both") {
		CHECK(graph.add_draw_list_draw_indirect_count(false, RDD::BufferID(11), records, 64, RDD::BufferID(12), count, 4, 8, 0));
		CHECK(graph.add_draw_list_draw_indirect_count(false, RDD::BufferID(11), records, 64, RDD::BufferID(11), records, 0, 8, 0));
		CHECK_EQ(errors.count, 0);
	}
	graph.add_draw_list_end();

	RDG::resource_tracker_free(records);
	RDG::resource_tracker_free(count);
}

TEST_CASE("[RD][IndirectCount] An indirect draw's buffer that a bound bindless table has not read since a write gives one named error") {
	// A storage-buffer table holds both buffers. The table's read in a draw list becomes a usage only at the
	// list's end, and a buffer has one usage per draw list, so it must not meet the indirect read there.
	RDG graph;
	graph.set_driver_traits_for_test(true, true);
	RDG::BindlessTableTracker *table = RDG::bindless_table_tracker_create(RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
	RDG::ResourceTracker *records = RDG::resource_tracker_create();
	RDG::ResourceTracker *count = RDG::resource_tracker_create();
	records->buffer_driver_id = RDD::BufferID(11);
	count->buffer_driver_id = RDD::BufferID(12);
	RDG::bindless_table_add_resident(table, records);
	RDG::bindless_table_add_resident(table, count);

	// A first bind of the table makes both ready.
	graph.begin();
	add_compute(graph, table);
	REQUIRE(table->dirty.is_empty());
	graph.begin();
	IndirectCountErrorCapture errors;

	SUBCASE("The table bound before the count draw: the draw is refused, and the table's read gets its barrier") {
		add_compute(graph, nullptr, records, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE); // 0: writes the records.
		REQUIRE_EQ(table->dirty.size(), 1u);
		begin_draw(graph);
		graph.add_draw_list_bindless_table(table);
		ERR_PRINT_OFF;
		CHECK_FALSE(graph.add_draw_list_draw_indirect_count(true, RDD::BufferID(11), records, 0, RDD::BufferID(12), count, 0, 16, 32));
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("The indirect buffer of an indirect count draw is in a bindless table that this draw list binds, and the table has not read it since it was last written. A buffer has one usage per draw list, so the table's read would not be synchronized; bind the table in an earlier list after the write, or keep the buffer out of the table."));
		graph.add_draw_list_draw(3, 1);
		graph.add_draw_list_end(); // 1
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(records->usage, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
		CHECK(has_shader_read_barrier(graph, 1, 11));
		CHECK(table->dirty.is_empty());
	}
	SUBCASE("The table bound before the count draw, with the count buffer written") {
		add_compute(graph, nullptr, count, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE);
		begin_draw(graph);
		graph.add_draw_list_bindless_table(table);
		ERR_PRINT_OFF;
		CHECK_FALSE(graph.add_draw_list_draw_indirect_count(false, RDD::BufferID(11), records, 0, RDD::BufferID(12), count, 0, 16, 0));
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 1);
		CHECK(errors.last_message.begins_with("The count buffer of an indirect count draw is in a bindless table that this draw list binds"));
		graph.add_draw_list_end();
		CHECK_EQ(errors.count, 1);
		CHECK(table->dirty.is_empty());
	}
	SUBCASE("The table bound after the count draw: the list's end names it, and the buffer stays dirty for the next bind") {
		add_compute(graph, nullptr, records, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE); // 0: writes the records.
		begin_draw(graph);
		CHECK(graph.add_draw_list_draw_indirect_count(true, RDD::BufferID(11), records, 0, RDD::BufferID(12), count, 0, 16, 32));
		graph.add_draw_list_bindless_table(table);
		graph.add_draw_list_draw(3, 1);
		CHECK_EQ(errors.count, 0);
		ERR_PRINT_OFF;
		graph.add_draw_list_end(); // 1
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("A buffer in a bindless table that this draw list binds is also the indirect or count buffer of one of its indirect draws, and the table has not read it since it was last written. A buffer has one usage per draw list, so the table's reads in this draw list are not synchronized; bind the table in an earlier list after the write, or keep the buffer out of the table."));
		CHECK_EQ(records->usage, RDG::RESOURCE_USAGE_INDIRECT_BUFFER_READ);
		CHECK_FALSE(has_shader_read_barrier(graph, 1, 11));
		REQUIRE_EQ(table->dirty.size(), 1u);
		CHECK_EQ(table->dirty[0], records);
		// The next command that binds the table gets the barrier the draw list could not have.
		add_compute(graph, table); // 2
		CHECK(has_shader_read_barrier(graph, 2, 11));
		CHECK(table->dirty.is_empty());
		CHECK_EQ(errors.count, 1);
	}
	SUBCASE("A plain indirect draw: the list's end names it as well") {
		add_compute(graph, nullptr, records, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE);
		begin_draw(graph);
		graph.add_draw_list_bindless_table(table);
		graph.add_draw_list_draw_indexed_indirect(RDD::BufferID(11), 0, 1, 0);
		graph.add_draw_list_usage(records, RDG::RESOURCE_USAGE_INDIRECT_BUFFER_READ); // As RenderingDevice adds it.
		ERR_PRINT_OFF;
		graph.add_draw_list_end();
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 1);
		CHECK(errors.last_message.begins_with("A buffer in a bindless table that this draw list binds is also the indirect or count buffer"));
		CHECK_EQ(table->dirty.size(), 1u);
	}
	SUBCASE("A resident the table has read since its last write: the count draw is recorded") {
		begin_draw(graph);
		graph.add_draw_list_bindless_table(table);
		CHECK(graph.add_draw_list_draw_indirect_count(true, RDD::BufferID(11), records, 0, RDD::BufferID(12), count, 0, 16, 32));
		graph.add_draw_list_end();
		CHECK_EQ(errors.count, 0);
		CHECK_EQ(records->usage, RDG::RESOURCE_USAGE_INDIRECT_BUFFER_READ);
		CHECK(table->dirty.is_empty());
	}

	RDG::bindless_table_remove_resident(table, records);
	RDG::bindless_table_remove_resident(table, count);
	RDG::bindless_table_tracker_free(table);
	RDG::resource_tracker_free(records);
	RDG::resource_tracker_free(count);
}

#endif // RD_ENABLED
