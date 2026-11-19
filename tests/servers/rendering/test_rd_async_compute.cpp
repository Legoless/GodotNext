/**************************************************************************/
/*  test_rd_async_compute.cpp                                             */
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

TEST_FORCE_LINK(test_rd_async_compute)

// Async compute queue (#174) without a GPU. The render-graph cases build a bare graph and go
// through the same planner, frame walk and submission order that RenderingDeviceGraph::end() and
// RenderingDevice::execute_chained_cmds() use. The RenderingDevice cases use a driverless device.
// godotnext/testbed/tests/async_compute checks the same fork and join on Metal.

#ifdef RD_ENABLED

#include "core/error/error_macros.h"
#include "servers/rendering/renderer_rd/effects/tsr_async.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_graph.h"

using RDG = RenderingDeviceGraph;

namespace TestRDAsyncCompute {

RDD::BufferID buffer_id(uint64_t p_id) {
	return RDD::BufferID(p_id);
}

bool same_order(const LocalVector<int32_t> &p_a, const LocalVector<int32_t> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (uint32_t i = 0; i < p_a.size(); i++) {
		if (p_a[i] != p_b[i]) {
			return false;
		}
	}
	return true;
}

bool same_commands(const LocalVector<int32_t> &p_commands, std::initializer_list<int32_t> p_expected) {
	if (p_commands.size() != p_expected.size()) {
		return false;
	}
	uint32_t i = 0;
	for (int32_t expected : p_expected) {
		if (p_commands[i++] != expected) {
			return false;
		}
	}
	return true;
}

RDG::ResourceTracker *tracker() {
	return RDG::resource_tracker_create();
}

void free_trackers(RDG::ResourceTracker **p_trackers, uint32_t p_count) {
	for (uint32_t i = 0; i < p_count; i++) {
		RDG::resource_tracker_free(p_trackers[i]);
	}
}

void add_compute(RDG &p_graph, RDG::Queue p_queue, RDG::ResourceTracker *p_read, RDG::ResourceTracker *p_write) {
	p_graph.add_compute_list_begin(RDD::BreadcrumbMarker::NONE, 0, p_queue);
	if (p_read != nullptr) {
		p_graph.add_compute_list_usage(p_read, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
	}
	if (p_write != nullptr) {
		p_graph.add_compute_list_usage(p_write, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE);
	}
	p_graph.add_compute_list_dispatch(1, 1, 1);
	p_graph.add_compute_list_end();
}

void add_async_compute(RDG &p_graph, RDG::ResourceTracker *p_read, RDG::ResourceTracker *p_write) {
	add_compute(p_graph, RDG::QUEUE_ASYNC_COMPUTE, p_read, p_write);
}

void add_main_compute(RDG &p_graph, RDG::ResourceTracker *p_read = nullptr, RDG::ResourceTracker *p_write = nullptr) {
	add_compute(p_graph, RDG::QUEUE_MAIN, p_read, p_write);
}

// Records one frame of RDG::submit_chained_command_buffers() with made-up driver IDs.
struct Submit {
	uint64_t queue = 0;
	LocalVector<uint64_t> waits;
	uint64_t command_buffer = 0;
	LocalVector<uint64_t> signals;
	uint64_t fence = 0;
	uint32_t swap_chains = 0;
};

enum : uint64_t {
	MAIN_QUEUE = 1,
	ASYNC_QUEUE = 2,
	FRAME_COMMAND_BUFFER = 100, // Pool buffer k is 100 + k.
	POOL_SEMAPHORE = 200, // Pool semaphore i is 200 + i.
	FORK_SEMAPHORE = 301,
	JOIN_SEMAPHORE = 302,
	ASYNC_COMMAND_BUFFER = 400,
	FRAME_WAIT_SEMAPHORE = 500,
	LAST_SEMAPHORE = 600,
	LAST_FENCE = 700,
	SWAP_CHAIN = 800,
};

void record_submit(void *p_userdata, RDD::CommandQueueID p_queue, VectorView<RDD::SemaphoreID> p_wait_semaphores, RDD::CommandBufferID p_command_buffer, VectorView<RDD::SemaphoreID> p_signal_semaphores, RDD::FenceID p_signal_fence, VectorView<RDD::SwapChainID> p_swap_chains) {
	Submit submit;
	submit.queue = p_queue.id;
	for (uint32_t i = 0; i < p_wait_semaphores.size(); i++) {
		submit.waits.push_back(p_wait_semaphores[i].id);
	}
	submit.command_buffer = p_command_buffer.id;
	for (uint32_t i = 0; i < p_signal_semaphores.size(); i++) {
		submit.signals.push_back(p_signal_semaphores[i].id);
	}
	submit.fence = p_signal_fence.id;
	submit.swap_chains = p_swap_chains.size();
	static_cast<LocalVector<Submit> *>(p_userdata)->push_back(submit);
}

// Submits a frame of p_command_buffer_count main command buffers, as execute_chained_cmds() does.
LocalVector<Submit> submit_frame(uint32_t p_command_buffer_count, int32_t p_fork_index, int32_t p_join_index, bool p_last_semaphore = true) {
	RDG::CommandBufferPool pool;
	// One more pool buffer than the frame uses; it must not be submitted.
	for (uint32_t i = 0; i < p_command_buffer_count; i++) {
		pool.buffers.push_back(RDD::CommandBufferID(FRAME_COMMAND_BUFFER + 1 + i));
		pool.semaphores.push_back(RDD::SemaphoreID(POOL_SEMAPHORE + i));
	}
	const RDD::SemaphoreID frame_waits[2] = { RDD::SemaphoreID(FRAME_WAIT_SEMAPHORE), RDD::SemaphoreID(FRAME_WAIT_SEMAPHORE + 1) };
	const RDD::SwapChainID swap_chain(SWAP_CHAIN);

	RDG::ChainedSubmission submission;
	submission.main_queue = RDD::CommandQueueID(MAIN_QUEUE);
	submission.async_queue = RDD::CommandQueueID(ASYNC_QUEUE);
	submission.frame_command_buffer = RDD::CommandBufferID(FRAME_COMMAND_BUFFER);
	submission.pool = &pool;
	submission.command_buffer_count = p_command_buffer_count;
	submission.frame_wait_semaphores = VectorView<RDD::SemaphoreID>(frame_waits, 2);
	submission.last_signal_semaphore = p_last_semaphore ? RDD::SemaphoreID(LAST_SEMAPHORE) : RDD::SemaphoreID();
	submission.last_signal_fence = RDD::FenceID(LAST_FENCE);
	submission.last_swap_chains = VectorView<RDD::SwapChainID>(swap_chain);
	submission.async_fork_index = p_fork_index;
	submission.async_join_index = p_join_index;
	submission.async_command_buffer = RDD::CommandBufferID(ASYNC_COMMAND_BUFFER);
	submission.async_fork_semaphore = RDD::SemaphoreID(FORK_SEMAPHORE);
	submission.async_join_semaphore = RDD::SemaphoreID(JOIN_SEMAPHORE);

	LocalVector<Submit> submits;
	RDG::submit_chained_command_buffers(submission, &record_submit, &submits);
	return submits;
}

uint32_t count_waits(const LocalVector<Submit> &p_submits, uint64_t p_semaphore) {
	uint32_t count = 0;
	for (const Submit &submit : p_submits) {
		for (uint64_t wait : submit.waits) {
			count += wait == p_semaphore ? 1 : 0;
		}
	}
	return count;
}

uint32_t count_signals(const LocalVector<Submit> &p_submits, uint64_t p_semaphore) {
	uint32_t count = 0;
	for (const Submit &submit : p_submits) {
		for (uint64_t signal : submit.signals) {
			count += signal == p_semaphore ? 1 : 0;
		}
	}
	return count;
}

int32_t find_submit(const LocalVector<Submit> &p_submits, uint64_t p_command_buffer) {
	for (uint32_t i = 0; i < p_submits.size(); i++) {
		if (p_submits[i].command_buffer == p_command_buffer) {
			return int32_t(i);
		}
	}
	return -1;
}

uint64_t main_command_buffer(int32_t p_index) {
	return FRAME_COMMAND_BUFFER + uint64_t(p_index);
}

// The cross-queue contract of one frame: the fork command buffer signals the async work once, the
// async work waits once and signals once, and the join command buffer waits for it once, after the
// async work was submitted. The frame fence and the swap chains stay on the last main command buffer.
void check_fork_join_submits(const LocalVector<Submit> &p_submits, uint32_t p_command_buffer_count, int32_t p_fork_index, int32_t p_join_index) {
	REQUIRE_EQ(p_submits.size(), p_command_buffer_count + 1);

	uint32_t async_submits = 0;
	for (const Submit &submit : p_submits) {
		async_submits += submit.queue == ASYNC_QUEUE ? 1 : 0;
	}
	CHECK_EQ(async_submits, 1u);

	CHECK_EQ(count_signals(p_submits, FORK_SEMAPHORE), 1u);
	CHECK_EQ(count_waits(p_submits, FORK_SEMAPHORE), 1u);
	CHECK_EQ(count_signals(p_submits, JOIN_SEMAPHORE), 1u);
	CHECK_EQ(count_waits(p_submits, JOIN_SEMAPHORE), 1u);

	const int32_t fork_submit = find_submit(p_submits, main_command_buffer(p_fork_index));
	const int32_t async_submit = find_submit(p_submits, ASYNC_COMMAND_BUFFER);
	const int32_t join_submit = find_submit(p_submits, main_command_buffer(p_join_index));
	REQUIRE(fork_submit >= 0);
	REQUIRE(async_submit >= 0);
	REQUIRE(join_submit >= 0);
	CHECK_EQ(async_submit, fork_submit + 1);
	CHECK(async_submit < join_submit);

	const Submit &fork = p_submits[fork_submit];
	CHECK_EQ(fork.queue, uint64_t(MAIN_QUEUE));
	CHECK(fork.signals.has(FORK_SEMAPHORE));
	const Submit &async = p_submits[async_submit];
	CHECK_EQ(async.queue, uint64_t(ASYNC_QUEUE));
	REQUIRE_EQ(async.waits.size(), 1u);
	CHECK_EQ(async.waits[0], uint64_t(FORK_SEMAPHORE));
	REQUIRE_EQ(async.signals.size(), 1u);
	CHECK_EQ(async.signals[0], uint64_t(JOIN_SEMAPHORE));
	CHECK_EQ(async.fence, 0u);
	CHECK_EQ(async.swap_chains, 0u);
	const Submit &join = p_submits[join_submit];
	CHECK_EQ(join.queue, uint64_t(MAIN_QUEUE));
	CHECK(join.waits.has(JOIN_SEMAPHORE));

	// The main command buffers still run in order, each waiting on the one before it.
	uint32_t main_index = 0;
	for (const Submit &submit : p_submits) {
		if (submit.queue != MAIN_QUEUE) {
			continue;
		}
		CHECK_EQ(submit.command_buffer, main_command_buffer(int32_t(main_index)));
		if (main_index > 0) {
			CHECK(submit.waits.has(POOL_SEMAPHORE + main_index - 1));
		}
		const bool last = main_index == p_command_buffer_count - 1;
		CHECK_EQ(submit.fence, last ? uint64_t(LAST_FENCE) : 0u);
		CHECK_EQ(submit.swap_chains, last ? 1u : 0u);
		main_index++;
	}
	CHECK_EQ(main_index, p_command_buffer_count);
}

const RDG::AsyncEmission::Step *find_step(const RDG::AsyncEmission &p_emission, RDG::AsyncEmission::StepType p_type, uint32_t p_level) {
	for (const RDG::AsyncEmission::Step &step : p_emission.steps) {
		if (step.type == p_type && step.level == p_level) {
			return &step;
		}
	}
	return nullptr;
}

// What makes the fork and the join safe: the main commands before fork_level are recorded into the
// fork command buffer or earlier ones, the main commands from join_level on into the join command
// buffer or later ones, and the main commands in between into neither.
void check_emission_order(const RDG::AsyncEmission &p_emission, const RDG::AsyncPlan &p_plan) {
	REQUIRE(p_emission.fork_index >= 0);
	CHECK(p_emission.fork_index < p_emission.join_index);
	CHECK(p_emission.join_index <= int32_t(p_emission.buffers_used));
	for (const RDG::AsyncEmission::Step &step : p_emission.steps) {
		if (step.type != RDG::AsyncEmission::STEP_MAIN) {
			continue;
		}
		CAPTURE(step.level);
		if (step.level < p_plan.fork_level) {
			CHECK(int32_t(step.buffer) <= p_emission.fork_index);
		} else {
			CHECK(int32_t(step.buffer) > p_emission.fork_index);
		}
		if (step.level < p_plan.join_level) {
			CHECK(int32_t(step.buffer) < p_emission.join_index);
		} else {
			CHECK(int32_t(step.buffer) >= p_emission.join_index);
		}
	}
}

uint32_t count_steps(const RDG::AsyncEmission &p_emission, RDG::AsyncEmission::StepType p_type) {
	uint32_t count = 0;
	for (const RDG::AsyncEmission::Step &step : p_emission.steps) {
		count += step.type == p_type ? 1 : 0;
	}
	return count;
}

// A copy, so the doctest macros do not bind a reference to the in-class constant.
const RenderingDevice::ComputeListID INVALID_LIST = RenderingDevice::INVALID_ID;

struct ErrorCapture {
	ErrorHandlerList handler;
	int count = 0;
	String last_message;

	ErrorCapture() {
		handler.errfunc = _capture;
		handler.userdata = this;
		add_error_handler(&handler);
	}

	~ErrorCapture() {
		remove_error_handler(&handler);
	}

	static void _capture(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
		ErrorCapture *self = static_cast<ErrorCapture *>(p_self);
		self->count++;
		self->last_message = String::utf8((p_message != nullptr && *p_message != 0) ? p_message : p_error);
	}
};

// GodotNext: rdg_access_stages (#267). A texture tracker with p_mipmaps mips and one layer, and a
// one-mip slice of it, set up the way RenderingDevice::_texture_make_mutable() does.
RDG::ResourceTracker *texture_tracker(uint64_t p_id, uint32_t p_mipmaps) {
	RDG::ResourceTracker *texture = RDG::resource_tracker_create();
	texture->texture_driver_id = RDD::TextureID(p_id);
	texture->texture_subresources.aspect = RDD::TEXTURE_ASPECT_COLOR_BIT;
	texture->texture_subresources.mipmap_count = p_mipmaps;
	texture->texture_subresources.layer_count = 1;
	return texture;
}

RDG::ResourceTracker *mip_slice_tracker(RDG::ResourceTracker *p_texture, uint64_t p_id, uint32_t p_mipmap) {
	RDG::ResourceTracker *slice = texture_tracker(p_id, 1);
	slice->parent = p_texture;
	slice->texture_subresources.base_mipmap = p_mipmap;
	slice->texture_slice_or_dirty_rect = Rect2i(p_mipmap, 0, 1, 1);
	return slice;
}

// A compute list (compute stage) that samples p_read and writes p_write as a storage image.
void add_texture_compute(RDG &p_graph, RDG::ResourceTracker *p_read, RDG::ResourceTracker *p_write, RDG::Queue p_queue = RDG::QUEUE_MAIN) {
	p_graph.add_compute_list_begin(RDD::BreadcrumbMarker::NONE, 0, p_queue);
	p_graph.add_compute_list_bind_pipeline(RDD::PipelineID());
	if (p_read != nullptr) {
		p_graph.add_compute_list_usage(p_read, RDG::RESOURCE_USAGE_TEXTURE_SAMPLE);
	}
	if (p_write != nullptr) {
		p_graph.add_compute_list_usage(p_write, RDG::RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE);
	}
	p_graph.add_compute_list_dispatch(1, 1, 1);
	p_graph.add_compute_list_end();
}

BitField<RDD::PipelineStageBits> previous_stages(const RDG &p_graph, int32_t p_command_index) {
	BitField<RDD::PipelineStageBits> self_stages;
	BitField<RDD::PipelineStageBits> previous;
	BitField<RDD::PipelineStageBits> next_stages;
	p_graph.command_get_stages(p_command_index, self_stages, previous, next_stages);
	return previous;
}

// An async compute list that samples every texture of p_sampled and writes p_written as a storage
// image.
void add_async_sampling_compute(RDG &p_graph, std::initializer_list<RDG::ResourceTracker *> p_sampled, RDG::ResourceTracker *p_written) {
	p_graph.add_compute_list_begin(RDD::BreadcrumbMarker::NONE, 0, RDG::QUEUE_ASYNC_COMPUTE);
	p_graph.add_compute_list_bind_pipeline(RDD::PipelineID());
	for (RDG::ResourceTracker *sampled : p_sampled) {
		p_graph.add_compute_list_usage(sampled, RDG::RESOURCE_USAGE_TEXTURE_SAMPLE);
	}
	p_graph.add_compute_list_usage(p_written, RDG::RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE);
	p_graph.add_compute_list_dispatch(1, 1, 1);
	p_graph.add_compute_list_end();
}

uint32_t count_texture_barriers(const LocalVector<RDD::TextureBarrier> &p_barriers, uint64_t p_texture, RDD::TextureLayout p_prev_layout, RDD::TextureLayout p_next_layout) {
	uint32_t count = 0;
	for (const RDD::TextureBarrier &barrier : p_barriers) {
		count += (barrier.texture.id == p_texture && barrier.prev_layout == p_prev_layout && barrier.next_layout == p_next_layout) ? 1 : 0;
	}
	return count;
}

const RDD::TextureBarrier *find_texture_barrier(const LocalVector<RDD::TextureBarrier> &p_barriers, uint64_t p_texture) {
	for (const RDD::TextureBarrier &barrier : p_barriers) {
		if (barrier.texture.id == p_texture) {
			return &barrier;
		}
	}
	return nullptr;
}

// The raster pass of godotnext/testbed/tests/rd_parity/async_overlap.gd: a draw list that clears
// and blends into one color attachment and touches nothing else.
void add_color_pass(RDG &p_graph, RDG::ResourceTracker *p_color) {
	const RDG::AttachmentOperation operation = RDG::ATTACHMENT_OPERATION_CLEAR;
	const RDD::RenderPassClearValue clear_value;
	p_graph.add_draw_list_begin(RDD::RenderPassID(), RDD::FramebufferID(), Rect2i(0, 0, 64, 64), VectorView<RDG::AttachmentOperation>(&operation, 1), VectorView<RDD::RenderPassClearValue>(&clear_value, 1), RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	p_graph.add_draw_list_usage(p_color, RDG::RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE);
	p_graph.add_draw_list_draw(3, 16);
	p_graph.add_draw_list_end();
}

// GodotNext: pt_tsr_async_compute (#193). A depth texture tracker, and a draw list that writes
// color attachments and uses a depth attachment, as every RenderForwardClustered pass does: the
// render graph always records a depth attachment as read-write.
RDG::ResourceTracker *depth_tracker(uint64_t p_id) {
	RDG::ResourceTracker *depth = texture_tracker(p_id, 1);
	depth->texture_subresources.aspect = RDD::TEXTURE_ASPECT_DEPTH_BIT;
	return depth;
}

void add_attachment_pass(RDG &p_graph, std::initializer_list<RDG::ResourceTracker *> p_colors, RDG::ResourceTracker *p_depth, RDG::ResourceTracker *p_sampled = nullptr) {
	const uint32_t count = uint32_t(p_colors.size()) + (p_depth != nullptr ? 1 : 0);
	LocalVector<RDG::AttachmentOperation> operations;
	LocalVector<RDD::RenderPassClearValue> clear_values;
	operations.resize(count);
	clear_values.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		operations[i] = RDG::ATTACHMENT_OPERATION_DEFAULT;
	}
	BitField<RDD::PipelineStageBits> stages = RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	stages.set_flag(RDD::PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
	stages.set_flag(RDD::PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
	p_graph.add_draw_list_begin(RDD::RenderPassID(), RDD::FramebufferID(), Rect2i(0, 0, 64, 64), VectorView<RDG::AttachmentOperation>(operations.ptr(), count), VectorView<RDD::RenderPassClearValue>(clear_values.ptr(), count), stages);
	for (RDG::ResourceTracker *color : p_colors) {
		p_graph.add_draw_list_usage(color, RDG::RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE);
	}
	if (p_depth != nullptr) {
		p_graph.add_draw_list_usage(p_depth, RDG::RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ_WRITE);
	}
	if (p_sampled != nullptr) {
		p_graph.add_draw_list_usage(p_sampled, RDG::RESOURCE_USAGE_TEXTURE_SAMPLE);
	}
	p_graph.add_draw_list_draw(3, 1);
	p_graph.add_draw_list_end();
}

// A compute list that samples every texture of p_sampled and writes every one of p_written as a
// storage image.
void add_image_compute(RDG &p_graph, RDG::Queue p_queue, std::initializer_list<RDG::ResourceTracker *> p_sampled, std::initializer_list<RDG::ResourceTracker *> p_written) {
	p_graph.add_compute_list_begin(RDD::BreadcrumbMarker::NONE, 0, p_queue);
	p_graph.add_compute_list_bind_pipeline(RDD::PipelineID());
	for (RDG::ResourceTracker *sampled : p_sampled) {
		p_graph.add_compute_list_usage(sampled, RDG::RESOURCE_USAGE_TEXTURE_SAMPLE);
	}
	for (RDG::ResourceTracker *written : p_written) {
		p_graph.add_compute_list_usage(written, RDG::RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE);
	}
	p_graph.add_compute_list_dispatch(1, 1, 1);
	p_graph.add_compute_list_end();
}

// The textures of a Forward+ frame with TSR, by id. p_base offsets the ids, so that two frames
// (two viewports) can share one graph.
struct TSRFrame {
	enum : uint64_t {
		DEPTH = 1,
		NORMAL = 2,
		COLOR = 3,
		VELOCITY = 4,
		DILATED = 5,
		PREV_DEPTH = 6,
		DISOCCLUSION = 7,
		REJECTION = 8,
		HISTORY = 9,
		OUTPUT = 10,
		FINAL = 11,
		COUNT = 11,
	};
	RDG::ResourceTracker *trackers[COUNT] = {};

	explicit TSRFrame(uint64_t p_base = 0) {
		for (uint64_t id = 1; id <= COUNT; id++) {
			trackers[id - 1] = id == DEPTH ? depth_tracker(p_base + id) : texture_tracker(p_base + id, 1);
		}
	}
	~TSRFrame() { free_trackers(trackers, COUNT); }
	RDG::ResourceTracker *operator[](uint64_t p_id) const { return trackers[p_id - 1]; }
};

// Command indices of record_tsr_frame() when the depth and velocity passes record after the
// motion vectors.
enum : int32_t {
	TSR_CMD_PREPASS = 0,
	TSR_CMD_OPAQUE = 1,
	TSR_CMD_MOTION_STORE = 2,
	TSR_CMD_MOTION_PASS = 3,
	TSR_CMD_SPLIT_DILATION = 4,
	TSR_CMD_SPLIT_DISOCCLUSION = 5,
	TSR_CMD_SPLIT_SKY = 6,
};

// The five core passes, as TSR::process() records them: only at a record point whose call runs,
// each pass where and on the queue nextgi_tsr_point_passes() names.
void record_tsr_passes(RDG &p_graph, const TSRAsyncSchedule &p_schedule, TSRRecordPoint p_point, const TSRFrame &p_frame) {
	if (!nextgi_tsr_point_role(p_schedule, p_point).runs()) {
		return;
	}
	const TSRPointPasses passes = nextgi_tsr_point_passes(p_schedule, p_point, nextgi_tsr_frame_passes(false, false, false, false, false));
	auto record = [&](TSRPass p_pass, std::initializer_list<RDG::ResourceTracker *> p_sampled, std::initializer_list<RDG::ResourceTracker *> p_written) {
		if (passes.records(p_pass)) {
			add_image_compute(p_graph, passes.runs_async(p_pass) ? RDG::QUEUE_ASYNC_COMPUTE : RDG::QUEUE_MAIN, p_sampled, p_written);
		}
	};
	record(TSR_PASS_VELOCITY_DILATION, { p_frame[TSRFrame::DEPTH], p_frame[TSRFrame::VELOCITY] }, { p_frame[TSRFrame::DILATED] });
	record(TSR_PASS_DISOCCLUSION, { p_frame[TSRFrame::DEPTH], p_frame[TSRFrame::DILATED], p_frame[TSRFrame::PREV_DEPTH] }, { p_frame[TSRFrame::DISOCCLUSION] });
	record(TSR_PASS_SHADING_REJECTION, { p_frame[TSRFrame::COLOR], p_frame[TSRFrame::DEPTH], p_frame[TSRFrame::DILATED], p_frame[TSRFrame::DISOCCLUSION] }, { p_frame[TSRFrame::REJECTION], p_frame[TSRFrame::PREV_DEPTH] });
	record(TSR_PASS_HISTORY_UPDATE, { p_frame[TSRFrame::COLOR], p_frame[TSRFrame::DILATED], p_frame[TSRFrame::REJECTION] }, { p_frame[TSRFrame::HISTORY] });
	record(TSR_PASS_RESOLVE, { p_frame[TSRFrame::HISTORY] }, { p_frame[TSRFrame::OUTPUT] });
}

// One Forward+ frame with TSR, as RenderForwardClustered records it with p_schedule in a frame the
// split allows: the depth prepass, the opaque pass (which samples p_opaque_samples, as a material
// with another viewport's texture does), the camera motion vectors and the motion pass; the record
// point after the motion vectors; the sky and the transparent pass, each with the depth attachment;
// the upscaler's record point; and the tonemapper, which samples TSR's output. Nothing records on
// the main queue between the record point and the sky: the split guard rules out post-opaque
// compositor effects and VoxelGI/SDFGI debug draws, TSR rules out MSAA, and SSAO, SSIL, SSR and
// the volumetric fog run before the opaque pass.
void record_tsr_frame(RDG &p_graph, const TSRAsyncSchedule &p_schedule, const TSRFrame &p_frame, RDG::ResourceTracker *p_opaque_samples = nullptr) {
	add_attachment_pass(p_graph, { p_frame[TSRFrame::NORMAL] }, p_frame[TSRFrame::DEPTH]);
	add_attachment_pass(p_graph, { p_frame[TSRFrame::COLOR] }, p_frame[TSRFrame::DEPTH], p_opaque_samples);
	add_image_compute(p_graph, RDG::QUEUE_MAIN, { p_frame[TSRFrame::DEPTH] }, { p_frame[TSRFrame::VELOCITY] });
	add_attachment_pass(p_graph, { p_frame[TSRFrame::COLOR], p_frame[TSRFrame::VELOCITY] }, p_frame[TSRFrame::DEPTH]);
	record_tsr_passes(p_graph, p_schedule, TSR_RECORD_AFTER_MOTION_VECTORS, p_frame);
	add_attachment_pass(p_graph, { p_frame[TSRFrame::COLOR] }, p_frame[TSRFrame::DEPTH]);
	add_attachment_pass(p_graph, { p_frame[TSRFrame::COLOR] }, p_frame[TSRFrame::DEPTH]);
	record_tsr_passes(p_graph, p_schedule, TSR_RECORD_UPSCALE, p_frame);
	p_graph.add_compute_list_begin();
	p_graph.add_compute_list_bind_pipeline(RDD::PipelineID());
	p_graph.add_compute_list_usage(p_frame[TSRFrame::OUTPUT], RDG::RESOURCE_USAGE_TEXTURE_SAMPLE);
	p_graph.add_compute_list_usage(p_frame[TSRFrame::FINAL], RDG::RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE);
	p_graph.add_compute_list_dispatch(1, 1, 1);
	p_graph.add_compute_list_end();
}

// The emission step (main or async) that records p_command.
const RDG::AsyncEmission::Step *find_command_step(const RDG::AsyncEmission &p_emission, int32_t p_command) {
	for (const RDG::AsyncEmission::Step &step : p_emission.steps) {
		for (int32_t command : step.commands) {
			if (command == p_command) {
				return &step;
			}
		}
	}
	return nullptr;
}

} // namespace TestRDAsyncCompute

using namespace TestRDAsyncCompute;

TEST_CASE("[RD][AsyncCompute] one cross-queue window covers every edge") {
	RDG graph;
	RDG::ResourceTracker *source = tracker();
	RDG::ResourceTracker *left = tracker();
	RDG::ResourceTracker *right = tracker();
	RDG::ResourceTracker *trackers[] = { source, left, right };

	// One main write feeds two async lists. Each async list feeds its own main read.
	// Four cross-queue edges, one fork and one join.
	graph.add_buffer_update(buffer_id(1), source, VectorView<RDG::RecordedBufferCopy>());
	add_async_compute(graph, source, left);
	add_async_compute(graph, source, right);
	RDD::BufferCopyRegion region;
	graph.add_buffer_get_data(buffer_id(2), left, buffer_id(4), region);
	graph.add_buffer_get_data(buffer_id(3), right, buffer_id(5), region);

	LocalVector<int32_t> order;
	RDG::AsyncPlan plan;
	graph.plan_for_test(order, plan, true);

	CHECK(plan.active);
	CHECK_EQ(plan.async_count, 2u);
	CHECK_EQ(plan.main_count, 3u);
	CHECK_EQ(plan.fork_level, 1u);
	CHECK_EQ(plan.join_level, 2u);
	CHECK(plan.fork_level < plan.join_level);
	const int32_t expected[] = { 0, 1, 2, 3, 4 };
	CHECK_EQ(order.size(), 5u);
	for (uint32_t i = 0; i < order.size(); i++) {
		CHECK_EQ(order[i], expected[i]);
	}

	// end() records it as one fork and one join, and the frame waits once for the async work.
	RDG::AsyncPlan emission_plan;
	RDG::AsyncEmission emission;
	graph.emit_for_test(emission_plan, emission);
	REQUIRE(emission_plan.active);
	CHECK_EQ(count_steps(emission, RDG::AsyncEmission::STEP_SPLIT), 2u);
	CHECK_EQ(emission.fork_index, 0);
	CHECK_EQ(emission.join_index, 2);
	CHECK_EQ(emission.buffers_used, 2u);
	check_emission_order(emission, emission_plan);
	check_fork_join_submits(submit_frame(emission.buffers_used + 1, emission.fork_index, emission.join_index), emission.buffers_used + 1, emission.fork_index, emission.join_index);

	free_trackers(trackers, 3);
}

TEST_CASE("[RD][AsyncCompute] no async queue keeps the main-queue order") {
	RDG async_graph;
	RDG::ResourceTracker *cleared = tracker();
	async_graph.add_buffer_clear(buffer_id(1), cleared, 0, 16);
	add_async_compute(async_graph, nullptr, nullptr);
	async_graph.add_driver_callback(nullptr, nullptr, VectorView<RDG::ResourceTracker *>(), VectorView<RDG::ResourceUsage>(), BitField<RDD::PipelineStageBits>());

	RDG main_graph;
	RDG::ResourceTracker *cleared_main = tracker();
	main_graph.add_buffer_clear(buffer_id(1), cleared_main, 0, 16);
	add_main_compute(main_graph);
	main_graph.add_driver_callback(nullptr, nullptr, VectorView<RDG::ResourceTracker *>(), VectorView<RDG::ResourceUsage>(), BitField<RDD::PipelineStageBits>());

	LocalVector<int32_t> with_queue;
	LocalVector<int32_t> without_queue;
	LocalVector<int32_t> main_only;
	RDG::AsyncPlan with_plan;
	RDG::AsyncPlan without_plan;
	RDG::AsyncPlan main_plan;
	async_graph.plan_for_test(with_queue, with_plan, true);
	async_graph.plan_for_test(without_queue, without_plan, false);
	main_graph.plan_for_test(main_only, main_plan, true);

	CHECK(with_plan.active);
	CHECK_FALSE(without_plan.active);
	CHECK_FALSE(main_plan.active);
	CHECK_FALSE(same_order(with_queue, main_only));
	CHECK(same_order(without_queue, main_only));
	const int32_t main_expected[] = { 0, 1, 2 };
	CHECK_EQ(main_only.size(), 3u);
	for (uint32_t i = 0; i < main_only.size(); i++) {
		CHECK_EQ(main_only[i], main_expected[i]);
	}

	// A frame without a fork submits the plain chain, unchanged by async compute.
	const LocalVector<Submit> submits = submit_frame(3, -1, -1);
	REQUIRE_EQ(submits.size(), 3u);
	for (uint32_t i = 0; i < submits.size(); i++) {
		const Submit &submit = submits[i];
		CHECK_EQ(submit.queue, uint64_t(MAIN_QUEUE));
		CHECK_EQ(submit.command_buffer, main_command_buffer(int32_t(i)));
		if (i == 0) {
			REQUIRE_EQ(submit.waits.size(), 2u);
			CHECK_EQ(submit.waits[0], uint64_t(FRAME_WAIT_SEMAPHORE));
			CHECK_EQ(submit.waits[1], uint64_t(FRAME_WAIT_SEMAPHORE + 1));
		} else {
			REQUIRE_EQ(submit.waits.size(), 1u);
			CHECK_EQ(submit.waits[0], uint64_t(POOL_SEMAPHORE + i - 1));
		}
		REQUIRE_EQ(submit.signals.size(), 1u);
		CHECK_EQ(submit.signals[0], i == 2 ? uint64_t(LAST_SEMAPHORE) : uint64_t(POOL_SEMAPHORE + i));
		CHECK_EQ(submit.fence, i == 2 ? uint64_t(LAST_FENCE) : 0u);
		CHECK_EQ(submit.swap_chains, i == 2 ? 1u : 0u);
	}
	CHECK_EQ(count_waits(submits, JOIN_SEMAPHORE), 0u);
	CHECK_EQ(count_signals(submits, FORK_SEMAPHORE), 0u);

	// Without a semaphore to signal, the last command buffer signals nothing but the fence.
	const LocalVector<Submit> unsignaled = submit_frame(1, -1, -1, false);
	REQUIRE_EQ(unsignaled.size(), 1u);
	CHECK(unsignaled[0].signals.is_empty());
	CHECK_EQ(unsignaled[0].fence, uint64_t(LAST_FENCE));

	RDG::ResourceTracker *trackers[] = { cleared, cleared_main };
	free_trackers(trackers, 2);
}

TEST_CASE("[RD][AsyncCompute] overlapping producer and consumer stays on the main queue") {
	// main -> async -> main -> async -> main falls back for the frame. The side lists are
	// independent main compute lists on the async lists' levels: they sort after the async lists by
	// recording index in the main-queue order, and would sort before them if the fallback left the
	// queue keys in place.
	RDG graph;
	RDG::ResourceTracker *source = tracker();
	RDG::ResourceTracker *mid = tracker();
	RDG::ResourceTracker *destination = tracker();
	RDG::ResourceTracker *output = tracker();
	RDG::ResourceTracker *side = tracker();
	RDG::ResourceTracker *side_2 = tracker();
	RDG::ResourceTracker *trackers[] = { source, mid, destination, output, side, side_2 };

	RDD::BufferCopyRegion region;
	graph.add_buffer_update(buffer_id(1), source, VectorView<RDG::RecordedBufferCopy>());
	add_async_compute(graph, source, mid);
	graph.add_buffer_copy(buffer_id(2), mid, buffer_id(3), destination, region);
	add_async_compute(graph, destination, output);
	graph.add_buffer_get_data(buffer_id(4), output, buffer_id(5), region);
	add_main_compute(graph, source, side);
	add_main_compute(graph, destination, side_2);

	RDG main_graph;
	RDG::ResourceTracker *source_main = tracker();
	RDG::ResourceTracker *mid_main = tracker();
	RDG::ResourceTracker *destination_main = tracker();
	RDG::ResourceTracker *output_main = tracker();
	RDG::ResourceTracker *side_main = tracker();
	RDG::ResourceTracker *side_2_main = tracker();
	RDG::ResourceTracker *main_trackers[] = { source_main, mid_main, destination_main, output_main, side_main, side_2_main };
	main_graph.add_buffer_update(buffer_id(1), source_main, VectorView<RDG::RecordedBufferCopy>());
	add_main_compute(main_graph, source_main, mid_main);
	main_graph.add_buffer_copy(buffer_id(2), mid_main, buffer_id(3), destination_main, region);
	add_main_compute(main_graph, destination_main, output_main);
	main_graph.add_buffer_get_data(buffer_id(4), output_main, buffer_id(5), region);
	add_main_compute(main_graph, source_main, side_main);
	add_main_compute(main_graph, destination_main, side_2_main);

	LocalVector<int32_t> order;
	LocalVector<int32_t> main_order;
	RDG::AsyncPlan plan;
	RDG::AsyncPlan main_plan;
	graph.plan_for_test(order, plan, true);
	main_graph.plan_for_test(main_order, main_plan, true);

	CHECK_FALSE(plan.active);
	CHECK_EQ(plan.async_count, 2u);
	CHECK(plan.fork_level >= plan.join_level);
	CHECK(same_order(order, main_order));
	// Levels 0..4: the update; async 1 and side 5; the copy; async 3 and side 6; the readback.
	const int32_t expected[] = { 0, 1, 5, 2, 3, 6, 4 };
	CHECK_EQ(order.size(), 7u);
	for (uint32_t i = 0; i < order.size(); i++) {
		CHECK_EQ(order[i], expected[i]);
	}

	// A frame that falls back records no fork and no join.
	RDG::AsyncPlan emission_plan;
	RDG::AsyncEmission emission;
	graph.emit_for_test(emission_plan, emission);
	CHECK_FALSE(emission_plan.active);
	CHECK(emission.steps.is_empty());
	CHECK_EQ(emission.fork_index, -1);
	CHECK_EQ(emission.join_index, -1);

	free_trackers(trackers, 6);
	free_trackers(main_trackers, 6);
}

TEST_CASE("[RD][AsyncCompute] end() splits the main command buffer once for the fork and once for the join") {
	// The update feeds the async list; the side list runs on the main queue next to it; the
	// readback consumes the async result.
	RDG graph;
	RDG::ResourceTracker *source = tracker();
	RDG::ResourceTracker *result = tracker();
	RDG::ResourceTracker *side = tracker();
	RDG::ResourceTracker *trackers[] = { source, result, side };

	RDD::BufferCopyRegion region;
	graph.add_buffer_update(buffer_id(1), source, VectorView<RDG::RecordedBufferCopy>());
	add_async_compute(graph, source, result);
	add_main_compute(graph, source, side);
	graph.add_buffer_get_data(buffer_id(2), result, buffer_id(3), region);

	RDG::AsyncPlan plan;
	RDG::AsyncEmission emission;
	graph.emit_for_test(plan, emission);
	REQUIRE(plan.active);
	CHECK_EQ(plan.fork_level, 1u);
	CHECK_EQ(plan.join_level, 2u);

	// Main buffer 0 holds the producer and signals the async list (fork). Main buffer 1 runs the
	// side list while the async list runs. Main buffer 2 waits for the async list (join) and holds
	// the consumer.
	REQUIRE_EQ(emission.steps.size(), 6u);
	const RDG::AsyncEmission::Step &producer = emission.steps[0];
	CHECK_EQ(producer.type, RDG::AsyncEmission::STEP_MAIN);
	CHECK_EQ(producer.level, 0u);
	CHECK_EQ(producer.buffer, 0u);
	CHECK(same_commands(producer.commands, { 0 }));
	CHECK_EQ(emission.steps[1].type, RDG::AsyncEmission::STEP_SPLIT);
	CHECK_EQ(emission.steps[1].buffer, 1u);
	const RDG::AsyncEmission::Step &overlap = emission.steps[2];
	CHECK_EQ(overlap.type, RDG::AsyncEmission::STEP_MAIN);
	CHECK_EQ(overlap.level, 1u);
	CHECK_EQ(overlap.buffer, 1u);
	CHECK(same_commands(overlap.commands, { 2 }));
	const RDG::AsyncEmission::Step &async = emission.steps[3];
	CHECK_EQ(async.type, RDG::AsyncEmission::STEP_ASYNC);
	CHECK_EQ(async.level, 1u);
	CHECK(same_commands(async.commands, { 1 }));
	CHECK_EQ(emission.steps[4].type, RDG::AsyncEmission::STEP_SPLIT);
	CHECK_EQ(emission.steps[4].buffer, 2u);
	const RDG::AsyncEmission::Step &consumer = emission.steps[5];
	CHECK_EQ(consumer.type, RDG::AsyncEmission::STEP_MAIN);
	CHECK_EQ(consumer.level, 2u);
	CHECK_EQ(consumer.buffer, 2u);
	CHECK(same_commands(consumer.commands, { 3 }));

	CHECK_EQ(emission.fork_index, int32_t(producer.buffer));
	CHECK_EQ(emission.join_index, int32_t(consumer.buffer));
	CHECK_EQ(emission.buffers_used, 2u);
	check_emission_order(emission, plan);

	// Exactly one wait on each side of the async queue.
	const uint32_t command_buffer_count = emission.buffers_used + 1;
	const LocalVector<Submit> submits = submit_frame(command_buffer_count, emission.fork_index, emission.join_index);
	check_fork_join_submits(submits, command_buffer_count, emission.fork_index, emission.join_index);
	// The side list's buffer does not wait for the async work; only the join buffer does.
	const Submit &side_submit = submits[find_submit(submits, main_command_buffer(int32_t(overlap.buffer)))];
	CHECK_FALSE(side_submit.waits.has(JOIN_SEMAPHORE));
	const Submit &join_submit = submits[find_submit(submits, main_command_buffer(emission.join_index))];
	REQUIRE_EQ(join_submit.waits.size(), 2u);
	CHECK_EQ(join_submit.waits[0], uint64_t(POOL_SEMAPHORE + 1));
	CHECK_EQ(join_submit.waits[1], uint64_t(JOIN_SEMAPHORE));

	free_trackers(trackers, 3);
}

TEST_CASE("[RD][AsyncCompute] a split made by the main commands keeps the fork and join indices") {
	// The swap chain pass begins its own command buffer. When that happens inside the frame, the
	// fork must still be the buffer that holds the producer and the join the one that holds the
	// consumer.
	RDG graph;
	RDG::ResourceTracker *source = tracker();
	RDG::ResourceTracker *result = tracker();
	RDG::ResourceTracker *trackers[] = { source, result };

	RDD::BufferCopyRegion region;
	graph.add_buffer_update(buffer_id(1), source, VectorView<RDG::RecordedBufferCopy>());
	add_async_compute(graph, source, result);
	graph.add_buffer_get_data(buffer_id(2), result, buffer_id(3), region);

	for (uint32_t split_level = 0; split_level < 3; split_level++) {
		CAPTURE(split_level);
		RDG::AsyncPlan plan;
		RDG::AsyncEmission emission;
		graph.emit_for_test(plan, emission, split_level);
		REQUIRE(plan.active);

		const RDG::AsyncEmission::Step *producer = find_step(emission, RDG::AsyncEmission::STEP_MAIN, 0);
		const RDG::AsyncEmission::Step *consumer = find_step(emission, RDG::AsyncEmission::STEP_MAIN, 2);
		REQUIRE(producer != nullptr);
		REQUIRE(consumer != nullptr);
		CHECK_EQ(emission.fork_index, int32_t(producer->buffer));
		CHECK(emission.join_index <= int32_t(consumer->buffer));
		check_emission_order(emission, plan);
		// The main split adds a third split; the fork and the join make the other two.
		CHECK_EQ(count_steps(emission, RDG::AsyncEmission::STEP_SPLIT), split_level == 1 ? 2u : 3u);

		const uint32_t command_buffer_count = emission.buffers_used + 1;
		check_fork_join_submits(submit_frame(command_buffer_count, emission.fork_index, emission.join_index), command_buffer_count, emission.fork_index, emission.join_index);
	}

	free_trackers(trackers, 2);
}

TEST_CASE("[RD][AsyncCompute] async work without a main consumer joins before the frame fence") {
	RDG graph;
	RDG::ResourceTracker *source = tracker();
	RDG::ResourceTracker *result = tracker();
	RDG::ResourceTracker *trackers[] = { source, result };

	graph.add_buffer_update(buffer_id(1), source, VectorView<RDG::RecordedBufferCopy>());
	add_async_compute(graph, source, result);

	RDG::AsyncPlan plan;
	RDG::AsyncEmission emission;
	graph.emit_for_test(plan, emission);
	REQUIRE(plan.active);
	CHECK_EQ(plan.join_level, plan.level_count);

	// The join happens in a trailing command buffer, which is the frame's last one.
	REQUIRE_FALSE(emission.steps.is_empty());
	CHECK_EQ(emission.steps[emission.steps.size() - 1].type, RDG::AsyncEmission::STEP_SPLIT);
	CHECK_EQ(emission.join_index, int32_t(emission.buffers_used));
	check_emission_order(emission, plan);

	const uint32_t command_buffer_count = emission.buffers_used + 1;
	const LocalVector<Submit> submits = submit_frame(command_buffer_count, emission.fork_index, emission.join_index);
	check_fork_join_submits(submits, command_buffer_count, emission.fork_index, emission.join_index);
	const Submit &last = submits[submits.size() - 1];
	CHECK_EQ(last.fence, uint64_t(LAST_FENCE));
	CHECK(last.waits.has(JOIN_SEMAPHORE));

	free_trackers(trackers, 2);
}

TEST_CASE("[RD][AsyncCompute] the device reports async compute through its feature") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, uint64_t(1) << RenderingDevice::SUPPORTS_ASYNC_COMPUTE);
	CHECK(rd->has_feature(RenderingDevice::SUPPORTS_ASYNC_COMPUTE));
	rd->set_feature_mock(true, 0);
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_ASYNC_COMPUTE));
	memdelete(rd);
}

