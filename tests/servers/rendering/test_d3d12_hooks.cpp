/**************************************************************************/
/*  test_d3d12_hooks.cpp                                                  */
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

TEST_FORCE_LINK(test_d3d12_hooks)

// GodotNext: d3d12_interposer_hook (#120). D3D12Hooks library override and present callback.
// No case needs a GPU or a window: the rendering-context cases only create a DXGI factory and
// enumerate adapters, which works on any Windows machine. Present itself is checked in rendered
// runs through d3d12_hooks_stub_install() (see test_d3d12_hooks.h).

#ifdef D3D12_ENABLED

#include "test_d3d12_hooks.h"

#include "core/config/engine.h"
#include "core/error/error_macros.h"
#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/local_vector.h"
#include "drivers/d3d12/d3d12_hooks.h"
#include "drivers/d3d12/rendering_context_driver_d3d12.h"

namespace TestD3D12Hooks {

typedef uint64_t (*StubCountFunc)(const char *p_name);
typedef void (*StubPresentFunc)(void *p_swap_chain, uint64_t p_frame_index, int p_stage);

// The stub interposer DLL that tests/SCsub builds next to the editor binary.
String stub_path() {
	return OS::get_singleton()->get_executable_path().get_base_dir().path_join("godotnext_d3d12_stub." + Engine::get_singleton()->get_architecture_name() + ".dll");
}

HMODULE find_loaded_module(const String &p_path) {
	return GetModuleHandleW((LPCWSTR)(p_path.replace_char('/', '\\').utf16().get_data()));
}

// Records the errors printed while it is alive.
struct ErrorCapture {
	ErrorHandlerList handler;
	int count = 0;
	String first_message;
	String last_message;

	ErrorCapture() {
		handler.errfunc = _capture;
		handler.userdata = this;
		add_error_handler(&handler);
	}

	~ErrorCapture() {
		remove_error_handler(&handler);
	}

	static void _capture(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
		ErrorCapture *self = static_cast<ErrorCapture *>(p_self);
		self->last_message = String::utf8((p_message != nullptr && *p_message != 0) ? p_message : p_error);
		if (self->count == 0) {
			self->first_message = self->last_message;
		}
		self->count++;
	}
};

// Counts the printed lines that contain a string.
struct PrintCapture {
	PrintHandlerList handler;
	String needle;
	int count = 0;
	String last_line;

	explicit PrintCapture(const String &p_needle) {
		needle = p_needle;
		handler.printfunc = _capture;
		handler.userdata = this;
		add_print_handler(&handler);
	}

	~PrintCapture() {
		remove_print_handler(&handler);
	}

