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

#include "servers/rendering/rendering_device_graph.h"

using RDG = RenderingDeviceGraph;

namespace {

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

RDG::ResourceTracker *tracker() {
	return RDG::resource_tracker_create();
}

void free_trackers(RDG::ResourceTracker **p_trackers, uint32_t p_count) {
	for (uint32_t i = 0; i < p_count; i++) {
		RDG::resource_tracker_free(p_trackers[i]);
	}
}

void add_async_compute(RDG &p_graph, RDG::ResourceTracker *p_read, RDG::ResourceTracker *p_write) {
	p_graph.add_compute_list_begin(RDD::BreadcrumbMarker::NONE, 0, RDG::QUEUE_ASYNC_COMPUTE);
	if (p_read != nullptr) {
		p_graph.add_compute_list_usage(p_read, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
	}
	if (p_write != nullptr) {
		p_graph.add_compute_list_usage(p_write, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE);
	}
	p_graph.add_compute_list_dispatch(1, 1, 1);
	p_graph.add_compute_list_end();
}

void add_main_compute(RDG &p_graph) {
	p_graph.add_compute_list_begin();
	p_graph.add_compute_list_dispatch(1, 1, 1);
	p_graph.add_compute_list_end();
}

} // namespace

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

	RDG::ResourceTracker *trackers[] = { cleared, cleared_main };
	free_trackers(trackers, 2);
}

TEST_CASE("[RD][AsyncCompute] overlapping producer and consumer stays on the main queue") {
	RDG graph;
	RDG::ResourceTracker *source = tracker();
	RDG::ResourceTracker *mid = tracker();
	RDG::ResourceTracker *destination = tracker();
	RDG::ResourceTracker *output = tracker();
	RDG::ResourceTracker *trackers[] = { source, mid, destination, output };

	RDD::BufferCopyRegion region;
	graph.add_buffer_update(buffer_id(1), source, VectorView<RDG::RecordedBufferCopy>());
	add_async_compute(graph, source, mid);
	graph.add_buffer_copy(buffer_id(2), mid, buffer_id(3), destination, region);
	add_async_compute(graph, destination, output);
	graph.add_buffer_get_data(buffer_id(4), output, buffer_id(5), region);

	RDG main_graph;
	RDG::ResourceTracker *source_main = tracker();
	RDG::ResourceTracker *mid_main = tracker();
	RDG::ResourceTracker *destination_main = tracker();
	RDG::ResourceTracker *output_main = tracker();
	main_graph.add_buffer_update(buffer_id(1), source_main, VectorView<RDG::RecordedBufferCopy>());
	main_graph.add_compute_list_begin();
	main_graph.add_compute_list_usage(source_main, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
	main_graph.add_compute_list_usage(mid_main, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE);
	main_graph.add_compute_list_dispatch(1, 1, 1);
	main_graph.add_compute_list_end();
	main_graph.add_buffer_copy(buffer_id(2), mid_main, buffer_id(3), destination_main, region);
	main_graph.add_compute_list_begin();
	main_graph.add_compute_list_usage(destination_main, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
	main_graph.add_compute_list_usage(output_main, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE);
	main_graph.add_compute_list_dispatch(1, 1, 1);
	main_graph.add_compute_list_end();
	main_graph.add_buffer_get_data(buffer_id(4), output_main, buffer_id(5), region);

	LocalVector<int32_t> order;
	LocalVector<int32_t> main_order;
	RDG::AsyncPlan plan;
	RDG::AsyncPlan main_plan;
	graph.plan_for_test(order, plan, true);
	main_graph.plan_for_test(main_order, main_plan, true);

	CHECK_FALSE(plan.active);
	CHECK(same_order(order, main_order));
	const int32_t expected[] = { 0, 1, 2, 3, 4 };
	CHECK_EQ(order.size(), 5u);
	for (uint32_t i = 0; i < order.size(); i++) {
		CHECK_EQ(order[i], expected[i]);
	}

	RDG::ResourceTracker *main_trackers[] = { source_main, mid_main, destination_main, output_main };
	free_trackers(trackers, 4);
	free_trackers(main_trackers, 4);
}
