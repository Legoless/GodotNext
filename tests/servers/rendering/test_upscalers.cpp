/**************************************************************************/
/*  test_upscalers.cpp                                                    */
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

TEST_FORCE_LINK(test_upscalers)

#ifdef RD_ENABLED
#include "servers/rendering/rendering_device_graph.h"

// GodotNext: pu_core_rd_callback_stages (#101). Add-time stage propagation needs no driver, so the
// graph can be exercised without a device.
static void _test_upscalers_noop_callback(RenderingDeviceDriver *, RDD::CommandBufferID, void *) {
}

static void _add_test_callback(RenderingDeviceGraph &r_graph, RenderingDeviceGraph::ResourceTracker *p_tracker, RenderingDeviceGraph::ResourceUsage p_usage, BitField<RDD::PipelineStageBits> p_stages) {
	RenderingDeviceGraph::ResourceTracker *trackers[1] = { p_tracker };
	RenderingDeviceGraph::ResourceUsage usages[1] = { p_usage };
	r_graph.add_driver_callback((RDD::DriverCallback)_test_upscalers_noop_callback, nullptr, VectorView<RenderingDeviceGraph::ResourceTracker *>(trackers, 1), VectorView<RenderingDeviceGraph::ResourceUsage>(usages, 1), p_stages);
}

TEST_CASE("[Upscalers][RDGraph] Driver callback stages") {
	RenderingDeviceGraph graph;
	RenderingDeviceGraph::ResourceTracker *tracker = graph.resource_tracker_create();
	tracker->buffer_driver_id = RDD::BufferID(1);

	BitField<RDD::PipelineStageBits> compute_stages = {};
	compute_stages.set_flag(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	BitField<RDD::PipelineStageBits> fragment_stages = {};
	fragment_stages.set_flag(RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	_add_test_callback(graph, tracker, RenderingDeviceGraph::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE, compute_stages);
	_add_test_callback(graph, tracker, RenderingDeviceGraph::RESOURCE_USAGE_STORAGE_BUFFER_READ, fragment_stages);

	BitField<RDD::PipelineStageBits> self = {};
	BitField<RDD::PipelineStageBits> previous = {};
	BitField<RDD::PipelineStageBits> next = {};
	graph.command_get_stages(1, self, previous, next);
	CHECK(self.has_flag(RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT));
	CHECK(previous.has_flag(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT));

	graph.command_get_stages(0, self, previous, next);
	CHECK(self.has_flag(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT));
	CHECK(next.has_flag(RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT));

	graph.resource_tracker_free(tracker);
}

TEST_CASE("[Upscalers][RDGraph] Driver callback default stages") {
	RenderingDeviceGraph graph;
	RenderingDeviceGraph::ResourceTracker *tracker = graph.resource_tracker_create();
	tracker->buffer_driver_id = RDD::BufferID(3);

	RenderingDeviceGraph::ResourceTracker *trackers[1] = { tracker };
	RenderingDeviceGraph::ResourceUsage usages[1] = { RenderingDeviceGraph::RESOURCE_USAGE_STORAGE_BUFFER_READ };
	graph.add_driver_callback((RDD::DriverCallback)_test_upscalers_noop_callback, nullptr, VectorView<RenderingDeviceGraph::ResourceTracker *>(trackers, 1), VectorView<RenderingDeviceGraph::ResourceUsage>(usages, 1), RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT);

	BitField<RDD::PipelineStageBits> self = {};
	BitField<RDD::PipelineStageBits> previous = {};
	BitField<RDD::PipelineStageBits> next = {};
	graph.command_get_stages(0, self, previous, next);
	CHECK(self.has_flag(RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT));

	graph.resource_tracker_free(tracker);
}
#endif // RD_ENABLED
