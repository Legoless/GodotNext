/**************************************************************************/
/*  dlss_ngx_d3d12.h                                                      */
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

// GodotNext: pu_dlss_d3d12 (#119), pu_rr_d3d12 (#204).
// Pure MIT header for the D3D12 DLSS backend and its cross-platform barrier-mode selection rule.
// Contains no Direct3D 12 or proprietary SDK headers.

#include "dlss_backend.h"

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "servers/rendering/rendering_device_driver.h"

enum DLSSD3D12BarrierPath {
	DLSS_D3D12_BARRIER_PATH_ENHANCED = 0,
	DLSS_D3D12_BARRIER_PATH_LEGACY = 1,
};

struct DLSSD3D12BarrierSelection {
	DLSSD3D12BarrierPath path = DLSS_D3D12_BARRIER_PATH_LEGACY;
	bool forced_legacy = false;
	const char *log_line = "";
};

// Pure D3D12 barrier-path selection rule, always compiled so unit tests can verify both
// enhanced and legacy paths (and the DEBUG_ENABLED force-legacy switch) on Linux and Windows.
inline DLSSD3D12BarrierSelection dlss_d3d12_select_barrier_path(
		bool p_device_enhanced_barriers,
		bool p_debug_enabled,
		const String &p_force_legacy_env,
		const String &p_legacy_env = String()) {
	const RDD::D3D12BarrierMode mode = RDD::d3d12_resolve_barrier_mode(
			p_device_enhanced_barriers,
			p_debug_enabled,
			p_force_legacy_env,
			p_legacy_env);
	DLSSD3D12BarrierSelection sel;
	sel.path = mode.enhanced_barriers ? DLSS_D3D12_BARRIER_PATH_ENHANCED : DLSS_D3D12_BARRIER_PATH_LEGACY;
	sel.forced_legacy = mode.forced_legacy;
	sel.log_line = mode.log_line;
	return sel;
}

using DLSSD3D12NativeUnwrapFn = void *(*)(void *);

#if defined(DLSS_BACKEND_NGX) && defined(D3D12_ENABLED)
DLSSBackend *dlss_backend_create_ngx_d3d12();

// Optional native-object unwrap hooks for D3D12 interposers (e.g. Streamline proxy device/command list).
void dlss_ngx_d3d12_set_native_device_unwrap(DLSSD3D12NativeUnwrapFn p_fn);
void dlss_ngx_d3d12_set_native_command_list_unwrap(DLSSD3D12NativeUnwrapFn p_fn);

// Convenience helpers for driver callbacks on D3D12: transition textures into SRV/UAV state on
// both enhanced and legacy barrier paths, then restore layouts and invalidate cached command-list state.
void dlss_ngx_d3d12_prepare_callback_textures(
		RenderingDeviceDriver *p_driver,
		RDD::CommandBufferID p_cmd_buffer,
		VectorView<RDD::TextureID> p_textures,
		VectorView<bool> p_read_write);

void dlss_ngx_d3d12_finish_callback_textures(
		RenderingDeviceDriver *p_driver,
		RDD::CommandBufferID p_cmd_buffer,
		VectorView<RDD::TextureID> p_textures = VectorView<RDD::TextureID>(),
		VectorView<bool> p_read_write = VectorView<bool>());
#endif
