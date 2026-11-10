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

#endif // RD_ENABLED
