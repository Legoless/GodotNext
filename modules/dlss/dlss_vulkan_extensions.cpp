/**************************************************************************/
/*  dlss_vulkan_extensions.cpp                                            */
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

#include "dlss_vulkan_extensions.h"

#ifdef VULKAN_ENABLED

#include "dlss_validation_allowlist.gen.h"

#include "core/string/print_string.h"
#include "core/variant/variant.h"

namespace {

String format_nvidia_driver_version(uint32_t p_vendor_id, uint32_t p_driver_version) {
	if (p_vendor_id == DLSSVulkanExtensionProvider::VENDOR_NVIDIA) {
		const uint32_t major = (p_driver_version >> 22) & 0x3ffu;
		const uint32_t minor = (p_driver_version >> 14) & 0xffu;
		const uint32_t secondary = (p_driver_version >> 6) & 0xffu;
		const uint32_t tertiary = p_driver_version & 0x3fu;
		if (secondary == 0 && tertiary == 0) {
			return vformat("%d.%02d", major, minor);
		}
		return vformat("%d.%02d.%02d", major, minor, secondary);
	}
	return vformat("%d.%d.%d", VK_API_VERSION_MAJOR(p_driver_version), VK_API_VERSION_MINOR(p_driver_version), VK_API_VERSION_PATCH(p_driver_version));
}

} // namespace

DLSSVulkanExtensionProvider::DLSSVulkanExtensionProvider(DLSSBackend *p_backend) :
		backend(p_backend) {}

void DLSSVulkanExtensionProvider::set_backend(DLSSBackend *p_backend) {
	MutexLock lock(mutex);
	backend = p_backend;
}

void DLSSVulkanExtensionProvider::set_device_destroying_callback(DeviceDestroyingCallback p_cb, void *p_userdata) {
	MutexLock lock(mutex);
	destroying_callback = p_cb;
	destroying_userdata = p_userdata;
}

void DLSSVulkanExtensionProvider::set_loaded_dll_sha256(const String &p_sha256) {
	MutexLock lock(mutex);
	loaded_dll_sha256 = p_sha256.to_lower();
	loaded_dll_sha256s.clear();
	if (!loaded_dll_sha256.is_empty()) {
		loaded_dll_sha256s.push_back(loaded_dll_sha256);
	}
}

void DLSSVulkanExtensionProvider::add_loaded_dll_sha256(const String &p_sha256) {
	const String lower = p_sha256.to_lower();
	if (lower.is_empty()) {
		return;
	}
	MutexLock lock(mutex);
	if (loaded_dll_sha256.is_empty()) {
		loaded_dll_sha256 = lower;
	}
	if (!loaded_dll_sha256s.has(lower)) {
		loaded_dll_sha256s.push_back(lower);
	}
}

void DLSSVulkanExtensionProvider::set_driver_version_for_tests(const String &p_driver_version) {
	MutexLock lock(mutex);
	driver_version = p_driver_version;
}

void DLSSVulkanExtensionProvider::set_test_allowlist(const Vector<AllowlistRow> &p_rows) {
	MutexLock lock(mutex);
	test_allowlist_rows = p_rows;
}

Vector<CharString> DLSSVulkanExtensionProvider::filter_device_extensions_for_vendor(uint32_t p_vendor_id, const Vector<CharString> &p_exts) {
	if (p_vendor_id != VENDOR_NVIDIA) {
		return Vector<CharString>();
	}
	return p_exts;
}

bool DLSSVulkanExtensionProvider::matches_allowlist(const String &p_vuid, const String &p_dll_sha256, const String &p_driver, const Vector<AllowlistRow> &p_extra_rows) {
	if (p_vuid.is_empty() || p_dll_sha256.is_empty() || p_driver.is_empty()) {
		return false;
	}
	const String sha_lower = p_dll_sha256.to_lower();
	for (uint32_t i = 0; i < DLSS_VALIDATION_ALLOWLIST_COUNT; i++) {
		const DLSSValidationAllowlistEntry &entry = DLSS_VALIDATION_ALLOWLIST[i];
		if (p_vuid == String(entry.vuid) && sha_lower == String(entry.dll_sha256).to_lower() && p_driver == String(entry.driver)) {
			return true;
		}
	}
	for (const AllowlistRow &row : p_extra_rows) {
		if (p_vuid == row.vuid && sha_lower == row.dll_sha256.to_lower() && p_driver == row.driver) {
			return true;
		}
	}
	return false;
}

uint32_t DLSSVulkanExtensionProvider::get_vendor_id() const {
	MutexLock lock(mutex);
	return vendor_id;
}

String DLSSVulkanExtensionProvider::get_driver_version() const {
	MutexLock lock(mutex);
	return driver_version;
}

bool DLSSVulkanExtensionProvider::are_extensions_satisfied() const {
	MutexLock lock(mutex);
	return extensions_satisfied;
}

bool DLSSVulkanExtensionProvider::is_device_created() const {
	MutexLock lock(mutex);
	return device_created;
}

Vector<CharString> DLSSVulkanExtensionProvider::get_requested_instance_extensions() const {
	MutexLock lock(mutex);
	return requested_instance_extensions;
}