TEST_CASE("[RD][AsyncCompute] compute lists record on the main queue unless async compute is active") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, 0);

	// Async compute inactive: an async request records on the main queue, before and after a barrier.
	RenderingDevice::ComputeListID list = rd->compute_list_begin(RenderingDevice::COMPUTE_QUEUE_ASYNC);
	REQUIRE(list != INVALID_LIST);
	CHECK_EQ(rd->compute_list_get_queue(list), RenderingDevice::COMPUTE_QUEUE_MAIN);
	rd->compute_list_add_barrier(list);
	CHECK_EQ(rd->compute_list_get_queue(list), RenderingDevice::COMPUTE_QUEUE_MAIN);
	rd->compute_list_end();

	// Async compute active: the async list stays async across a barrier, and a main list stays main.
	rd->set_async_compute_mock(true);
	list = rd->compute_list_begin(RenderingDevice::COMPUTE_QUEUE_ASYNC);
	REQUIRE(list != INVALID_LIST);
	CHECK_EQ(rd->compute_list_get_queue(list), RenderingDevice::COMPUTE_QUEUE_ASYNC);
	rd->compute_list_add_barrier(list);
	CHECK_EQ(rd->compute_list_get_queue(list), RenderingDevice::COMPUTE_QUEUE_ASYNC);
	rd->compute_list_add_barrier(list);
	CHECK_EQ(rd->compute_list_get_queue(list), RenderingDevice::COMPUTE_QUEUE_ASYNC);
	rd->compute_list_end();

	list = rd->compute_list_begin();
	REQUIRE(list != INVALID_LIST);
	CHECK_EQ(rd->compute_list_get_queue(list), RenderingDevice::COMPUTE_QUEUE_MAIN);
	rd->compute_list_add_barrier(list);
	CHECK_EQ(rd->compute_list_get_queue(list), RenderingDevice::COMPUTE_QUEUE_MAIN);
	rd->compute_list_end();

	rd->set_async_compute_mock(false);
	memdelete(rd);
}

