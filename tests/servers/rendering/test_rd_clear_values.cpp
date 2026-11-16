/**************************************************************************/
/*  test_rd_clear_values.cpp                                              */
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

TEST_FORCE_LINK(test_rd_clear_values)

// Render-pass clear values with unused framebuffer slots (#275), without a GPU. RenderingDevice
// gives the graph one clear value per framebuffer slot; the driver's render pass and framebuffer
// hold only the slots in use, and every driver reads one clear value per attachment they hold.
// godotnext/testbed/tests/rd_parity/attachment_clears.tscn clears and reads back the same shapes
// on a GPU.

#ifdef RD_ENABLED

#include "servers/rendering/rendering_device_graph.h"

using RDG = RenderingDeviceGraph;

namespace TestRDClearValues {

// A value that tells the slots apart: color (p_tag, p_tag + 1, p_tag + 2, p_tag + 3).
RDD::RenderPassClearValue color_value(float p_tag) {
	RDD::RenderPassClearValue value;
	value.color = Color(p_tag, p_tag + 1.0f, p_tag + 2.0f, p_tag + 3.0f);
	return value;
}

RDD::RenderPassClearValue depth_value(float p_depth, uint32_t p_stencil) {
	RDD::RenderPassClearValue value;
	value.depth = p_depth;
	value.stencil = p_stencil;
	return value;
}

bool same_value(const RDD::RenderPassClearValue &p_a, const RDD::RenderPassClearValue &p_b) {
	return memcmp(&p_a, &p_b, sizeof(RDD::RenderPassClearValue)) == 0;
}

// A framebuffer cache shaped the way RenderingDevice::framebuffer_create_multipass() fills one:
// a tracker per used slot and a null one per unused slot, and a driver texture per used slot.
struct SlotFramebuffer {
	RDG::FramebufferCache cache;

	SlotFramebuffer(std::initializer_list<bool> p_used) {
		uint64_t texture = 1;
		for (bool used : p_used) {
			if (used) {
				cache.trackers.push_back(RDG::resource_tracker_create());
				cache.textures.push_back(RDD::TextureID(texture++));
			} else {
				cache.trackers.push_back(nullptr);
			}
		}
		cache.width = 64;
		cache.height = 32;
	}

	~SlotFramebuffer() {
		for (RDG::ResourceTracker *tracker : cache.trackers) {
			if (tracker != nullptr) {
				RDG::resource_tracker_free(tracker);
			}
		}
	}
};

// One clear value per slot: color_value(10 * slot), and for p_depth_slot a depth value.
LocalVector<RDD::RenderPassClearValue> slot_values(uint32_t p_slot_count, int32_t p_depth_slot = -1) {
	LocalVector<RDD::RenderPassClearValue> values;
	for (uint32_t i = 0; i < p_slot_count; i++) {
		values.push_back(int32_t(i) == p_depth_slot ? depth_value(0.25f + 0.125f * float(i), 7 + i) : color_value(10.0f * float(i)));
	}
	return values;
}

// The values of p_slots out of p_values, in order.
bool values_are(const LocalVector<RDD::RenderPassClearValue> &p_driver_values, const LocalVector<RDD::RenderPassClearValue> &p_values, std::initializer_list<uint32_t> p_slots) {
	if (p_driver_values.size() != p_slots.size()) {
		return false;
	}
	uint32_t i = 0;
	for (uint32_t slot : p_slots) {
		if (!same_value(p_driver_values[i++], p_values[slot])) {
			return false;
		}
	}
	return true;
}

LocalVector<RDD::RenderPassClearValue> driver_values(const SlotFramebuffer &p_framebuffer, const LocalVector<RDD::RenderPassClearValue> &p_values) {
	LocalVector<RDD::RenderPassClearValue> result;
	RDG::draw_list_driver_clear_values(&p_framebuffer.cache, p_values, result);
	return result;
}

} // namespace TestRDClearValues

using namespace TestRDClearValues;

