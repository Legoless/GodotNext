/**************************************************************************/
/*  tsr_async.h                                                           */
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

#include "core/typedefs.h"

// GodotNext: pt_tsr_async_compute (#193). Which TSR passes record on the async compute queue at
// each level of rendering/anti_aliasing/tsr/async_compute, and where in the frame. The levels
// follow the public description of UE's r.TSR.AsyncCompute (docs/godotnext/tsr-sources.md):
// 0 off; 1 only passes that read nothing this frame rendered; 2 also passes that read only the
// depth and velocity buffers; 3 every pass. TSR::process() takes every per-record-point decision
// from nextgi_tsr_point_role() and nextgi_tsr_point_passes(). Pure, so the doctests check the
// mapping and those decisions without a GPU.

// TSR::process records the passes in this order at every level and record point.
enum TSRPass : uint8_t {
	TSR_PASS_VELOCITY_DILATION,
	TSR_PASS_DISOCCLUSION,
	TSR_PASS_FLICKER_ANALYSIS,
	TSR_PASS_THIN_ANALYSIS,
	TSR_PASS_SHADING_REJECTION,
	TSR_PASS_HISTORY_UPDATE,
	TSR_PASS_REJECTION_AA,
	TSR_PASS_RESOLVE,
	TSR_PASS_DEBUG_VIEW,
	TSR_PASS_PERSISTENT_STORE,
	TSR_PASS_MAX,
};

enum TSRAsyncLevel : uint8_t {
	TSR_ASYNC_OFF = 0,
	TSR_ASYNC_HISTORY_CLEARS = 1,
	TSR_ASYNC_DEPTH_VELOCITY = 2,
	TSR_ASYNC_ALL = 3,
	TSR_ASYNC_MAX = 4,
};

inline constexpr int64_t TSR_ASYNC_DEFAULT_LEVEL = TSR_ASYNC_DEPTH_VELOCITY;

// The lowest level that moves a pass to the async queue: what it reads that this frame rendered.
// Velocity dilation reads the depth and velocity buffers; parallax disocclusion reads the depth,
// the dilated velocity and the previous frame's depth. Every other pass reads the scene color, or
// the history built from it. No pass reads only earlier frames (level 1): the history reset is a
// shader flag, and the clears when a texture is allocated are transfer commands, which the render
// graph keeps on the main queue. Level 1 therefore records as level 0.
inline constexpr TSRAsyncLevel nextgi_tsr_pass_async_level(TSRPass p_pass) {
	switch (p_pass) {
		case TSR_PASS_VELOCITY_DILATION:
		case TSR_PASS_DISOCCLUSION:
			return TSR_ASYNC_DEPTH_VELOCITY;
		default:
			return TSR_ASYNC_ALL;
	}
}

enum TSRRecordPoint : uint8_t {
	// Right after the motion-vector pass, where the depth and velocity buffers are final.
	TSR_RECORD_AFTER_MOTION_VECTORS,
	// The upscaler's place in the frame, after the transparent pass: where TSR records without async.
	TSR_RECORD_UPSCALE,
	TSR_RECORD_MAX,
};

struct TSRAsyncSchedule {
	uint32_t level = TSR_ASYNC_OFF; // The setting, clamped to 0-3.
	bool async_active = false; // This viewport may use the async queue (p_async_available below).
	// The depth and velocity passes record after the motion vectors (level 2 only).
	bool split = false;
	bool async[TSR_PASS_MAX] = {};
	TSRRecordPoint point[TSR_PASS_MAX] = {
		TSR_RECORD_UPSCALE,
		TSR_RECORD_UPSCALE,
		TSR_RECORD_UPSCALE,
		TSR_RECORD_UPSCALE,
		TSR_RECORD_UPSCALE,
		TSR_RECORD_UPSCALE,
		TSR_RECORD_UPSCALE,
		TSR_RECORD_UPSCALE,
		TSR_RECORD_UPSCALE,
		TSR_RECORD_UPSCALE,
	};

	// The first record point of the frame, which does TSR's once-per-frame work.
	constexpr TSRRecordPoint first_point() const { return split ? TSR_RECORD_AFTER_MOTION_VECTORS : TSR_RECORD_UPSCALE; }
	constexpr bool uses_async() const {
		for (uint32_t i = 0; i < TSR_PASS_MAX; i++) {
			if (async[i]) {
				return true;
			}
		}
		return false;
	}
};