	static void _capture(void *p_self, const String &p_string, bool p_error, bool p_rich) {
		PrintCapture *self = static_cast<PrintCapture *>(p_self);
		if (p_string.contains(self->needle)) {
			self->count++;
			self->last_line = p_string;
		}
	}
};

// Clears the registered hooks when a case starts and when it ends.
struct HookReset {
	HookReset() { reset(); }
	~HookReset() { reset(); }
	static void reset() {
		D3D12Hooks::set_library_override(String(), String());
		D3D12Hooks::set_present_callback(Callable());
		D3D12Hooks::set_device_created_callback(nullptr, nullptr);
		D3D12Hooks::set_device_destroying_callback(nullptr, nullptr);
	}
};

// Stands in for an initialized rendering context.
struct RegistrationLock {
	RegistrationLock() { D3D12Hooks::lock_registration(); }
	~RegistrationLock() { D3D12Hooks::unlock_registration(); }
};

struct PresentRecord {
	uint64_t swap_chain = 0;
	uint64_t frame_index = 0;
	int stage = -1;
};

LocalVector<PresentRecord> present_records;

void record_present(uint64_t p_swap_chain, uint64_t p_frame_index, int p_stage) {
	PresentRecord record;
	record.swap_chain = p_swap_chain;
	record.frame_index = p_frame_index;
	record.stage = p_stage;
	present_records.push_back(record);
}

IDXGISwapChain *fake_swap_chain() {
	return reinterpret_cast<IDXGISwapChain *>(uintptr_t(0x1000));
}

ID3D12Device *fake_d3d12_device() {
	return reinterpret_cast<ID3D12Device *>(uintptr_t(0x2000));
}

struct DeviceCreatedCapture {
	int count = 0;
	ID3D12Device *last_device = nullptr;
};

void record_device_created(ID3D12Device *p_device, void *p_userdata) {
	DeviceCreatedCapture *cap = static_cast<DeviceCreatedCapture *>(p_userdata);
	cap->count++;
	cap->last_device = p_device;
}

TEST_CASE("[D3D12Hooks] Without registration, D3D12 and DXGI are the system libraries and no present callback runs") {
	HookReset reset;
	present_records.clear();

	CHECK_FALSE(D3D12Hooks::is_registration_locked());
	CHECK_FALSE(D3D12Hooks::has_library_override());
	CHECK(D3D12Hooks::get_d3d12_library_path() == "D3D12.dll");
	CHECK(D3D12Hooks::get_dxgi_library_path() == "DXGI.dll");
	CHECK_FALSE(D3D12Hooks::has_present_callback());

	D3D12Hooks::call_present_callback(fake_swap_chain(), 7, D3D12Hooks::PRESENT_STAGE_BEFORE);
	D3D12Hooks::call_present_callback(fake_swap_chain(), 7, D3D12Hooks::PRESENT_STAGE_AFTER);
	CHECK(present_records.is_empty());
}

TEST_CASE("[D3D12Hooks] The library override is registered before init") {
	HookReset reset;

	REQUIRE(D3D12Hooks::set_library_override("interposer/d3d12.dll", "interposer/dxgi.dll") == OK);
	CHECK(D3D12Hooks::has_library_override());
	CHECK(D3D12Hooks::get_d3d12_library_path() == "interposer/d3d12.dll");
	CHECK(D3D12Hooks::get_dxgi_library_path() == "interposer/dxgi.dll");

	// An empty path keeps that library's default.
	REQUIRE(D3D12Hooks::set_library_override("", "interposer/dxgi.dll") == OK);
	CHECK(D3D12Hooks::has_library_override());
	CHECK(D3D12Hooks::get_d3d12_library_path() == "D3D12.dll");
	CHECK(D3D12Hooks::get_dxgi_library_path() == "interposer/dxgi.dll");

	// Two empty paths remove the override.
	REQUIRE(D3D12Hooks::set_library_override("", "") == OK);
	CHECK_FALSE(D3D12Hooks::has_library_override());
	CHECK(D3D12Hooks::get_d3d12_library_path() == "D3D12.dll");
	CHECK(D3D12Hooks::get_dxgi_library_path() == "DXGI.dll");
}

TEST_CASE("[D3D12Hooks] The present callback receives the swap chain, the frame index and the stage") {
	HookReset reset;
	present_records.clear();

	REQUIRE(D3D12Hooks::set_present_callback(callable_mp_static(&record_present)) == OK);
	CHECK(D3D12Hooks::has_present_callback());

	ErrorCapture errors;
	D3D12Hooks::call_present_callback(fake_swap_chain(), 41, D3D12Hooks::PRESENT_STAGE_BEFORE);
	D3D12Hooks::call_present_callback(fake_swap_chain(), 41, D3D12Hooks::PRESENT_STAGE_AFTER);
	CHECK(errors.count == 0);
	REQUIRE(present_records.size() == 2);
	CHECK(present_records[0].swap_chain == 0x1000);
	CHECK(present_records[0].frame_index == 41);
	CHECK(present_records[0].stage == D3D12Hooks::PRESENT_STAGE_BEFORE);
	CHECK(present_records[1].swap_chain == 0x1000);
	CHECK(present_records[1].frame_index == 41);
	CHECK(present_records[1].stage == D3D12Hooks::PRESENT_STAGE_AFTER);

	// An empty Callable removes it.
	REQUIRE(D3D12Hooks::set_present_callback(Callable()) == OK);
	CHECK_FALSE(D3D12Hooks::has_present_callback());
	D3D12Hooks::call_present_callback(fake_swap_chain(), 42, D3D12Hooks::PRESENT_STAGE_BEFORE);
	CHECK(present_records.size() == 2);
}

TEST_CASE("[D3D12Hooks] The device created callback runs on device creation and is locked during init") {
	HookReset reset;
	DeviceCreatedCapture cap;

	REQUIRE(D3D12Hooks::set_device_created_callback(&record_device_created, &cap) == OK);
	D3D12Hooks::call_device_created_callback(nullptr);
	CHECK(cap.count == 0);
	D3D12Hooks::call_device_created_callback(fake_d3d12_device());
	CHECK(cap.count == 1);
	CHECK(cap.last_device == fake_d3d12_device());

	{
		RegistrationLock lock;
		ErrorCapture errors;
		ERR_PRINT_OFF;
		const Error err = D3D12Hooks::set_device_created_callback(nullptr, nullptr);
		ERR_PRINT_ON;
		CHECK(err == ERR_LOCKED);
		CHECK(errors.count == 1);
		CHECK(errors.last_message.contains("D3D12Hooks::set_device_created_callback()"));
	}

	REQUIRE(D3D12Hooks::set_device_created_callback(nullptr, nullptr) == OK);
	D3D12Hooks::call_device_created_callback(fake_d3d12_device());
	CHECK(cap.count == 1);

	DeviceCreatedCapture destroy_cap;
	REQUIRE(D3D12Hooks::set_device_destroying_callback(&record_device_created, &destroy_cap) == OK);
	D3D12Hooks::call_device_destroying_callback(nullptr);
	CHECK(destroy_cap.count == 0);
	D3D12Hooks::call_device_destroying_callback(fake_d3d12_device());
	CHECK(destroy_cap.count == 1);
	CHECK(destroy_cap.last_device == fake_d3d12_device());

	{
		RegistrationLock lock;
		ErrorCapture errors;
		ERR_PRINT_OFF;
		const Error err = D3D12Hooks::set_device_destroying_callback(nullptr, nullptr);
		ERR_PRINT_ON;
		CHECK(err == ERR_LOCKED);
		CHECK(errors.count == 1);
		CHECK(errors.last_message.contains("D3D12Hooks::set_device_destroying_callback()"));
	}

	REQUIRE(D3D12Hooks::set_device_destroying_callback(nullptr, nullptr) == OK);
	D3D12Hooks::call_device_destroying_callback(fake_d3d12_device());
	CHECK(destroy_cap.count == 1);
}

TEST_CASE("[D3D12Hooks] Registering after init fails with one named error") {
	HookReset reset;
	REQUIRE(D3D12Hooks::set_library_override("interposer/d3d12.dll", "interposer/dxgi.dll") == OK);

	{
		RegistrationLock lock;
		CHECK(D3D12Hooks::is_registration_locked());

		ErrorCapture errors;
		ERR_PRINT_OFF;
		const Error override_err = D3D12Hooks::set_library_override("other/d3d12.dll", "other/dxgi.dll");
		ERR_PRINT_ON;
		CHECK(override_err == ERR_LOCKED);
		CHECK(errors.count == 1);
		CHECK(errors.last_message.contains("D3D12Hooks::set_library_override()"));
		// The registered override is unchanged.
		CHECK(D3D12Hooks::get_d3d12_library_path() == "interposer/d3d12.dll");
		CHECK(D3D12Hooks::get_dxgi_library_path() == "interposer/dxgi.dll");

		ErrorCapture callback_errors;
		ERR_PRINT_OFF;
		const Error callback_err = D3D12Hooks::set_present_callback(callable_mp_static(&record_present));
		ERR_PRINT_ON;
		CHECK(callback_err == ERR_LOCKED);
		CHECK(callback_errors.count == 1);
		CHECK(callback_errors.last_message.contains("D3D12Hooks::set_present_callback()"));
		CHECK_FALSE(D3D12Hooks::has_present_callback());
	}

	// Registration opens again once no rendering context is initialized.
	CHECK_FALSE(D3D12Hooks::is_registration_locked());
	CHECK(D3D12Hooks::set_library_override("", "") == OK);
}

TEST_CASE("[D3D12Hooks] Without an override, a rendering context loads the system libraries and logs no override") {
	HookReset reset;
	PrintCapture override_lines("D3D12: Library override:");

	RenderingContextDriverD3D12 *context = memnew(RenderingContextDriverD3D12);
	CHECK(context->initialize() == OK);
	CHECK(D3D12Hooks::is_registration_locked());
	CHECK(override_lines.count == 0);
	CHECK(context->lib_d3d12 == GetModuleHandleW(L"d3d12.dll"));
	CHECK(context->lib_dxgi == GetModuleHandleW(L"dxgi.dll"));
	CHECK(find_loaded_module(stub_path()) == nullptr);
	memdelete(context);

	CHECK_FALSE(D3D12Hooks::is_registration_locked());
}

TEST_CASE("[D3D12Hooks] The stub interposer replaces D3D12 and DXGI for a rendering context") {
	HookReset reset;
	const String stub = stub_path();
	REQUIRE_MESSAGE(FileAccess::exists(stub), vformat("The stub interposer is built with the tests (tests/SCsub): %s", stub));
	REQUIRE(D3D12Hooks::set_library_override(stub, stub) == OK);

	PrintCapture override_lines("D3D12: Library override:");
	RenderingContextDriverD3D12 *context = memnew(RenderingContextDriverD3D12);
	CHECK(context->initialize() == OK);
	CHECK(D3D12Hooks::is_registration_locked());

	// One log line, naming both paths.
	CHECK(override_lines.count == 1);
	CHECK(override_lines.last_line.contains("D3D12 from \"" + stub + "\""));
	CHECK(override_lines.last_line.contains("DXGI from \"" + stub + "\""));

	HMODULE stub_module = find_loaded_module(stub);
	REQUIRE(stub_module != nullptr);
	CHECK(context->lib_d3d12 == stub_module);
	CHECK(context->lib_dxgi == stub_module);

	StubCountFunc stub_count = (StubCountFunc)(void *)GetProcAddress(stub_module, "godotnext_d3d12_stub_count");
	REQUIRE(stub_count != nullptr);
	// The context asked for the SDK configuration and created its DXGI factory through the stub,
	// and the stub forwarded both to the system libraries.
	CHECK(stub_count("D3D12GetInterface") == 1);
	CHECK(stub_count("CreateDXGIFactory2") == 1);
	CHECK(context->device_get_count() > 0);

	// Registration stays closed while the context is alive.
	ErrorCapture errors;
	ERR_PRINT_OFF;
	const Error err = D3D12Hooks::set_library_override("", "");
	ERR_PRINT_ON;
	CHECK(err == ERR_LOCKED);
	CHECK(errors.count == 1);

	memdelete(context);
	CHECK_FALSE(D3D12Hooks::is_registration_locked());
}

TEST_CASE("[D3D12Hooks] A missing override library fails initialization with an error naming it") {
	HookReset reset;
	const String missing = "godotnext_missing_d3d12_interposer.dll";
	REQUIRE(D3D12Hooks::set_library_override(missing, "") == OK);

	RenderingContextDriverD3D12 *context = memnew(RenderingContextDriverD3D12);
	ErrorCapture errors;
	ERR_PRINT_OFF;
	const Error err = context->initialize();
	ERR_PRINT_ON;
	CHECK(err != OK);
	CHECK(errors.count >= 1);
	CHECK(errors.first_message.contains(missing));
	CHECK(context->lib_d3d12 == nullptr);
	memdelete(context);

	CHECK_FALSE(D3D12Hooks::is_registration_locked());
}

// The stub interposer in rendered runs (test_d3d12_hooks.h).

HMODULE stub_run_module = nullptr;
StubPresentFunc stub_run_present = nullptr;

void stub_run_present_callback(uint64_t p_swap_chain, uint64_t p_frame_index, int p_stage) {
	stub_run_present((void *)(uintptr_t)p_swap_chain, p_frame_index, p_stage);
}

} // namespace TestD3D12Hooks

