/**************************************************************************/
/*  test_nextgi_front_layer.cpp                                           */
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

TEST_FORCE_LINK(test_nextgi_front_layer)

#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_front_layer.h"
#include "servers/rendering/renderer_rd/environment/next_gi_gather.h"

TEST_CASE("[NextGI][FrontLayer] Fragment visibility and nearest-layer matching") {
	CHECK_FALSE(RendererRD::nextgi_front_layer_fragment_visible(0.09f, 0.5f, 0.8f));
	CHECK(RendererRD::nextgi_front_layer_fragment_visible(0.1f, 0.8f, 0.8f));
	CHECK_FALSE(RendererRD::nextgi_front_layer_fragment_visible(0.5f, 0.79f, 0.8f));
	CHECK(RendererRD::nextgi_front_layer_depth_matches(5.0f, 5.00005f));
	CHECK_FALSE(RendererRD::nextgi_front_layer_depth_matches(5.0f, 5.0002f));
}

TEST_CASE("[NextGI][FrontLayer] Allow and environment flags gate lazy resources") {
	CHECK(RendererRD::nextgi_front_layer_allowed(true, true, true));
	CHECK_FALSE(RendererRD::nextgi_front_layer_allowed(false, true, true));
	CHECK_FALSE(RendererRD::nextgi_front_layer_allowed(true, false, true));
	CHECK_FALSE(RendererRD::nextgi_front_layer_allowed(true, true, false));

	RendererRD::NextGIFrontLayer layer;
	Dictionary info;
	layer.fill_info(info);
	CHECK_FALSE(bool(info["front_layer_targets_created"]));
	CHECK_EQ(int(info["front_layer_shader_count"]), 0);
	CHECK_EQ(int(info["front_layer_dispatch_count"]), 0);
	// GodotNext: nextgi_front_layer_hit (#276). No trace yet, so no trace size and no hit target.
	// A missing key would read as 0, so check that the keys are reported first.
	CHECK(info.has("front_layer_trace_width"));
	CHECK(info.has("front_layer_trace_height"));
	CHECK(info.has("front_layer_hit_distance_width"));
	CHECK(info.has("front_layer_hit_distance_height"));
	CHECK_EQ(int(info["front_layer_trace_width"]), 0);
	CHECK_EQ(int(info["front_layer_trace_height"]), 0);
	CHECK_EQ(int(info["front_layer_hit_distance_width"]), 0);
	CHECK_EQ(int(info["front_layer_hit_distance_height"]), 0);
}

// GodotNext: nextgi_front_layer_hit (#276).
TEST_CASE("[NextGI][FrontLayer] The front layer's trace writes its own hit distances, never the view's") {
	const RID front_target = RID::from_uint64(11);
	const RID view_target = RID::from_uint64(22);
	// The view's gather writes and reads the view's RB_TEX_SPECULAR_HIT.
	CHECK_EQ(RendererRD::nextgi_specular_hit_target(false, RID(), view_target), view_target);
	CHECK_EQ(RendererRD::nextgi_specular_hit_target(false, front_target, view_target), view_target);
	CHECK_FALSE(RendererRD::nextgi_specular_hit_target(false, front_target, RID()).is_valid());
	// The front layer's gather uses its own target while the view's texture exists, and does not
	// fall back to the view's texture when its own is missing.
	CHECK_EQ(RendererRD::nextgi_specular_hit_target(true, front_target, view_target), front_target);
	CHECK_FALSE(RendererRD::nextgi_specular_hit_target(true, RID(), view_target).is_valid());

	// A gather that never traced allocated no front-layer target.
	RendererRD::NextGIGather gather;
	CHECK_EQ(gather.get_front_hit_distance_size(), Size2i());
}

TEST_CASE("[NextGI][FrontLayer] Counters unregister with the owner") {
	RendererRD::NextGI next_gi;
	{
		RendererRD::NextGIFrontLayer layer;
		layer.set_owner(&next_gi);
		const Dictionary info = next_gi.get_tier_info();
		CHECK(info.has("front_layer_shader_count"));
		CHECK(info.has("front_layer_independent_history"));
		CHECK(next_gi.has_front_layer());
	}
	CHECK_FALSE(next_gi.has_front_layer());
}
