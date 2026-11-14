/**************************************************************************/
/*  godotnext_d3d12_stub.cpp                                              */
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

// GodotNext: d3d12_interposer_hook (#120). A stub D3D12 and DXGI interposer for the D3D12Hooks
// tests, built only with the tests (tests/SCsub). D3D12Hooks::set_library_override() points both
// libraries at this one DLL. Each export Godot resolves with GetProcAddress forwards to the system
// library and counts the call. godotnext_d3d12_stub_present() is called from a present callback
// before and after each Present and reads the swap chain's own present count, so the stub counts
// the Present calls the swap chain saw for each frame. Nothing here is vendor-specific.

#include <windows.h>

#include <dxgi.h>

#include <cstdint>
#include <cstring>
#include <cwchar>

namespace {

enum Counter {
	COUNTER_CREATE_DXGI_FACTORY,
	COUNTER_CREATE_DXGI_FACTORY1,
	COUNTER_CREATE_DXGI_FACTORY2,
	COUNTER_DXGI_GET_DEBUG_INTERFACE1,
	COUNTER_DXGI_DECLARE_ADAPTER_REMOVAL_SUPPORT,
	COUNTER_D3D12_CREATE_DEVICE,
	COUNTER_D3D12_GET_DEBUG_INTERFACE,
	COUNTER_D3D12_GET_INTERFACE,
	COUNTER_D3D12_ENABLE_EXPERIMENTAL_FEATURES,
	COUNTER_D3D12_SERIALIZE_ROOT_SIGNATURE,
	COUNTER_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE,
	COUNTER_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER,
	COUNTER_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER,
	// Present tally, from godotnext_d3d12_stub_present().
	COUNTER_PRESENT_FRAMES, // Frames with a before/after pair.
	COUNTER_PRESENT_CALLS, // Present calls the swap chains counted between before and after.
	COUNTER_PRESENT_MISMATCHES, // Pairs that saw a count other than 1, or that were out of order.
	COUNTER_MAX,
};

const char *counter_names[COUNTER_MAX] = {
	"CreateDXGIFactory",
	"CreateDXGIFactory1",
	"CreateDXGIFactory2",
	"DXGIGetDebugInterface1",
	"DXGIDeclareAdapterRemovalSupport",
	"D3D12CreateDevice",
	"D3D12GetDebugInterface",
	"D3D12GetInterface",
	"D3D12EnableExperimentalFeatures",
	"D3D12SerializeRootSignature",
	"D3D12SerializeVersionedRootSignature",
	"D3D12CreateRootSignatureDeserializer",
	"D3D12CreateVersionedRootSignatureDeserializer",
	"present_frames",
	"present_calls",
	"present_mismatches",
};

volatile LONG64 counters[COUNTER_MAX] = {};

enum SystemLibrary {
	SYSTEM_LIBRARY_D3D12,
	SYSTEM_LIBRARY_DXGI,
	SYSTEM_LIBRARY_MAX,
};

HMODULE system_libraries[SYSTEM_LIBRARY_MAX] = {};

// Loads the system library by its full System32 path on first use, never from DllMain.
HMODULE get_system_library(SystemLibrary p_library) {
	HMODULE module = (HMODULE)InterlockedCompareExchangePointer((PVOID volatile *)&system_libraries[p_library], nullptr, nullptr);
	if (module != nullptr) {
		return module;
	}

	wchar_t path[MAX_PATH] = {};
	const UINT length = GetSystemDirectoryW(path, MAX_PATH);
	const wchar_t *name = p_library == SYSTEM_LIBRARY_D3D12 ? L"\\d3d12.dll" : L"\\dxgi.dll";
	if (length == 0 || length + wcslen(name) >= MAX_PATH) {
		return nullptr;
	}
	wcscat_s(path, MAX_PATH, name);

	module = LoadLibraryW(path);
	if (module == nullptr) {
		return nullptr;
	}
	HMODULE previous = (HMODULE)InterlockedCompareExchangePointer((PVOID volatile *)&system_libraries[p_library], module, nullptr);
	if (previous != nullptr) {
		// Another thread got there first; keep one reference.
		FreeLibrary(module);
		return previous;
	}
	return module;
}

// Counts the call and returns the system library's export of the same name, or null.
template <typename T>
T forward(SystemLibrary p_library, Counter p_counter, const char *p_name) {
	InterlockedIncrement64(&counters[p_counter]);
	HMODULE module = get_system_library(p_library);
	if (module == nullptr) {
		return nullptr;
	}
	return (T)(void *)GetProcAddress(module, p_name);
}

// The present tally. Calls come in before/after pairs on the thread that presents.
SRWLOCK present_lock = SRWLOCK_INIT;
bool present_pending = false;
void *present_pending_swap_chain = nullptr;
uint64_t present_pending_frame = 0;
UINT present_pending_count = 0;
bool present_have_last = false;
void *present_last_swap_chain = nullptr;
uint64_t present_last_frame = 0;

UINT get_present_count(void *p_swap_chain) {
	UINT count = 0;
	if (FAILED(static_cast<IDXGISwapChain *>(p_swap_chain)->GetLastPresentCount(&count))) {
		return 0;
	}
	return count;
}

} // namespace

