/**************************************************************************/
/*  dlss_streamline_d3d12.h                                               */
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

// GodotNext: pu_streamline_reflex (#209), pu_dlss_fg (#200), pu_dlss_mfg (#201).
// Pure MIT header for Streamline lifecycle and NVIDIA Reflex control on Windows D3D12.
// Contains no Streamline SDK or D3D12 headers and no proprietary runtime filenames.

#include "core/string/ustring.h"
#include "core/variant/dictionary.h"

#include <cstdint>

class DLSSBackendMock;
class DLSSFrameGenBackendOps;

struct DLSSReflexResolution {
	int effective_mode = 0; // 0 = Off, 1 = On, 2 = On + Boost.
	String reason; // "OFF", "OK", "FORCED_ON_WITH_FG", "API_UNSUPPORTED", "REFLEX_UNAVAILABLE".
	uint32_t frame_limit_us = 0;
	bool forced_by_fg = false;
};

// Pure decision rule: should Streamline initialize before DisplayServer creation (#209 step 2)?
bool dlss_streamline_should_load(bool p_is_editor, const String &p_driver_name, const String &p_fg_provider, int p_reflex_mode);

// Pure Reflex state and frame-limiter resolution shared by runtime and mock doctests (#209 step 5, #201 step 3).
DLSSReflexResolution dlss_resolve_reflex_state(
		int p_requested_mode,
		bool p_fg_active,
		bool p_fg_dynamic,
		const String &p_api_name,
		bool p_streamline_loaded,
		bool p_low_latency_available,
		int p_max_fps);

// Reads dlss_runtime.json in p_dir, locates the interposer entry, and verifies its SHA-256 on disk.
bool dlss_streamline_verify_interposer_manifest(const String &p_dir, String &r_interposer_path, String &r_error);

// Lifecycle hooks called from register_types.cpp.
bool dlss_streamline_init_if_requested();
void dlss_streamline_shutdown();
bool dlss_streamline_is_loaded();
bool dlss_streamline_init_failed();

// Reflex state synchronization and status reporting.
void dlss_streamline_set_fg_active(bool p_active, bool p_dynamic);
void dlss_streamline_sync_reflex_options();
Dictionary dlss_streamline_get_latency_status();

// Returns the live Streamline DLSS-G backend ops (only when DLSS_STREAMLINE_ENABLED && D3D12_ENABLED).
DLSSFrameGenBackendOps *dlss_streamline_get_fg_ops();

// Test helpers (pure MIT, available in mock and ngx builds).
void dlss_streamline_install_mock_marker_callback(DLSSBackendMock *p_mock);
void dlss_streamline_reset_log_latches_for_tests();
