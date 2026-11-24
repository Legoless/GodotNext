/**************************************************************************/
/*  d3d12_hooks.h                                                         */
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

// GodotNext: d3d12_interposer_hook (#120).
#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/callable.h"

#include <drivers/d3d12/godot_d3dx12.h>

struct IDXGISwapChain;

class D3D12Hooks {
private:
	static D3D12Hooks *singleton;

	// GodotNext: d3d12_interposer_hook (#120). Vendor-neutral hooks a module registers before the
	// RenderingServer starts. They are static, so they don't need a D3D12Hooks instance: a registered
	// instance also changes adapter and feature-level selection (XR).
	static String d3d12_library_override;
	static String dxgi_library_override;
	static Callable *present_callback;
	static void (*device_created_callback)(ID3D12Device *p_device, void *p_userdata);
	static void *device_created_userdata;
	static void (*device_destroying_callback)(ID3D12Device *p_device, void *p_userdata);
	static void *device_destroying_userdata;
	static SafeNumeric<uint32_t> registration_locks;

public:
	typedef void (*DeviceCreatedCallback)(ID3D12Device *p_device, void *p_userdata);
	typedef void (*DeviceDestroyingCallback)(ID3D12Device *p_device, void *p_userdata);
	D3D12Hooks();
	virtual ~D3D12Hooks();
	virtual D3D_FEATURE_LEVEL get_feature_level() const = 0;
	virtual LUID get_adapter_luid() const = 0;
	virtual void set_device(ID3D12Device *p_device) = 0;
	virtual void set_command_queue(ID3D12CommandQueue *p_queue) = 0;
	virtual void cleanup_device() = 0;
	static D3D12Hooks *get_singleton() { return singleton; }

	// GodotNext: d3d12_interposer_hook (#120).
	enum PresentStage {
		PRESENT_STAGE_BEFORE,
		PRESENT_STAGE_AFTER,
	};

	// Loads D3D12 and DXGI from these paths instead of the system's D3D12.dll and DXGI.dll. An empty
	// path keeps that library's default; two empty paths remove the override. Call it before the
	// RenderingServer starts (MODULE_INITIALIZATION_LEVEL_SERVERS): while a D3D12 rendering context is
	// initialized, it fails with ERR_LOCKED and changes nothing.
	static Error set_library_override(const String &p_d3d12_path, const String &p_dxgi_path);
	static bool has_library_override();
	static String get_d3d12_library_path();
	static String get_dxgi_library_path();

	// Calls p_callback immediately before and after each IDXGISwapChain::Present, with three
	// arguments: the IDXGISwapChain pointer as an int, the frame index (RenderingDevice's frames
	// drawn) and a PresentStage. An empty Callable removes it. Same registration rule as above.
	static Error set_present_callback(const Callable &p_callback);
	static bool has_present_callback() { return present_callback != nullptr; }
	static void call_present_callback(IDXGISwapChain *p_swap_chain, uint64_t p_frame_index, PresentStage p_stage);

	// Calls p_callback immediately after ID3D12Device creation (before command queues and swap chains
	// are created). Pass nullptr to clear. Same registration rule as above.
	static Error set_device_created_callback(DeviceCreatedCallback p_callback, void *p_userdata);
	static void call_device_created_callback(ID3D12Device *p_device);

	// Calls p_callback immediately before ID3D12Device teardown in ~RenderingDeviceDriverD3D12,
	// while the device is still alive and idle. Pass nullptr to clear. Same registration rule as above.
	static Error set_device_destroying_callback(DeviceDestroyingCallback p_callback, void *p_userdata);
	static void call_device_destroying_callback(ID3D12Device *p_device);

	// Called by RenderingContextDriverD3D12: registration stays closed while any context is initialized.
	static void lock_registration();
	static void unlock_registration();
	static bool is_registration_locked();
};
