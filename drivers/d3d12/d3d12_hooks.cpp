/**************************************************************************/
/*  d3d12_hooks.cpp                                                       */
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

#include "d3d12_hooks.h"

#include "core/error/error_macros.h"
#include "core/os/memory.h"
#include "core/variant/variant.h"

D3D12Hooks *D3D12Hooks::singleton = nullptr;

// GodotNext: d3d12_interposer_hook (#120).
String D3D12Hooks::d3d12_library_override;
String D3D12Hooks::dxgi_library_override;
Callable *D3D12Hooks::present_callback = nullptr;
D3D12Hooks::DeviceCreatedCallback D3D12Hooks::device_created_callback = nullptr;
void *D3D12Hooks::device_created_userdata = nullptr;
D3D12Hooks::DeviceDestroyingCallback D3D12Hooks::device_destroying_callback = nullptr;
void *D3D12Hooks::device_destroying_userdata = nullptr;
SafeNumeric<uint32_t> D3D12Hooks::registration_locks;

D3D12Hooks::D3D12Hooks() {
	if (singleton == nullptr) {
		singleton = this;
	}
}

D3D12Hooks::~D3D12Hooks() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

// GodotNext: d3d12_interposer_hook (#120).

Error D3D12Hooks::set_library_override(const String &p_d3d12_path, const String &p_dxgi_path) {
	ERR_FAIL_COND_V_MSG(is_registration_locked(), ERR_LOCKED, "D3D12Hooks::set_library_override() must be called before the D3D12 rendering context initializes (before RenderingServer init).");
	d3d12_library_override = p_d3d12_path;
	dxgi_library_override = p_dxgi_path;
	return OK;
}

bool D3D12Hooks::has_library_override() {
	return !d3d12_library_override.is_empty() || !dxgi_library_override.is_empty();
}

String D3D12Hooks::get_d3d12_library_path() {
	return d3d12_library_override.is_empty() ? String("D3D12.dll") : d3d12_library_override;
}

String D3D12Hooks::get_dxgi_library_path() {
	return dxgi_library_override.is_empty() ? String("DXGI.dll") : dxgi_library_override;
}

Error D3D12Hooks::set_present_callback(const Callable &p_callback) {
	ERR_FAIL_COND_V_MSG(is_registration_locked(), ERR_LOCKED, "D3D12Hooks::set_present_callback() must be called before the D3D12 rendering context initializes (before RenderingServer init).");
	// A pointer, not a static Callable: it must not be destroyed after the core types at exit.
	if (present_callback != nullptr) {
		memdelete(present_callback);
		present_callback = nullptr;
	}
	if (!p_callback.is_null()) {
		present_callback = memnew(Callable(p_callback));
	}
	return OK;
}

void D3D12Hooks::call_present_callback(IDXGISwapChain *p_swap_chain, uint64_t p_frame_index, PresentStage p_stage) {
	if (present_callback == nullptr) {
		return;
	}
	const Variant swap_chain = (uint64_t)(uintptr_t)p_swap_chain;
	const Variant frame_index = p_frame_index;
	const Variant stage = (int)p_stage;
	const Variant *args[3] = { &swap_chain, &frame_index, &stage };
	Variant ret;
	Callable::CallError ce;
	present_callback->callp(args, 3, ret, ce);
	if (ce.error != Callable::CallError::CALL_OK) {
		ERR_PRINT_ONCE("D3D12Hooks: the present callback failed: " + Variant::get_callable_error_text(*present_callback, args, 3, ce) + ".");
	}
}

Error D3D12Hooks::set_device_created_callback(DeviceCreatedCallback p_callback, void *p_userdata) {
	ERR_FAIL_COND_V_MSG(is_registration_locked(), ERR_LOCKED, "D3D12Hooks::set_device_created_callback() must be called before the D3D12 rendering context initializes (before RenderingServer init).");
	device_created_callback = p_callback;
	device_created_userdata = p_userdata;
	return OK;
}

void D3D12Hooks::call_device_created_callback(ID3D12Device *p_device) {
	if (device_created_callback != nullptr && p_device != nullptr) {
		device_created_callback(p_device, device_created_userdata);
	}
}

Error D3D12Hooks::set_device_destroying_callback(DeviceDestroyingCallback p_callback, void *p_userdata) {
	ERR_FAIL_COND_V_MSG(is_registration_locked(), ERR_LOCKED, "D3D12Hooks::set_device_destroying_callback() must be called before the D3D12 rendering context initializes (before RenderingServer init).");
	device_destroying_callback = p_callback;
	device_destroying_userdata = p_userdata;
	return OK;
}

void D3D12Hooks::call_device_destroying_callback(ID3D12Device *p_device) {
	if (device_destroying_callback != nullptr && p_device != nullptr) {
		device_destroying_callback(p_device, device_destroying_userdata);
	}
}

void D3D12Hooks::lock_registration() {
	registration_locks.increment();
}

void D3D12Hooks::unlock_registration() {
	ERR_FAIL_COND(registration_locks.get() == 0);
	registration_locks.decrement();
}

bool D3D12Hooks::is_registration_locked() {
	return registration_locks.get() > 0;
}