TEST_CASE("[RD][Graph] a buffer written again in a later frame gets a barrier with its last write") {
	// GodotNext: rdg_cross_frame_waw (#262). The canvas state uniform buffer is copied to in one
	// frame and copied to again in the next with no read between: same usage, so only this rule
	// orders the two copies.
	using G = RenderingDeviceGraph;
	CHECK(G::cross_frame_write_after_write(G::RESOURCE_USAGE_COPY_TO, G::RESOURCE_USAGE_COPY_TO, true, true));
	CHECK(G::cross_frame_write_after_write(G::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE, G::RESOURCE_USAGE_COPY_TO, true, true));
	// Within a frame the write index already orders them; a read in between changes the usage.
	CHECK_FALSE(G::cross_frame_write_after_write(G::RESOURCE_USAGE_COPY_TO, G::RESOURCE_USAGE_COPY_TO, false, true));
	CHECK_FALSE(G::cross_frame_write_after_write(G::RESOURCE_USAGE_UNIFORM_BUFFER_READ, G::RESOURCE_USAGE_COPY_TO, true, true));
	CHECK_FALSE(G::cross_frame_write_after_write(G::RESOURCE_USAGE_COPY_TO, G::RESOURCE_USAGE_UNIFORM_BUFFER_READ, true, true));
	// A buffer never used before, and textures (their layout transitions), are left as they were.
	CHECK_FALSE(G::cross_frame_write_after_write(G::RESOURCE_USAGE_NONE, G::RESOURCE_USAGE_COPY_TO, true, true));
	CHECK_FALSE(G::cross_frame_write_after_write(G::RESOURCE_USAGE_COPY_TO, G::RESOURCE_USAGE_COPY_TO, true, false));
}

