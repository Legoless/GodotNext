/**************************************************************************/
/*  dlss_vulkan_extensions.h                                              */
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

// GodotNext: pu_dlss_sr_effect (#100).
// Vulkan extension provider and object-attributed validation allowlist filter for modules/dlss.

#ifdef VULKAN_ENABLED

#include "dlss_backend.h"

#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "drivers/vulkan/vulkan_extension_provider.h"

class DLSSVulkanExtensionProvider : public VulkanExtensionProvider {
public:
	struct AllowlistRow {
		String vuid;
		String dll_sha256;
		String driver;
	};

	typedef void (*DeviceDestroyingCallback)(void *p_userdata);

private:
	mutable Mutex mutex;
	DLSSBackend *backend = nullptr;
	DeviceDestroyingCallback destroying_callback = nullptr;
	void *destroying_userdata = nullptr;

	Vector<CharString> requested_instance_extensions;
	Vector<CharString> requested_device_extensions;
	Vector<CharString> missing_instance_extensions;
	Vector<CharString> missing_device_extensions;

	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t vendor_id = 0;
	String driver_version;
	String loaded_dll_sha256;
	bool device_created = false;
	bool extensions_satisfied = true;

	Vector<AllowlistRow> test_allowlist_rows;

public:
	static constexpr uint32_t VENDOR_NVIDIA = 0x10DE;

	explicit DLSSVulkanExtensionProvider(DLSSBackend *p_backend);
	~DLSSVulkanExtensionProvider() override = default;

	void set_backend(DLSSBackend *p_backend);
	void set_device_destroying_callback(DeviceDestroyingCallback p_cb, void *p_userdata);
	void set_loaded_dll_sha256(const String &p_sha256);
	void set_driver_version_for_tests(const String &p_driver_version);
	void set_test_allowlist(const Vector<AllowlistRow> &p_rows);

	// Pure vendor-gating helper shared with doctests.
	static Vector<CharString> filter_device_extensions_for_vendor(uint32_t p_vendor_id, const Vector<CharString> &p_exts);
	// Pure allowlist matcher shared with doctests.
	static bool matches_allowlist(const String &p_vuid, const String &p_dll_sha256, const String &p_driver, const Vector<AllowlistRow> &p_extra_rows);

	uint32_t get_vendor_id() const;
	String get_driver_version() const;
	bool are_extensions_satisfied() const;
	bool is_device_created() const;
	Vector<CharString> get_requested_instance_extensions() const;
	Vector<CharString> get_requested_device_extensions() const;
	Vector<CharString> get_missing_extensions() const;

	// VulkanExtensionProvider overrides.
	String get_name() const override { return "DLSS"; }
	Vector<CharString> get_instance_extensions() override;
	Vector<CharString> get_device_extensions(VkInstance p_instance, VkPhysicalDevice p_physical_device) override;
	void on_device_created(const DeviceInfo &p_info) override;
	void on_device_destroying(VkDevice p_device) override;
	ValidationFilterResult filter_validation_message(const VkDebugUtilsMessengerCallbackDataEXT *p_callback_data) override;
};

#endif // VULKAN_ENABLED