static_assert(TSR_PASS_MAX == 10, "TSRAsyncSchedule::point lists one record point per pass.");

// p_level is rendering/anti_aliasing/tsr/async_compute. p_async_available is
// nextgi_tsr_async_available(): false without an active async queue, for a multiview (XR) viewport
// and for every TSR viewport of an RD frame after the first one that used the queue; every level
// then records exactly as level 0: every pass on the main queue, at the upscaler's place.
// p_split_allowed is true when nothing between the motion-vector pass and the upscaler can change
// the depth or velocity TSR reads (RenderForwardClustered: no MSAA resolve, no transparent surface
// that writes depth, no compositor effect in between, no VoxelGI or SDFGI debug draw). Without it,
// level 2 records as level 0: at the upscaler's place nothing on the main queue could overlap the
// two passes, so the async queue would only add a wait.
// Level 3 records every pass at the upscaler's place on the async queue. Splitting there too would
// put async work on both sides of the transparent pass, which needs two fork-join windows; the
// render graph has one per frame and would put the whole frame back on the main queue.
inline constexpr TSRAsyncSchedule nextgi_tsr_async_schedule(int64_t p_level, bool p_async_available, bool p_split_allowed) {
	TSRAsyncSchedule schedule;
	schedule.level = uint32_t(CLAMP(p_level, int64_t(TSR_ASYNC_OFF), int64_t(TSR_ASYNC_ALL)));
	schedule.async_active = p_async_available;
	if (!p_async_available) {
		return schedule;
	}
	if (schedule.level == TSR_ASYNC_ALL) {
		for (uint32_t i = 0; i < TSR_PASS_MAX; i++) {
			schedule.async[i] = true;
		}
		return schedule;
	}
	if (schedule.level == TSR_ASYNC_DEPTH_VELOCITY && p_split_allowed) {
		schedule.split = true;
		for (uint32_t i = 0; i < TSR_PASS_MAX; i++) {
			if (nextgi_tsr_pass_async_level(TSRPass(i)) <= TSR_ASYNC_DEPTH_VELOCITY) {
				schedule.async[i] = true;
				schedule.point[i] = TSR_RECORD_AFTER_MOTION_VECTORS;
			}
		}
	}
	return schedule;
}

// Whether a TSR viewport may use the async queue in this RenderingDevice frame. p_async_active is
// RD::is_async_compute_active() (RD::SUPPORTS_ASYNC_COMPUTE and
// rendering/rendering_device/async_compute). The render graph has one fork-join window per frame
// and puts the whole frame back on the main queue when it cannot keep it:
// - when one async part's consumer comes before another's producer, as when a second TSR viewport
//   samples the first one's output or follows it through a shared shadow atlas. So only the first
//   TSR viewport of each frame that uses the queue gets it: p_claimed_frame is the RD frame
//   (get_frames_drawn()) of the last viewport that did;
// - when an async list needs a layout transition before the fork on a texture whose other slice an
//   earlier async list of the frame used (#174), as the second view of a multiview (XR) viewport
//   does on the layered depth, velocity, color and TSR textures. So a viewport with more than one
//   view (p_view_count) records as level 0.
inline constexpr bool nextgi_tsr_async_available(bool p_async_active, uint32_t p_view_count, uint64_t p_rd_frame, uint64_t p_claimed_frame) {
	return p_async_active && p_view_count == 1 && p_rd_frame != p_claimed_frame;
}

inline constexpr uint32_t nextgi_tsr_pass_bit(TSRPass p_pass) {
	return 1u << uint32_t(p_pass);
}