TEST_CASE("[RD][Graph] a texture's barriers wait on the stages of an access from an earlier frame") {
	// GodotNext: rdg_access_stages (#267). SSEffects clears its last-frame texture when it creates it
	// and builds the mips at the end of the render. A flush between the two (a synchronous readback
	// in the middle of the render, as in the voxel lighting check) puts the clear in one graph frame
	// and the mips in the next.
	RDG graph;
	RDG::ResourceTracker *texture = texture_tracker(1, 3);
	RDG::ResourceTracker *mips[3] = { mip_slice_tracker(texture, 2, 0), mip_slice_tracker(texture, 3, 1), mip_slice_tracker(texture, 4, 2) };

	// Frame 1: a copy writes the whole texture.
	graph.add_texture_update(texture->texture_driver_id, texture, VectorView<RDG::RecordedBufferToTextureCopy>());

	// Frame 2: mip 0 is written, then each mip from the one above it. Every mip starts from the
	// texture's copy write. Mip 0, the texture's first use in the frame, gets the stages of the
	// texture's previous frame; mips 1 and 2 depend only on the compute list before them.
	graph.begin();
	add_texture_compute(graph, nullptr, mips[0]);
	add_texture_compute(graph, mips[0], mips[1]);
	add_texture_compute(graph, mips[1], mips[2]);
	for (int32_t i = 0; i < 3; i++) {
		INFO("mip ", i);
		CHECK(previous_stages(graph, i).has_flag(RDD::PIPELINE_STAGE_COPY_BIT));
	}

	// Frame 3: the whole texture is sampled. Its barrier still names the copy write of frame 1,
	// while the texture's previous frame used only the compute stage; the mips go back to the
	// texture's layout from their compute writes.
	graph.begin();
	add_texture_compute(graph, texture, nullptr);
	CHECK(previous_stages(graph, 0).has_flag(RDD::PIPELINE_STAGE_COPY_BIT));
	CHECK(previous_stages(graph, 0).has_flag(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT));

	// Frame 4: the texture's last access is now the sample, so a copy into it no longer waits on the
	// copy stage.
	graph.begin();
	graph.add_texture_update(texture->texture_driver_id, texture, VectorView<RDG::RecordedBufferToTextureCopy>());
	CHECK(previous_stages(graph, 0).has_flag(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT));
	CHECK_FALSE(previous_stages(graph, 0).has_flag(RDD::PIPELINE_STAGE_COPY_BIT));

	free_trackers(mips, 3);
	free_trackers(&texture, 1);
}