void d3d12_hooks_stub_install() {
	using namespace TestD3D12Hooks;
	if (OS::get_singleton()->get_environment("GODOTNEXT_D3D12_HOOKS_STUB") != "1") {
		return;
	}

	const String stub = stub_path();
	// Keep a reference, so the counts outlive the rendering context, which frees its libraries.
	stub_run_module = LoadLibraryW((LPCWSTR)(stub.replace_char('/', '\\').utf16().get_data()));
	ERR_FAIL_NULL_MSG(stub_run_module, "GODOTNEXT_D3D12_HOOKS_STUB: Failed to load the stub interposer \"" + stub + "\".");
	stub_run_present = (StubPresentFunc)(void *)GetProcAddress(stub_run_module, "godotnext_d3d12_stub_present");
	ERR_FAIL_NULL_MSG(stub_run_present, "GODOTNEXT_D3D12_HOOKS_STUB: The stub interposer has no present export.");

	ERR_FAIL_COND(D3D12Hooks::set_library_override(stub, stub) != OK);
	ERR_FAIL_COND(D3D12Hooks::set_present_callback(callable_mp_static(&stub_run_present_callback)) != OK);
}

void d3d12_hooks_stub_finish() {
	using namespace TestD3D12Hooks;
	if (stub_run_module == nullptr) {
		return;
	}

	// The rendering context is gone, so registration is open again.
	D3D12Hooks::set_present_callback(Callable());
	D3D12Hooks::set_library_override(String(), String());

	StubCountFunc stub_count = (StubCountFunc)(void *)GetProcAddress(stub_run_module, "godotnext_d3d12_stub_count");
	if (stub_count != nullptr) {
		const uint64_t frames = stub_count("present_frames");
		const uint64_t presents = stub_count("present_calls");
		const uint64_t mismatches = stub_count("present_mismatches");
		const uint64_t factories = stub_count("CreateDXGIFactory2");
		const uint64_t get_interface = stub_count("D3D12GetInterface");
		const uint64_t create_device = stub_count("D3D12CreateDevice");
		const bool pass = frames > 0 && presents == frames && mismatches == 0 && factories > 0 && get_interface + create_device > 0;
		print_line(vformat("GODOTNEXT_RESULT d3d12_hooks_stub pass=%s frames=%d presents=%d present_mismatches=%d CreateDXGIFactory2=%d D3D12GetInterface=%d D3D12CreateDevice=%d",
				pass ? "true" : "false", frames, presents, mismatches, factories, get_interface, create_device));
		if (!pass) {
			ERR_PRINT("GODOTNEXT_D3D12_HOOKS_STUB: The stub interposer did not count exactly 1 Present per frame through the D3D12 hooks.");
		}
	} else {
		ERR_PRINT("GODOTNEXT_D3D12_HOOKS_STUB: The stub interposer has no count export.");
	}

	FreeLibrary(stub_run_module);
	stub_run_module = nullptr;
	stub_run_present = nullptr;
}

#endif // D3D12_ENABLED