TEST_CASE("[RD][ClearValues] The driver gets one clear value per attachment it holds, unused slots left out (#275)") {
	SUBCASE("No unused slot: every value, in order") {
		const SlotFramebuffer framebuffer{ true, true, true };
		const LocalVector<RDD::RenderPassClearValue> values = slot_values(3, 2);
		CHECK(values_are(driver_values(framebuffer, values), values, { 0, 1, 2 }));
	}
	SUBCASE("An unused slot before a color attachment and depth") {
		const SlotFramebuffer framebuffer{ true, false, true, true };
		const LocalVector<RDD::RenderPassClearValue> values = slot_values(4, 3);
		CHECK(values_are(driver_values(framebuffer, values), values, { 0, 2, 3 }));
	}
	SUBCASE("The #205 color pass without separate specular or velocity: two unused slots, two guides, depth") {
		const SlotFramebuffer framebuffer{ true, false, false, true, true, true };
		const LocalVector<RDD::RenderPassClearValue> values = slot_values(6, 5);
		CHECK(values_are(driver_values(framebuffer, values), values, { 0, 3, 4, 5 }));
	}
	SUBCASE("Unused first and last slots") {
		const SlotFramebuffer framebuffer{ false, true, false, true, false };
		const LocalVector<RDD::RenderPassClearValue> values = slot_values(5, 3);
		CHECK(values_are(driver_values(framebuffer, values), values, { 1, 3 }));
	}
	SUBCASE("Depth between an unused slot and a color attachment") {
		const SlotFramebuffer framebuffer{ true, true, false, true };
		const LocalVector<RDD::RenderPassClearValue> values = slot_values(4, 1);
		CHECK(values_are(driver_values(framebuffer, values), values, { 0, 1, 3 }));
	}
	SUBCASE("An empty framebuffer has no attachment and no clear value") {
		RDG::FramebufferCache cache;
		LocalVector<RDD::RenderPassClearValue> result;
		result.push_back(color_value(99.0f));
		RDG::draw_list_driver_clear_values(&cache, VectorView<RDD::RenderPassClearValue>(), result);
		CHECK(result.is_empty());
	}
}

TEST_CASE("[RD][ClearValues] Without a framebuffer cache every clear value is kept (#275)") {
	// A screen's render pass and framebuffer come from the swap chain, one attachment each.
	const LocalVector<RDD::RenderPassClearValue> values = slot_values(2);
	LocalVector<RDD::RenderPassClearValue> result;
	result.push_back(color_value(99.0f));
	RDG::draw_list_driver_clear_values(nullptr, values, result);
	CHECK(values_are(result, values, { 0, 1 }));
}

TEST_CASE("[RD][ClearValues] A recorded draw list clears every used slot and passes the driver's clear values (#275)") {
	// Slots: color (clears), unused, color (clears), color (loads), unused, depth (clears). The
	// load ops stay per slot (the render pass creation function takes them by slot).
	SlotFramebuffer framebuffer{ true, false, true, true, false, true };
	const LocalVector<RDD::RenderPassClearValue> values = slot_values(6, 5);
	const RDG::AttachmentOperation operations[] = {
		RDG::ATTACHMENT_OPERATION_CLEAR,
		RDG::ATTACHMENT_OPERATION_DEFAULT,
		RDG::ATTACHMENT_OPERATION_CLEAR,
		RDG::ATTACHMENT_OPERATION_DEFAULT,
		RDG::ATTACHMENT_OPERATION_DEFAULT,
		RDG::ATTACHMENT_OPERATION_CLEAR,
	};

	RDG graph;
	graph.add_draw_list_begin(&framebuffer.cache, Rect2i(0, 0, 64, 32), VectorView<RDG::AttachmentOperation>(operations, 6), values, RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	graph.add_draw_list_end();

	LocalVector<RDD::AttachmentLoadOp> load_ops;
	LocalVector<RDD::RenderPassClearValue> clear_values;
	graph.draw_list_attachments_for_test(0, load_ops, clear_values);
	REQUIRE_EQ(load_ops.size(), 6u);
	CHECK_EQ(load_ops[0], RDD::ATTACHMENT_LOAD_OP_CLEAR);
	CHECK_EQ(load_ops[1], RDD::ATTACHMENT_LOAD_OP_DONT_CARE);
	CHECK_EQ(load_ops[2], RDD::ATTACHMENT_LOAD_OP_CLEAR);
	CHECK_EQ(load_ops[3], RDD::ATTACHMENT_LOAD_OP_LOAD);
	CHECK_EQ(load_ops[4], RDD::ATTACHMENT_LOAD_OP_DONT_CARE);
	// Depth sits past the last clear value's index; it still clears.
	CHECK_EQ(load_ops[5], RDD::ATTACHMENT_LOAD_OP_CLEAR);
	CHECK(values_are(clear_values, values, { 0, 2, 3, 5 }));
}

#endif // RD_ENABLED