TEST_CASE("[RD][AsyncCompute] an invalid queue is rejected") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, 0);
	rd->set_async_compute_mock(true);

	ErrorCapture errors;
	ERR_PRINT_OFF;
	const RenderingDevice::ComputeListID list = rd->compute_list_begin(RenderingDevice::COMPUTE_QUEUE_MAX);
	ERR_PRINT_ON;
	CHECK_EQ(list, INVALID_LIST);
	CHECK_EQ(errors.count, 1);
	CHECK(errors.last_message.contains("Invalid compute queue."));

	// No list was begun, so a valid one can start.
	const RenderingDevice::ComputeListID valid = rd->compute_list_begin(RenderingDevice::COMPUTE_QUEUE_ASYNC);
	CHECK(valid != INVALID_LIST);
	rd->compute_list_end();

	rd->set_async_compute_mock(false);
	memdelete(rd);
}

TEST_CASE("[RD][AsyncCompute] the async queue needs a compute family of its own") {
	// Vulkan queues are virtual: a compute queue created on the main family is the main VkQueue,
	// which is what a device without a compute-only family offers.
	CHECK(RenderingDevice::async_compute_queue_family_usable(RDD::CommandQueueFamilyID(1), RDD::CommandQueueFamilyID(3)));
	CHECK_FALSE(RenderingDevice::async_compute_queue_family_usable(RDD::CommandQueueFamilyID(1), RDD::CommandQueueFamilyID(1)));
	CHECK_FALSE(RenderingDevice::async_compute_queue_family_usable(RDD::CommandQueueFamilyID(1), RDD::CommandQueueFamilyID()));
}