// D3D12 exports.

extern "C" HRESULT WINAPI stub_D3D12CreateDevice(IUnknown *p_adapter, int p_minimum_feature_level, REFIID p_riid, void **r_device) {
	typedef HRESULT(WINAPI * Fn)(IUnknown *, int, REFIID, void **);
	Fn fn = forward<Fn>(SYSTEM_LIBRARY_D3D12, COUNTER_D3D12_CREATE_DEVICE, "D3D12CreateDevice");
	return fn != nullptr ? fn(p_adapter, p_minimum_feature_level, p_riid, r_device) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI stub_D3D12GetDebugInterface(REFIID p_riid, void **r_debug) {
	typedef HRESULT(WINAPI * Fn)(REFIID, void **);
	Fn fn = forward<Fn>(SYSTEM_LIBRARY_D3D12, COUNTER_D3D12_GET_DEBUG_INTERFACE, "D3D12GetDebugInterface");
	return fn != nullptr ? fn(p_riid, r_debug) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI stub_D3D12GetInterface(REFCLSID p_clsid, REFIID p_riid, void **r_interface) {
	typedef HRESULT(WINAPI * Fn)(REFCLSID, REFIID, void **);
	Fn fn = forward<Fn>(SYSTEM_LIBRARY_D3D12, COUNTER_D3D12_GET_INTERFACE, "D3D12GetInterface");
	return fn != nullptr ? fn(p_clsid, p_riid, r_interface) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI stub_D3D12EnableExperimentalFeatures(UINT p_count, const IID *p_iids, void *p_configs, UINT *p_config_sizes) {
	typedef HRESULT(WINAPI * Fn)(UINT, const IID *, void *, UINT *);
	Fn fn = forward<Fn>(SYSTEM_LIBRARY_D3D12, COUNTER_D3D12_ENABLE_EXPERIMENTAL_FEATURES, "D3D12EnableExperimentalFeatures");
	return fn != nullptr ? fn(p_count, p_iids, p_configs, p_config_sizes) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI stub_D3D12SerializeRootSignature(const void *p_desc, int p_version, void **r_blob, void **r_error_blob) {
	typedef HRESULT(WINAPI * Fn)(const void *, int, void **, void **);
	Fn fn = forward<Fn>(SYSTEM_LIBRARY_D3D12, COUNTER_D3D12_SERIALIZE_ROOT_SIGNATURE, "D3D12SerializeRootSignature");
	return fn != nullptr ? fn(p_desc, p_version, r_blob, r_error_blob) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI stub_D3D12SerializeVersionedRootSignature(const void *p_desc, void **r_blob, void **r_error_blob) {
	typedef HRESULT(WINAPI * Fn)(const void *, void **, void **);
	Fn fn = forward<Fn>(SYSTEM_LIBRARY_D3D12, COUNTER_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE, "D3D12SerializeVersionedRootSignature");
	return fn != nullptr ? fn(p_desc, r_blob, r_error_blob) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI stub_D3D12CreateRootSignatureDeserializer(const void *p_data, SIZE_T p_size, REFIID p_riid, void **r_deserializer) {
	typedef HRESULT(WINAPI * Fn)(const void *, SIZE_T, REFIID, void **);
	Fn fn = forward<Fn>(SYSTEM_LIBRARY_D3D12, COUNTER_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER, "D3D12CreateRootSignatureDeserializer");
	return fn != nullptr ? fn(p_data, p_size, p_riid, r_deserializer) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI stub_D3D12CreateVersionedRootSignatureDeserializer(const void *p_data, SIZE_T p_size, REFIID p_riid, void **r_deserializer) {
	typedef HRESULT(WINAPI * Fn)(const void *, SIZE_T, REFIID, void **);
	Fn fn = forward<Fn>(SYSTEM_LIBRARY_D3D12, COUNTER_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER, "D3D12CreateVersionedRootSignatureDeserializer");
	return fn != nullptr ? fn(p_data, p_size, p_riid, r_deserializer) : E_NOTIMPL;
}

// DXGI exports.

extern "C" HRESULT WINAPI stub_CreateDXGIFactory(REFIID p_riid, void **r_factory) {
	typedef HRESULT(WINAPI * Fn)(REFIID, void **);
	Fn fn = forward<Fn>(SYSTEM_LIBRARY_DXGI, COUNTER_CREATE_DXGI_FACTORY, "CreateDXGIFactory");
	return fn != nullptr ? fn(p_riid, r_factory) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI stub_CreateDXGIFactory1(REFIID p_riid, void **r_factory) {
	typedef HRESULT(WINAPI * Fn)(REFIID, void **);
	Fn fn = forward<Fn>(SYSTEM_LIBRARY_DXGI, COUNTER_CREATE_DXGI_FACTORY1, "CreateDXGIFactory1");
	return fn != nullptr ? fn(p_riid, r_factory) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI stub_CreateDXGIFactory2(UINT p_flags, REFIID p_riid, void **r_factory) {
	typedef HRESULT(WINAPI * Fn)(UINT, REFIID, void **);
	Fn fn = forward<Fn>(SYSTEM_LIBRARY_DXGI, COUNTER_CREATE_DXGI_FACTORY2, "CreateDXGIFactory2");
	return fn != nullptr ? fn(p_flags, p_riid, r_factory) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI stub_DXGIGetDebugInterface1(UINT p_flags, REFIID p_riid, void **r_debug) {
	typedef HRESULT(WINAPI * Fn)(UINT, REFIID, void **);
	Fn fn = forward<Fn>(SYSTEM_LIBRARY_DXGI, COUNTER_DXGI_GET_DEBUG_INTERFACE1, "DXGIGetDebugInterface1");
	return fn != nullptr ? fn(p_flags, p_riid, r_debug) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI stub_DXGIDeclareAdapterRemovalSupport() {
	typedef HRESULT(WINAPI * Fn)();
	Fn fn = forward<Fn>(SYSTEM_LIBRARY_DXGI, COUNTER_DXGI_DECLARE_ADAPTER_REMOVAL_SUPPORT, "DXGIDeclareAdapterRemovalSupport");
	return fn != nullptr ? fn() : E_NOTIMPL;
}

// Test exports.

// Returns the counter with this name (an export name, or present_frames, present_calls or
// present_mismatches), or UINT64_MAX for an unknown name.
extern "C" uint64_t stub_count(const char *p_name) {
	for (int i = 0; i < COUNTER_MAX; i++) {
		if (strcmp(p_name, counter_names[i]) == 0) {
			return (uint64_t)InterlockedCompareExchange64(&counters[i], 0, 0);
		}
	}
	return UINT64_MAX;
}

// p_stage is 0 immediately before Present and 1 immediately after it (D3D12Hooks::PresentStage).
extern "C" void stub_present(void *p_swap_chain, uint64_t p_frame_index, int p_stage) {
	if (p_swap_chain == nullptr) {
		InterlockedIncrement64(&counters[COUNTER_PRESENT_MISMATCHES]);
		return;
	}
	const UINT count = get_present_count(p_swap_chain);

	AcquireSRWLockExclusive(&present_lock);
	if (p_stage == 0) {
		if (present_pending) {
			// The previous before had no after.
			InterlockedIncrement64(&counters[COUNTER_PRESENT_MISMATCHES]);
		}
		present_pending = true;
		present_pending_swap_chain = p_swap_chain;
		present_pending_frame = p_frame_index;
		present_pending_count = count;
	} else {
		if (!present_pending || present_pending_swap_chain != p_swap_chain || present_pending_frame != p_frame_index) {
			InterlockedIncrement64(&counters[COUNTER_PRESENT_MISMATCHES]);
		} else {
			const UINT presents = count - present_pending_count;
			InterlockedAdd64(&counters[COUNTER_PRESENT_CALLS], presents);
			InterlockedIncrement64(&counters[COUNTER_PRESENT_FRAMES]);
			// Exactly one Present per frame and swap chain, and frames only move forward.
			const bool repeated = present_have_last && present_last_swap_chain == p_swap_chain && present_last_frame >= p_frame_index;
			if (presents != 1 || repeated) {
				InterlockedIncrement64(&counters[COUNTER_PRESENT_MISMATCHES]);
			}
			present_have_last = true;
			present_last_swap_chain = p_swap_chain;
			present_last_frame = p_frame_index;
		}
		present_pending = false;
	}
	ReleaseSRWLockExclusive(&present_lock);
}