Vector<CharString> DLSSVulkanExtensionProvider::get_requested_device_extensions() const {
	MutexLock lock(mutex);
	return requested_device_extensions;
}

Vector<CharString> DLSSVulkanExtensionProvider::get_missing_extensions() const {
	MutexLock lock(mutex);
	Vector<CharString> out = missing_instance_extensions;
	out.append_array(missing_device_extensions);
	return out;
}

Vector<CharString> DLSSVulkanExtensionProvider::get_instance_extensions() {
	DLSSBackend *active = nullptr;
	{
		MutexLock lock(mutex);
		active = backend;
	}
	Vector<CharString> exts;
	if (active != nullptr) {
		exts = active->get_instance_extensions();
	}
	{
		MutexLock lock(mutex);
		requested_instance_extensions = exts;
	}
	if (!exts.is_empty()) {
		PackedStringArray names;
		for (const CharString &e : exts) {
			names.push_back(String::utf8(e.get_data()));
		}
		print_verbose(vformat("DLSS Vulkan required instance extensions (%d): %s", exts.size(), String(", ").join(names)));
	}
	return exts;
}

Vector<CharString> DLSSVulkanExtensionProvider::get_device_extensions(VkInstance p_instance, VkPhysicalDevice p_physical_device) {
	uint32_t detected_vendor = 0;
	String detected_driver;
	if (p_physical_device != VK_NULL_HANDLE) {
		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties(p_physical_device, &props);
		detected_vendor = props.vendorID;
		detected_driver = format_nvidia_driver_version(props.vendorID, props.driverVersion);
	}

	DLSSBackend *active = nullptr;
	{
		MutexLock lock(mutex);
		vendor_id = detected_vendor;
		driver_version = detected_driver;
		active = backend;
	}

	Vector<CharString> exts;
	if (detected_vendor == VENDOR_NVIDIA && active != nullptr) {
		exts = active->get_device_extensions(reinterpret_cast<uint64_t>(p_instance), reinterpret_cast<uint64_t>(p_physical_device));
	}
	exts = filter_device_extensions_for_vendor(detected_vendor, exts);

	{
		MutexLock lock(mutex);
		requested_device_extensions = exts;
	}
	if (!exts.is_empty()) {
		PackedStringArray names;
		for (const CharString &e : exts) {
			names.push_back(String::utf8(e.get_data()));
		}
		print_verbose(vformat("DLSS Vulkan required device extensions (vendor=0x%04x, driver=%s, count=%d): %s", detected_vendor, detected_driver, exts.size(), String(", ").join(names)));
	}
	return exts;
}

void DLSSVulkanExtensionProvider::on_device_created(const DeviceInfo &p_info) {
	MutexLock lock(mutex);
	instance = p_info.instance;
	physical_device = p_info.physical_device;
	device = p_info.device;
	device_created = true;

	if (physical_device != VK_NULL_HANDLE && vendor_id == 0) {
		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties(physical_device, &props);
		vendor_id = props.vendorID;
		driver_version = format_nvidia_driver_version(props.vendorID, props.driverVersion);
	}

	missing_instance_extensions = VulkanExtensionProviders::missing_for(requested_instance_extensions, p_info.enabled_instance_extensions);
	missing_device_extensions = VulkanExtensionProviders::missing_for(requested_device_extensions, p_info.enabled_device_extensions);
	extensions_satisfied = missing_instance_extensions.is_empty() && missing_device_extensions.is_empty();
}

void DLSSVulkanExtensionProvider::on_device_destroying(VkDevice p_device) {
	DeviceDestroyingCallback cb = nullptr;
	void *userdata = nullptr;
	{
		MutexLock lock(mutex);
		if (p_device != device && device != VK_NULL_HANDLE) {
			return;
		}
		cb = destroying_callback;
		userdata = destroying_userdata;
		device_created = false;
		device = VK_NULL_HANDLE;
		physical_device = VK_NULL_HANDLE;
		instance = VK_NULL_HANDLE;
	}
	if (cb != nullptr) {
		cb(userdata);
	}
}

VulkanExtensionProvider::ValidationFilterResult DLSSVulkanExtensionProvider::filter_validation_message(const VkDebugUtilsMessengerCallbackDataEXT *p_callback_data) {
	if (p_callback_data == nullptr || p_callback_data->pMessageIdName == nullptr) {
		return VALIDATION_DEFAULT;
	}
	const String vuid = String::utf8(p_callback_data->pMessageIdName);
	Vector<String> shas;
	String drv;
	Vector<AllowlistRow> extra;
	{
		MutexLock lock(mutex);
		shas = loaded_dll_sha256s;
		if (shas.is_empty() && !loaded_dll_sha256.is_empty()) {
			shas.push_back(loaded_dll_sha256);
		}
		drv = driver_version;
		extra = test_allowlist_rows;
	}
	for (const String &sha : shas) {
		if (matches_allowlist(vuid, sha, drv, extra)) {
			return VALIDATION_ALLOWLISTED;
		}
	}
	return VALIDATION_DEFAULT;
}

#endif // VULKAN_ENABLED