TEST_CASE("[RD][AsyncCompute] the async queue's barriers keep only compute-queue stages, accesses and layouts") {
	using G = RenderingDeviceGraph;

	// Compute and copy stages stay as they are.
	BitField<RDD::PipelineStageBits> compute_copy = RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	compute_copy.set_flag(RDD::PIPELINE_STAGE_COPY_BIT);
	CHECK_EQ(uint64_t(G::async_queue_stages(compute_copy)), uint64_t(compute_copy));
	CHECK_EQ(uint64_t(G::async_queue_stages(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT)), uint64_t(RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT));
	CHECK_EQ(uint64_t(G::async_queue_stages(RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT)), uint64_t(RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT));

	// Graphics stages go, and the compute stage takes their place so the barrier chains to the fork
	// semaphore's wait.
	BitField<RDD::PipelineStageBits> graphics = RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	graphics.set_flag(RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	graphics.set_flag(RDD::PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
	graphics.set_flag(RDD::PIPELINE_STAGE_RESOLVE_BIT);
	graphics.set_flag(RDD::PIPELINE_STAGE_ALL_GRAPHICS_BIT);
	graphics.set_flag(RDD::PIPELINE_STAGE_COPY_BIT);
	BitField<RDD::PipelineStageBits> masked = G::async_queue_stages(graphics);
	CHECK(masked.has_flag(RDD::PIPELINE_STAGE_COPY_BIT));
	CHECK(masked.has_flag(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT));
	CHECK_FALSE(masked.has_flag(RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT));
	CHECK_FALSE(masked.has_flag(RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));
	CHECK_FALSE(masked.has_flag(RDD::PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT));
	CHECK_FALSE(masked.has_flag(RDD::PIPELINE_STAGE_RESOLVE_BIT));
	CHECK_FALSE(masked.has_flag(RDD::PIPELINE_STAGE_ALL_GRAPHICS_BIT));

	// Graphics accesses go; compute, copy and memory accesses stay.
	BitField<RDD::BarrierAccessBits> access = RDD::BARRIER_ACCESS_SHADER_READ_BIT;
	access.set_flag(RDD::BARRIER_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
	access.set_flag(RDD::BARRIER_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
	access.set_flag(RDD::BARRIER_ACCESS_RESOLVE_WRITE_BIT);
	access.set_flag(RDD::BARRIER_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
	access.set_flag(RDD::BARRIER_ACCESS_INDEX_READ_BIT);
	access.set_flag(RDD::BARRIER_ACCESS_INPUT_ATTACHMENT_READ_BIT);
	CHECK_EQ(uint64_t(G::async_queue_access(access)), uint64_t(RDD::BARRIER_ACCESS_SHADER_READ_BIT));
	BitField<RDD::BarrierAccessBits> compute_access = RDD::BARRIER_ACCESS_UNIFORM_READ_BIT;
	compute_access.set_flag(RDD::BARRIER_ACCESS_SHADER_WRITE_BIT);
	compute_access.set_flag(RDD::BARRIER_ACCESS_INDIRECT_COMMAND_READ_BIT);
	compute_access.set_flag(RDD::BARRIER_ACCESS_COPY_READ_BIT);
	compute_access.set_flag(RDD::BARRIER_ACCESS_COPY_WRITE_BIT);
	compute_access.set_flag(RDD::BARRIER_ACCESS_MEMORY_WRITE_BIT);
	compute_access.set_flag(RDD::BARRIER_ACCESS_ACCELERATION_STRUCTURE_READ_BIT);
	CHECK_EQ(uint64_t(G::async_queue_access(compute_access)), uint64_t(compute_access));

	// The layouts a D3D12 compute command list accepts.
	CHECK(G::async_queue_supports_texture_layout(RDD::TEXTURE_LAYOUT_UNDEFINED));
	CHECK(G::async_queue_supports_texture_layout(RDD::TEXTURE_LAYOUT_GENERAL));
	CHECK(G::async_queue_supports_texture_layout(RDD::TEXTURE_LAYOUT_STORAGE_OPTIMAL));
	CHECK(G::async_queue_supports_texture_layout(RDD::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
	CHECK(G::async_queue_supports_texture_layout(RDD::TEXTURE_LAYOUT_COPY_SRC_OPTIMAL));
	CHECK(G::async_queue_supports_texture_layout(RDD::TEXTURE_LAYOUT_COPY_DST_OPTIMAL));
	CHECK_FALSE(G::async_queue_supports_texture_layout(RDD::TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
	CHECK_FALSE(G::async_queue_supports_texture_layout(RDD::TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL));
	CHECK_FALSE(G::async_queue_supports_texture_layout(RDD::TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL));
	CHECK_FALSE(G::async_queue_supports_texture_layout(RDD::TEXTURE_LAYOUT_RESOLVE_SRC_OPTIMAL));
	CHECK_FALSE(G::async_queue_supports_texture_layout(RDD::TEXTURE_LAYOUT_RESOLVE_DST_OPTIMAL));
	CHECK_FALSE(G::async_queue_supports_texture_layout(RDD::TEXTURE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL));
	CHECK_FALSE(G::async_queue_supports_texture_layout(RDD::TEXTURE_LAYOUT_FRAGMENT_DENSITY_MAP_ATTACHMENT_OPTIMAL));
}

TEST_CASE("[RD][AsyncCompute] an async list's barrier from a graphics layout runs on the main queue before the fork") {
	// A resolve writes one texture and a copy another; an async list samples both and writes a third,
	// which a main readback consumes. The resolve destination's transition starts from a layout a
	// compute queue cannot use, so it moves to the end of the fork command buffer; the others stay on
	// the async queue without the resolve's graphics stage.
	RDG graph;
	graph.set_driver_traits_for_test(true, true);
	RDG::ResourceTracker *msaa = texture_tracker(1, 1);
	RDG::ResourceTracker *resolved = texture_tracker(2, 1);
	RDG::ResourceTracker *copied = texture_tracker(3, 1);
	RDG::ResourceTracker *written = texture_tracker(4, 1);
	RDG::ResourceTracker *trackers[] = { msaa, resolved, copied, written };

	graph.add_texture_resolve(msaa->texture_driver_id, msaa, resolved->texture_driver_id, resolved, 0, 0, 0, 0);
	graph.add_texture_update(copied->texture_driver_id, copied, VectorView<RDG::RecordedBufferToTextureCopy>());
	add_async_sampling_compute(graph, { resolved, copied }, written);
	graph.add_texture_get_data(written->texture_driver_id, written, buffer_id(9), VectorView<RDD::BufferTextureCopyRegion>());

	RDG::AsyncPlan plan;
	RDG::AsyncBarriers fork;
	graph.async_barriers_for_test(RDG::BARRIER_SELECTION_ASYNC_FORK, plan, fork);
	REQUIRE(plan.active);
	CHECK_EQ(plan.fork_level, 1u);
	CHECK_EQ(plan.join_level, 2u);

	// The fork: only the resolve destination's transition, as recorded, with the resolve stage it
	// waits on (valid on the main queue).
	REQUIRE_EQ(fork.texture_barriers.size(), 1u);
	CHECK_EQ(count_texture_barriers(fork.texture_barriers, 2, RDD::TEXTURE_LAYOUT_RESOLVE_DST_OPTIMAL, RDD::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), 1u);
	CHECK(fork.texture_barriers[0].src_access.has_flag(RDD::BARRIER_ACCESS_RESOLVE_WRITE_BIT));
	CHECK(fork.texture_barriers[0].dst_access.has_flag(RDD::BARRIER_ACCESS_SHADER_READ_BIT));
	CHECK(fork.src_stages.has_flag(RDD::PIPELINE_STAGE_RESOLVE_BIT));
	CHECK(fork.dst_stages.has_flag(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT));
	CHECK(fork.buffer_barriers.is_empty());

	// The async queue: the copy destination's and the written texture's transitions, and no resolve
	// stage, access or layout.
	RDG::AsyncBarriers async;
	graph.async_barriers_for_test(RDG::BARRIER_SELECTION_ASYNC_QUEUE, plan, async);
	REQUIRE(plan.active);
	CHECK_EQ(async.texture_barriers.size(), 2u);
	CHECK_EQ(count_texture_barriers(async.texture_barriers, 3, RDD::TEXTURE_LAYOUT_COPY_DST_OPTIMAL, RDD::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), 1u);
	CHECK_EQ(count_texture_barriers(async.texture_barriers, 4, RDD::TEXTURE_LAYOUT_UNDEFINED, RDD::TEXTURE_LAYOUT_STORAGE_OPTIMAL), 1u);
	CHECK(find_texture_barrier(async.texture_barriers, 2) == nullptr);
	const RDD::TextureBarrier *copied_barrier = find_texture_barrier(async.texture_barriers, 3);
	REQUIRE(copied_barrier != nullptr);
	CHECK(copied_barrier->src_access.has_flag(RDD::BARRIER_ACCESS_COPY_WRITE_BIT));
	CHECK(async.src_stages.has_flag(RDD::PIPELINE_STAGE_COPY_BIT));
	CHECK(async.src_stages.has_flag(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT));
	CHECK_FALSE(async.src_stages.has_flag(RDD::PIPELINE_STAGE_RESOLVE_BIT));

	// end() records the fork barriers on the fork command buffer, before it ends.
	RDG::AsyncPlan emission_plan;
	RDG::AsyncEmission emission;
	graph.emit_for_test(emission_plan, emission);
	REQUIRE(emission_plan.active);
	CHECK_EQ(emission.fork_barrier_buffer, emission.fork_index);

	free_trackers(trackers, 4);
}

TEST_CASE("[RD][AsyncCompute] without image layouts every barrier stays on the async queue") {
	// Metal has no layouts, and its async check passed with the barriers as recorded.
	RDG graph;
	graph.set_driver_traits_for_test(true, false);
	RDG::ResourceTracker *msaa = texture_tracker(1, 1);
	RDG::ResourceTracker *resolved = texture_tracker(2, 1);
	RDG::ResourceTracker *written = texture_tracker(3, 1);
	RDG::ResourceTracker *trackers[] = { msaa, resolved, written };

	graph.add_texture_resolve(msaa->texture_driver_id, msaa, resolved->texture_driver_id, resolved, 0, 0, 0, 0);
	add_async_sampling_compute(graph, { resolved }, written);
	graph.add_texture_get_data(written->texture_driver_id, written, buffer_id(9), VectorView<RDD::BufferTextureCopyRegion>());

	RDG::AsyncPlan plan;
	RDG::AsyncBarriers fork;
	graph.async_barriers_for_test(RDG::BARRIER_SELECTION_ASYNC_FORK, plan, fork);
	REQUIRE(plan.active);
	CHECK(fork.texture_barriers.is_empty());

	RDG::AsyncBarriers async;
	graph.async_barriers_for_test(RDG::BARRIER_SELECTION_ASYNC_QUEUE, plan, async);
	CHECK_EQ(count_texture_barriers(async.texture_barriers, 2, RDD::TEXTURE_LAYOUT_RESOLVE_DST_OPTIMAL, RDD::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), 1u);
	CHECK(async.src_stages.has_flag(RDD::PIPELINE_STAGE_RESOLVE_BIT));

	free_trackers(trackers, 3);
}

TEST_CASE("[RD][AsyncCompute] a fork barrier on a resource an earlier async list used makes the frame fall back") {
	// An async list writes mip 1 of a resolved texture, so the slice's transition out of the resolve
	// layout moves before the fork. A second async list then samples the whole texture: the slice
	// goes back to the parent's resolve layout first, from the layout the first async list left it
	// in. That source is on the async queue, so the normalization cannot run before the fork.
	for (int pass = 0; pass < 3; pass++) {
		const bool second_list = pass != 1;
		const bool layouts = pass != 2;
		CAPTURE(second_list);
		CAPTURE(layouts);

		RDG graph;
		graph.set_driver_traits_for_test(true, layouts);
		RDG::ResourceTracker *msaa = texture_tracker(1, 2);
		RDG::ResourceTracker *texture = texture_tracker(2, 2);
		RDG::ResourceTracker *mip1 = mip_slice_tracker(texture, 3, 1);
		RDG::ResourceTracker *trackers[] = { mip1, msaa, texture };

		graph.add_texture_resolve(msaa->texture_driver_id, msaa, texture->texture_driver_id, texture, 0, 0, 0, 0);
		add_texture_compute(graph, nullptr, mip1, RDG::QUEUE_ASYNC_COMPUTE);
		if (second_list) {
			add_texture_compute(graph, texture, nullptr, RDG::QUEUE_ASYNC_COMPUTE);
		}

		LocalVector<int32_t> order;
		RDG::AsyncPlan plan;
		graph.plan_for_test(order, plan, true);
		CHECK_EQ(plan.async_count, second_list ? 2u : 1u);
		// The partition alone allows a fork: the resolve feeds the async lists and nothing on the main
		// queue consumes them.
		CHECK_EQ(plan.fork_level, 1u);
		// With layouts, the second list's normalization makes the frame fall back; one list, or no
		// layouts (Metal), keeps the fork.
		CHECK_EQ(plan.active, !(second_list && layouts));

		if (!second_list) {
			// The single list's transition out of the resolve layout is the fork barrier.
			RDG::AsyncBarriers fork;
			graph.async_barriers_for_test(RDG::BARRIER_SELECTION_ASYNC_FORK, plan, fork);
			REQUIRE(plan.active);
			CHECK_EQ(count_texture_barriers(fork.texture_barriers, 3, RDD::TEXTURE_LAYOUT_RESOLVE_DST_OPTIMAL, RDD::TEXTURE_LAYOUT_STORAGE_OPTIMAL), 1u);
		}

		free_trackers(trackers, 3);
	}
}

TEST_CASE("[RD][AsyncCompute] the overlap fixture's draw list runs on the main queue between the fork and the join") {
	// godotnext/testbed/tests/rd_parity/async_overlap.gd times frames of a timestamp, an async
	// compute list, an independent draw list and a closing timestamp. The draw list must be recorded
	// into a main command buffer after the fork and before the join, so the two passes can overlap.
	RDG graph;
	RDG::ResourceTracker *input = tracker();
	RDG::ResourceTracker *output = tracker();
	RDG::ResourceTracker *color = texture_tracker(10, 1);
	RDG::ResourceTracker *trackers[] = { input, output, color };

	graph.add_capture_timestamp(RDD::QueryPoolID(1), 0);
	add_async_compute(graph, input, output);
	add_color_pass(graph, color);
	graph.add_capture_timestamp(RDD::QueryPoolID(1), 1);

	RDG::AsyncPlan plan;
	RDG::AsyncEmission emission;
	graph.emit_for_test(plan, emission);
	REQUIRE(plan.active);
	CHECK_EQ(plan.async_count, 1u);
	CHECK_EQ(plan.main_count, 3u);
	CHECK_EQ(plan.fork_level, 1u);
	CHECK_EQ(plan.join_level, 2u);

	const RDG::AsyncEmission::Step *opening = find_step(emission, RDG::AsyncEmission::STEP_MAIN, 0);
	const RDG::AsyncEmission::Step *draw = find_step(emission, RDG::AsyncEmission::STEP_MAIN, 1);
	const RDG::AsyncEmission::Step *async = find_step(emission, RDG::AsyncEmission::STEP_ASYNC, 1);
	const RDG::AsyncEmission::Step *closing = find_step(emission, RDG::AsyncEmission::STEP_MAIN, 2);
	REQUIRE(opening != nullptr);
	REQUIRE(draw != nullptr);
	REQUIRE(async != nullptr);
	REQUIRE(closing != nullptr);
	CHECK(same_commands(opening->commands, { 0 }));
	CHECK(same_commands(draw->commands, { 2 }));
	CHECK(same_commands(async->commands, { 1 }));
	CHECK(same_commands(closing->commands, { 3 }));
	CHECK_EQ(int32_t(opening->buffer), emission.fork_index);
	CHECK(int32_t(draw->buffer) > emission.fork_index);
	CHECK(int32_t(draw->buffer) < emission.join_index);
	CHECK_EQ(int32_t(closing->buffer), emission.join_index);
	check_emission_order(emission, plan);

	const uint32_t command_buffer_count = emission.buffers_used + 1;
	const LocalVector<Submit> submits = submit_frame(command_buffer_count, emission.fork_index, emission.join_index);
	check_fork_join_submits(submits, command_buffer_count, emission.fork_index, emission.join_index);
	// The draw list's command buffer does not wait for the async work.
	CHECK_FALSE(submits[find_submit(submits, main_command_buffer(int32_t(draw->buffer)))].waits.has(JOIN_SEMAPHORE));

	// With async compute off, the compute list records on the main queue. Both passes share level 1,
	// where draw lists sort before compute lists.
	RDG main_graph;
	RDG::ResourceTracker *input_main = tracker();
	RDG::ResourceTracker *output_main = tracker();
	RDG::ResourceTracker *color_main = texture_tracker(10, 1);
	RDG::ResourceTracker *main_trackers[] = { input_main, output_main, color_main };
	main_graph.add_capture_timestamp(RDD::QueryPoolID(1), 0);
	add_main_compute(main_graph, input_main, output_main);
	add_color_pass(main_graph, color_main);
	main_graph.add_capture_timestamp(RDD::QueryPoolID(1), 1);

	LocalVector<int32_t> main_order;
	RDG::AsyncPlan main_plan;
	main_graph.plan_for_test(main_order, main_plan, true);
	CHECK_FALSE(main_plan.active);
	CHECK_EQ(main_plan.async_count, 0u);
	CHECK(same_commands(main_order, { 0, 2, 1, 3 }));

	free_trackers(trackers, 3);
	free_trackers(main_trackers, 3);
}

TEST_CASE("[RD][AsyncCompute] a timestamp between the async list and the draw list puts the draw list after the join") {
	// A timestamp depends on every command since the previous one, and every later command depends
	// on it. Between the passes, it consumes the async work, so the draw list can no longer overlap
	// it. This is why the overlap fixture brackets both passes with one pair of timestamps.
	RDG graph;
	RDG::ResourceTracker *input = tracker();
	RDG::ResourceTracker *output = tracker();
	RDG::ResourceTracker *color = texture_tracker(10, 1);
	RDG::ResourceTracker *trackers[] = { input, output, color };

	graph.add_capture_timestamp(RDD::QueryPoolID(1), 0);
	add_async_compute(graph, input, output);
	graph.add_capture_timestamp(RDD::QueryPoolID(1), 1);
	add_color_pass(graph, color);
	graph.add_capture_timestamp(RDD::QueryPoolID(1), 2);

	RDG::AsyncPlan plan;
	RDG::AsyncEmission emission;
	graph.emit_for_test(plan, emission);
	REQUIRE(plan.active);
	CHECK_EQ(plan.fork_level, 1u);
	CHECK_EQ(plan.join_level, 2u);

	const RDG::AsyncEmission::Step *draw = find_step(emission, RDG::AsyncEmission::STEP_MAIN, 3);
	REQUIRE(draw != nullptr);
	CHECK(same_commands(draw->commands, { 3 }));
	CHECK(int32_t(draw->buffer) >= emission.join_index);
	check_emission_order(emission, plan);

	free_trackers(trackers, 3);
}

// GodotNext: pt_tsr_async_compute (#193).
TEST_CASE("[RD][AsyncCompute][TSR] without async compute every level records the level-0 frame") {
	// nextgi_tsr_async_schedule() keeps every pass on the main queue at the upscaler's place when
	// the device has no active async queue, so the graph sees exactly the level-0 frame.
	RDG level0_graph;
	TSRFrame level0_frame;
	record_tsr_frame(level0_graph, nextgi_tsr_async_schedule(0, true, true), level0_frame);
	LocalVector<int32_t> level0_order;
	RDG::AsyncPlan level0_plan;
	level0_graph.plan_for_test(level0_order, level0_plan, true);
	CHECK_FALSE(level0_plan.active);
	CHECK_EQ(level0_plan.async_count, 0u);

	for (int64_t level = 0; level <= 3; level++) {
		CAPTURE(level);
		RDG graph;
		TSRFrame frame;
		record_tsr_frame(graph, nextgi_tsr_async_schedule(level, false, true), frame);
		LocalVector<int32_t> order;
		RDG::AsyncPlan plan;
		graph.plan_for_test(order, plan, true);
		CHECK_FALSE(plan.active);
		CHECK_EQ(plan.async_count, 0u);
		CHECK_EQ(plan.main_count, level0_plan.main_count);
		CHECK(same_order(order, level0_order));
	}

	// Level 1 and level 2 without the split record the level-0 frame too.
	const TSRAsyncSchedule same_as_level0[] = { nextgi_tsr_async_schedule(1, true, true), nextgi_tsr_async_schedule(2, true, false) };
	for (const TSRAsyncSchedule &schedule : same_as_level0) {
		RDG graph;
		TSRFrame frame;
		record_tsr_frame(graph, schedule, frame);
		LocalVector<int32_t> order;
		RDG::AsyncPlan plan;
		graph.plan_for_test(order, plan, true);
		CHECK_EQ(plan.async_count, 0u);
		CHECK(same_order(order, level0_order));
	}
}

TEST_CASE("[RD][AsyncCompute][TSR] level 2 forks after the motion vectors and joins at the sky pass") {
	// The two async passes sample the depth buffer, and every later draw list uses it as a
	// read-write attachment, so the sky pass waits for them (write after read). In the frame
	// RenderForwardClustered records, nothing on the main queue lies between the fork and that join:
	// the async passes overlap no main work yet (read-only depth attachments would let the sky and
	// transparent passes run beside them).
	RDG graph;
	graph.set_driver_traits_for_test(true, true);
	TSRFrame frame;
	const TSRAsyncSchedule schedule = nextgi_tsr_async_schedule(2, true, true);
	REQUIRE(schedule.split);
	record_tsr_frame(graph, schedule, frame);

	RDG::AsyncPlan plan;
	RDG::AsyncEmission emission;
	graph.emit_for_test(plan, emission);
	REQUIRE(plan.active);
	CHECK_EQ(plan.async_count, 2u);

	const RDG::AsyncEmission::Step *motion_pass = find_command_step(emission, TSR_CMD_MOTION_PASS);
	const RDG::AsyncEmission::Step *dilation = find_command_step(emission, TSR_CMD_SPLIT_DILATION);
	const RDG::AsyncEmission::Step *disocclusion = find_command_step(emission, TSR_CMD_SPLIT_DISOCCLUSION);
	const RDG::AsyncEmission::Step *sky = find_command_step(emission, TSR_CMD_SPLIT_SKY);
	REQUIRE(motion_pass != nullptr);
	REQUIRE(dilation != nullptr);
	REQUIRE(disocclusion != nullptr);
	REQUIRE(sky != nullptr);
	CHECK_EQ(dilation->type, RDG::AsyncEmission::STEP_ASYNC);
	CHECK_EQ(disocclusion->type, RDG::AsyncEmission::STEP_ASYNC);
	CHECK_EQ(sky->type, RDG::AsyncEmission::STEP_MAIN);

	// The fork follows the motion pass; the join is the sky pass, the first draw list with depth.
	CHECK_EQ(plan.fork_level, motion_pass->level + 1);
	CHECK_EQ(dilation->level, plan.fork_level);
	CHECK_EQ(disocclusion->level, plan.fork_level + 1);
	CHECK_EQ(plan.join_level, sky->level);
	CHECK_EQ(sky->level, disocclusion->level + 1);
	CHECK(int32_t(motion_pass->buffer) <= emission.fork_index);
	CHECK(int32_t(sky->buffer) >= emission.join_index);
	// No main step inside the window.
	for (const RDG::AsyncEmission::Step &step : emission.steps) {
		if (step.type == RDG::AsyncEmission::STEP_MAIN) {
			CAPTURE(step.level);
			CHECK((int32_t(step.buffer) <= emission.fork_index || int32_t(step.buffer) >= emission.join_index));
		}
	}
	check_emission_order(emission, plan);

	// One fork and one join: the async work waits once, and the frame waits once for it.
	const uint32_t command_buffer_count = emission.buffers_used + 1;
	check_fork_join_submits(submit_frame(command_buffer_count, emission.fork_index, emission.join_index), command_buffer_count, emission.fork_index, emission.join_index);

	// The depth and velocity leave their attachment layouts on the main queue before the fork; the
	// TSR textures change layout on the async queue.
	RDG::AsyncPlan barrier_plan;
	RDG::AsyncBarriers fork;
	graph.async_barriers_for_test(RDG::BARRIER_SELECTION_ASYNC_FORK, barrier_plan, fork);
	REQUIRE(barrier_plan.active);
	CHECK_EQ(fork.texture_barriers.size(), 2u);
	CHECK_EQ(count_texture_barriers(fork.texture_barriers, TSRFrame::DEPTH, RDD::TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, RDD::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), 1u);
	CHECK_EQ(count_texture_barriers(fork.texture_barriers, TSRFrame::VELOCITY, RDD::TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, RDD::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), 1u);
	RDG::AsyncBarriers async;
	graph.async_barriers_for_test(RDG::BARRIER_SELECTION_ASYNC_QUEUE, barrier_plan, async);
	CHECK(find_texture_barrier(async.texture_barriers, TSRFrame::DEPTH) == nullptr);
	CHECK(find_texture_barrier(async.texture_barriers, TSRFrame::VELOCITY) == nullptr);
	CHECK(find_texture_barrier(async.texture_barriers, TSRFrame::DILATED) != nullptr);
	CHECK(find_texture_barrier(async.texture_barriers, TSRFrame::DISOCCLUSION) != nullptr);
	CHECK(find_texture_barrier(async.texture_barriers, TSRFrame::PREV_DEPTH) != nullptr);
	for (const RDD::TextureBarrier &barrier : async.texture_barriers) {
		CHECK(RDG::async_queue_supports_texture_layout(barrier.prev_layout));
		CHECK(RDG::async_queue_supports_texture_layout(barrier.next_layout));
	}
}

TEST_CASE("[RD][AsyncCompute][TSR] only the first TSR viewport of a frame uses the async queue") {
	// A second TSR viewport whose opaque pass samples the first one's output: its motion pass, the
	// producer of its async passes, comes after the first viewport's sky pass, the consumer of the
	// first viewport's. One fork and one join cannot cover both, so the whole frame would go back to
	// the main queue. nextgi_tsr_async_available() gives the queue to the first viewport only.
	const int64_t levels[] = { 2, 3 };
	for (int64_t level : levels) {
		CAPTURE(level);
		{
			RDG graph;
			graph.set_driver_traits_for_test(true, true);
			TSRFrame first(0);
			TSRFrame second(100);
			const TSRAsyncSchedule schedule = nextgi_tsr_async_schedule(level, true, true);
			record_tsr_frame(graph, schedule, first);
			record_tsr_frame(graph, schedule, second, first[TSRFrame::FINAL]);
			LocalVector<int32_t> order;
			RDG::AsyncPlan plan;
			graph.plan_for_test(order, plan, true);
			CHECK(plan.async_count > 0u);
			CHECK(plan.fork_level >= plan.join_level);
			CHECK_FALSE(plan.active);
		}
		{
			RDG graph;
			graph.set_driver_traits_for_test(true, true);
			TSRFrame first(0);
			TSRFrame second(100);
			const uint64_t rd_frame = 41;
			uint64_t claimed_frame = UINT64_MAX;
			const TSRAsyncSchedule first_schedule = nextgi_tsr_async_schedule(level, nextgi_tsr_async_available(true, 1, rd_frame, claimed_frame), true);
			REQUIRE(first_schedule.uses_async());
			claimed_frame = rd_frame;
			const TSRAsyncSchedule second_schedule = nextgi_tsr_async_schedule(level, nextgi_tsr_async_available(true, 1, rd_frame, claimed_frame), true);
			CHECK_FALSE(second_schedule.uses_async());
			record_tsr_frame(graph, first_schedule, first);
			record_tsr_frame(graph, second_schedule, second, first[TSRFrame::FINAL]);
			RDG::AsyncPlan plan;
			RDG::AsyncEmission emission;
			graph.emit_for_test(plan, emission);
			CHECK(plan.active);
			CHECK_EQ(plan.async_count, level == 2 ? 2u : 5u);
			check_emission_order(emission, plan);
		}
	}
}

TEST_CASE("[RD][AsyncCompute][TSR] level 3 records every pass at the upscaler's place") {
	// Every pass on the async queue after the transparent pass: one fork after it, one join at the
	// tonemapper.
	{
		RDG graph;
		graph.set_driver_traits_for_test(true, true);
		TSRFrame frame;
		const TSRAsyncSchedule schedule = nextgi_tsr_async_schedule(3, true, true);
		record_tsr_frame(graph, schedule, frame);
		RDG::AsyncPlan plan;
		RDG::AsyncEmission emission;
		graph.emit_for_test(plan, emission);
		REQUIRE(plan.active);
		CHECK_EQ(plan.async_count, 5u);
		check_emission_order(emission, plan);
		const uint32_t command_buffer_count = emission.buffers_used + 1;
		check_fork_join_submits(submit_frame(command_buffer_count, emission.fork_index, emission.join_index), command_buffer_count, emission.fork_index, emission.join_index);
	}

	// Splitting the depth and velocity passes off as well puts async work on both sides of the
	// transparent pass: a consumer of the first part comes before a producer of the second, which
	// one fork and one join cannot cover, so the whole frame would fall back to the main queue.
	{
		RDG graph;
		graph.set_driver_traits_for_test(true, true);
		TSRFrame frame;
		TSRAsyncSchedule split_everything = nextgi_tsr_async_schedule(2, true, true);
		for (uint32_t i = 0; i < TSR_PASS_MAX; i++) {
			split_everything.async[i] = true;
		}
		record_tsr_frame(graph, split_everything, frame);
		LocalVector<int32_t> order;
		RDG::AsyncPlan plan;
		graph.plan_for_test(order, plan, true);
		CHECK_EQ(plan.async_count, 5u);
		CHECK(plan.fork_level >= plan.join_level);
		CHECK_FALSE(plan.active);
	}
}

#endif // RD_ENABLED
