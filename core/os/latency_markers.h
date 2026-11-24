/**************************************************************************/
/*  latency_markers.h                                                     */
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

// GodotNext: pu_streamline_reflex (#209, plan §21.5 U5).
// Vendor-neutral PC-latency marker seam. Emitting a marker is a single atomic
// pointer load and branch-not-taken when no callback is registered.

#include <atomic>
#include <cstdint>

enum LatencyMarker {
	LATENCY_MARKER_SLEEP = 0,
	LATENCY_MARKER_SIMULATION_START,
	LATENCY_MARKER_SIMULATION_END,
	LATENCY_MARKER_INPUT_SAMPLE,
	LATENCY_MARKER_RENDER_SUBMIT_START,
	LATENCY_MARKER_RENDER_SUBMIT_END,
	LATENCY_MARKER_PRESENT_START,
	LATENCY_MARKER_PRESENT_END,
};

typedef void (*LatencyMarkerCallback)(LatencyMarker p_marker, uint64_t p_frame_id);

namespace LatencyMarkers {

extern std::atomic<LatencyMarkerCallback> callback;
extern std::atomic<uint64_t> current_frame_id;
extern std::atomic<uint64_t> render_frame_id;
extern std::atomic<uint64_t> last_input_sample_frame_id;

} // namespace LatencyMarkers

inline void set_latency_marker_callback(LatencyMarkerCallback p_callback) {
	LatencyMarkers::callback.store(p_callback, std::memory_order_release);
}

inline LatencyMarkerCallback get_latency_marker_callback() {
	return LatencyMarkers::callback.load(std::memory_order_acquire);
}

inline uint64_t begin_latency_frame() {
	return LatencyMarkers::current_frame_id.fetch_add(1, std::memory_order_acq_rel) + 1;
}

inline uint64_t get_latency_frame_id() {
	return LatencyMarkers::current_frame_id.load(std::memory_order_acquire);
}

inline void set_render_latency_frame_id(uint64_t p_frame_id) {
	LatencyMarkers::render_frame_id.store(p_frame_id, std::memory_order_release);
}

inline uint64_t get_render_latency_frame_id() {
	const uint64_t rf = LatencyMarkers::render_frame_id.load(std::memory_order_acquire);
	return rf != 0 ? rf : get_latency_frame_id();
}

inline void emit_latency_marker(LatencyMarker p_marker, uint64_t p_frame_id) {
	const LatencyMarkerCallback cb = LatencyMarkers::callback.load(std::memory_order_acquire);
	if (cb != nullptr) {
		cb(p_marker, p_frame_id);
	}
}

// Emits LATENCY_MARKER_INPUT_SAMPLE at most once per frame ID so multiple
// Input::flush_buffered_events() calls in a frame never double-emit.
inline void emit_latency_input_sample_once(uint64_t p_frame_id) {
	const LatencyMarkerCallback cb = LatencyMarkers::callback.load(std::memory_order_acquire);
	if (cb == nullptr) {
		return;
	}
	uint64_t prev = LatencyMarkers::last_input_sample_frame_id.load(std::memory_order_relaxed);
	while (prev != p_frame_id) {
		if (LatencyMarkers::last_input_sample_frame_id.compare_exchange_weak(prev, p_frame_id, std::memory_order_acq_rel, std::memory_order_relaxed)) {
			cb(LATENCY_MARKER_INPUT_SAMPLE, p_frame_id);
			return;
		}
	}
}

inline void reset_latency_markers_for_tests() {
	LatencyMarkers::callback.store(nullptr, std::memory_order_release);
	LatencyMarkers::current_frame_id.store(0, std::memory_order_release);
	LatencyMarkers::render_frame_id.store(0, std::memory_order_release);
	LatencyMarkers::last_input_sample_frame_id.store(UINT64_MAX, std::memory_order_release);
}