// The passes a frame runs: the five core passes always, the others while their feature is on.
inline constexpr uint32_t nextgi_tsr_frame_passes(bool p_flicker, bool p_thin, bool p_rejection_aa, bool p_debug_view, bool p_persistent_store) {
	uint32_t passes = nextgi_tsr_pass_bit(TSR_PASS_VELOCITY_DILATION) | nextgi_tsr_pass_bit(TSR_PASS_DISOCCLUSION) | nextgi_tsr_pass_bit(TSR_PASS_SHADING_REJECTION) | nextgi_tsr_pass_bit(TSR_PASS_HISTORY_UPDATE) | nextgi_tsr_pass_bit(TSR_PASS_RESOLVE);
	passes |= p_flicker ? nextgi_tsr_pass_bit(TSR_PASS_FLICKER_ANALYSIS) : 0u;
	passes |= p_thin ? nextgi_tsr_pass_bit(TSR_PASS_THIN_ANALYSIS) : 0u;
	passes |= p_rejection_aa ? nextgi_tsr_pass_bit(TSR_PASS_REJECTION_AA) : 0u;
	passes |= p_debug_view ? nextgi_tsr_pass_bit(TSR_PASS_DEBUG_VIEW) : 0u;
	passes |= p_persistent_store ? nextgi_tsr_pass_bit(TSR_PASS_PERSISTENT_STORE) : 0u;
	return passes;
}

// What one TSR::process() call does besides its passes. A frame calls process() once per record
// point, in TSRRecordPoint order; a call whose point has neither role records nothing.
struct TSRPointRole {
	// The frame's first record point: the once-per-frame work (dimensions, the history reset, the
	// textures, the persistent frames, the flicker clock and the stability uniform buffer), all on
	// the main queue and before the frame's first async list.
	bool frame_begin = false;
	// The upscaler's place, the frame's last record point: advance_frame().
	bool frame_end = false;

	constexpr bool runs() const { return frame_begin || frame_end; }
};

inline constexpr TSRPointRole nextgi_tsr_point_role(const TSRAsyncSchedule &p_schedule, TSRRecordPoint p_point) {
	TSRPointRole role;
	role.frame_begin = p_point == p_schedule.first_point();
	role.frame_end = p_point == TSR_RECORD_UPSCALE;
	return role;
}

// The passes one TSR::process() call records, each on the queue it names.
struct TSRPointPasses {
	uint32_t record_mask = 0; // nextgi_tsr_pass_bit() of every pass recorded at this point.
	uint32_t async_mask = 0; // Those of them that record on the async compute queue.

	constexpr bool records(TSRPass p_pass) const { return (record_mask & nextgi_tsr_pass_bit(p_pass)) != 0; }
	constexpr bool runs_async(TSRPass p_pass) const { return (async_mask & nextgi_tsr_pass_bit(p_pass)) != 0; }
};

// p_frame_passes is nextgi_tsr_frame_passes(): TSR::process() records exactly the passes this
// returns, and nothing else.
inline constexpr TSRPointPasses nextgi_tsr_point_passes(const TSRAsyncSchedule &p_schedule, TSRRecordPoint p_point, uint32_t p_frame_passes) {
	TSRPointPasses passes;
	for (uint32_t i = 0; i < TSR_PASS_MAX; i++) {
		const uint32_t bit = nextgi_tsr_pass_bit(TSRPass(i));
		if ((p_frame_passes & bit) == 0 || p_schedule.point[i] != p_point) {
			continue;
		}
		passes.record_mask |= bit;
		if (p_schedule.async[i]) {
			passes.async_mask |= bit;
		}
	}
	return passes;
}

// GodotNext: rd_readonly_depth (#301). Tracks whether Forward+'s transparent render list writes
// depth or stencil. Any depth_draw_always surface modifies the depth buffer (requiring a
// read-write depth attachment and disabling TSR level 2's early split); any surface with stencil
// write flags requires a read-write depth-stencil attachment (even in AVBOIT composite, which
// suppresses depth writes), though depth itself stays unchanged for TSR.
struct ForwardTransparentDepthScan {
	bool any_depth_draw_always = false;
	bool any_stencil_write = false;

	constexpr void add_surface(bool p_depth_draw_always, bool p_stencil_write) {
		any_depth_draw_always = any_depth_draw_always || p_depth_draw_always;
		any_stencil_write = any_stencil_write || p_stencil_write;
	}

	constexpr bool tsr_split_depth_unchanged() const { return !any_depth_draw_always; }
	constexpr bool transparent_pass_read_only_depth() const { return !any_depth_draw_always && !any_stencil_write; }
	constexpr bool avboit_composite_read_only_depth() const { return !any_stencil_write; }
};
