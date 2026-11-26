/**************************************************************************/
/*  vulkan_extension_provider.h                                           */
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

// GodotNext: pu_vk_extension_providers (#105).

#include "core/error/error_list.h"
#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/vector.h"

#include <drivers/vulkan/godot_vulkan.h>

// A module that needs optional Vulkan instance or device extensions, or pNext feature structures,
// before the instance and the device exist, registers a provider. Providers work beside VulkanHooks,
// which has a single owner (OpenXR): their extension names are merged into the create infos before
// the hooks run, so the hooks' owner forwards them and keeps choosing the physical device.
//
// Every extension a provider asks for is optional: a missing one disables only that provider's
// feature, and device creation never fails because of it.
class VulkanExtensionProvider {
public:
	enum ValidationFilterResult {
		// The message is printed (and aborts under --gpu-abort) as without the provider.
		VALIDATION_DEFAULT,
		// The message is printed as a warning tagged with the provider's name, and never aborts.
		VALIDATION_ALLOWLISTED,
	};

	struct DeviceInfo {
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice physical_device = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		// The main queue: the one RenderingDevice records and submits frames on.
		uint32_t queue_family_index = 0;
		uint32_t queue_index = 0;
		// Every extension that is enabled, Godot's own included. A name the provider asked for
		// that is not in these sets was missing (see VulkanExtensionProviders::missing_for()).
		HashSet<CharString> enabled_instance_extensions;
		HashSet<CharString> enabled_device_extensions;
	};

	virtual ~VulkanExtensionProvider() {}

	// A short name for log lines.
	virtual String get_name() const = 0;

	// Called once per rendering context, before the instance is created.
	virtual Vector<CharString> get_instance_extensions() { return Vector<CharString>(); }

	// Called once per device, before it is created, for the physical device Godot or the
	// VulkanHooks owner chose.
	virtual Vector<CharString> get_device_extensions(VkInstance p_instance, VkPhysicalDevice p_physical_device) { return Vector<CharString>(); }

	// Prepends feature structures to the device's pNext chain: set the last new structure's pNext
	// to r_next, then r_next to the first new one. The structures must stay valid until
	// on_device_created(). It is called only when every device extension the provider asked for is
	// enabled. Chain only features the physical device reports: the chain's shape is checked, the
	// feature values are not.
	virtual void chain_device_features(VkPhysicalDevice p_physical_device, void *&r_next) {}

	// Called once per device, when RenderingDevice creates its main queue.
	virtual void on_device_created(const DeviceInfo &p_info) {}

	// Called before the device given to on_device_created() is destroyed.
	virtual void on_device_destroying(VkDevice p_device) {}

	// Called for a VK_EXT_debug_utils message that references no handle GodotNext created (the
	// shared instance, physical device, device, queues and command buffers do not count) and no
	// surface (only the rendering context creates surfaces). It can run on any thread and must not
	// call Vulkan. Only builds with the created-handle registry (DEBUG_ENABLED) running with
	// validation layers call it.
	virtual ValidationFilterResult filter_validation_message(const VkDebugUtilsMessengerCallbackDataEXT *p_callback_data) { return VALIDATION_DEFAULT; }
};

class VulkanExtensionProviders {
	static Mutex mutex;
	static LocalVector<VulkanExtensionProvider *> providers;
	static SafeNumeric<uint32_t> registration_locks;

	static LocalVector<VulkanExtensionProvider *> _get_providers();
	static bool _accept_chained_features(const VulkanExtensionProvider *p_provider, void *p_previous_next, void *&r_next);

public:
	// The device extensions each provider asked for, kept by the device driver between the two
	// calls below.
	typedef HashMap<const VulkanExtensionProvider *, Vector<CharString>> DeviceRequests;

	// Call it at MODULE_INITIALIZATION_LEVEL_SERVERS, before the display server creates the
	// rendering context: while a Vulkan rendering context exists it fails with ERR_LOCKED.
	// Providers are asked in registration order. The registry does not own them.
	static Error register_provider(VulkanExtensionProvider *p_provider);
	// Allowed at any time. A provider unregistered while a device exists gets no further calls, so
	// it releases what it holds on that device first. Modules uninitialize
	// MODULE_INITIALIZATION_LEVEL_SERVERS before the rendering device is destroyed: a provider that
	// relies on on_device_destroying() stays registered until MODULE_INITIALIZATION_LEVEL_CORE
	// uninitializes.
	static void unregister_provider(VulkanExtensionProvider *p_provider);
	static uint32_t get_provider_count();
	// Test-only isolation: detaches any providers registered at module init (e.g. modules/dlss)
	// so unit tests can assert an empty registry and restore them afterwards.
	static LocalVector<VulkanExtensionProvider *> detach_all_for_tests();
	static void restore_for_tests(const LocalVector<VulkanExtensionProvider *> &p_saved);

	// Called by RenderingContextDriverVulkan: registration stays closed while any context exists.
	static void lock_registration();
	static void unlock_registration();
	static bool is_registration_locked();

	// Adds each name once, as optional. Names already requested keep their required flag.
	static void merge_requests(HashMap<CharString, bool> &r_requested, const Vector<CharString> &p_names);
	// The names that are not enabled, each once.
	static Vector<CharString> missing_for(const Vector<CharString> &p_names, const HashSet<CharString> &p_enabled);

	// The calls the Vulkan drivers make, in this order.
	static void merge_instance_extensions(HashMap<CharString, bool> &r_requested);
	static void merge_device_extensions(VkInstance p_instance, VkPhysicalDevice p_physical_device, HashMap<CharString, bool> &r_requested, DeviceRequests &r_provider_requests);
	static void chain_device_features(VkPhysicalDevice p_physical_device, const DeviceRequests &p_provider_requests, const HashSet<CharString> &p_enabled_device_extensions, void *&r_next);
	static void notify_device_created(const VulkanExtensionProvider::DeviceInfo &p_info);
	static void notify_device_destroying(VkDevice p_device);

	// The object-attributed rule. Returns true, with the provider's name, when a provider
	// allowlists the message. A message that references a handle in VulkanCreatedHandles, or any
	// surface, reaches no provider. Builds without that registry never allowlist.
	static bool filter_validation_message(const VkDebugUtilsMessengerCallbackDataEXT *p_callback_data, String &r_provider_name);
};

#ifdef DEBUG_ENABLED
// The Vulkan handles a RenderingDeviceDriverVulkan created and has not destroyed yet. Debug builds
// only (never in template_release), and filled only when validation layers are enabled. A handle
// is counted, because a driver can hand out a value again before the previous owner is released
// (descriptor sets of a reset pool).
class VulkanCreatedHandles {
	static BinaryMutex registries_mutex;
	static LocalVector<const VulkanCreatedHandles *> registries;

	mutable BinaryMutex mutex;
	HashMap<uint64_t, uint32_t> handles;

public:
	void add(uint64_t p_handle);
	// Returns false when the handle was not there.
	bool remove(uint64_t p_handle);
	bool has(uint64_t p_handle) const;
	uint32_t get_count() const;

	// Whether any live registry holds the handle.
	static bool is_created(uint64_t p_handle);

	VulkanCreatedHandles();
	~VulkanCreatedHandles();
};
#endif // DEBUG_ENABLED
