/**************************************************************************/
/*  vulkan_extension_provider.cpp                                         */
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

#include "vulkan_extension_provider.h"

// GodotNext: pu_vk_extension_providers (#105).

#include "core/error/error_macros.h"
#include "core/os/memory.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

Mutex VulkanExtensionProviders::mutex;
LocalVector<VulkanExtensionProvider *> VulkanExtensionProviders::providers;
SafeNumeric<uint32_t> VulkanExtensionProviders::registration_locks;

// A provider's own structures before the chain it was given; more than this is a cycle.
static const uint32_t MAX_CHAINED_FEATURE_STRUCTURES = 64;

static String _join_names(const Vector<CharString> &p_names) {
	String joined;
	for (const CharString &name : p_names) {
		if (!joined.is_empty()) {
			joined += ", ";
		}
		joined += String::utf8(name.get_data());
	}
	return joined;
}

#ifdef DEBUG_ENABLED
// With GODOTNEXT_VK_TEST_DEVICE_EXTENSION=<name> in the environment, a built-in provider asks for
// that device extension and prints whether the device enabled it. It checks the registry on a real
// driver, where a doctest cannot.
class VulkanTestExtensionProvider : public VulkanExtensionProvider {
	CharString extension;

public:
	virtual String get_name() const override { return "GodotNext test provider"; }

	virtual Vector<CharString> get_device_extensions(VkInstance p_instance, VkPhysicalDevice p_physical_device) override {
		Vector<CharString> names;
		names.push_back(extension);
		return names;
	}

	virtual void on_device_created(const DeviceInfo &p_info) override {
		print_line(vformat("GODOTNEXT_RESULT {\"test\":\"vk_ext_provider\",\"extension\":\"%s\",\"enabled\":%s}", String::utf8(extension.get_data()), p_info.enabled_device_extensions.has(extension) ? "true" : "false"));
	}

	VulkanTestExtensionProvider(const CharString &p_extension) :
			extension(p_extension) {}
};

static VulkanTestExtensionProvider *test_provider = nullptr;

static void _create_test_provider() {
	const String name = OS::get_singleton()->get_environment("GODOTNEXT_VK_TEST_DEVICE_EXTENSION");
	if (name.is_empty()) {
		return;
	}
	for (int i = 0; i < name.length(); i++) {
		ERR_FAIL_COND_MSG(!is_ascii_identifier_char(name[i]), "GODOTNEXT_VK_TEST_DEVICE_EXTENSION: \"" + name + "\" is not an extension name.");
	}
	test_provider = memnew(VulkanTestExtensionProvider(name.utf8()));
}
#endif // DEBUG_ENABLED

Error VulkanExtensionProviders::register_provider(VulkanExtensionProvider *p_provider) {
	ERR_FAIL_NULL_V(p_provider, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(is_registration_locked(), ERR_LOCKED, "VulkanExtensionProviders::register_provider() must be called before the Vulkan rendering context initializes (at MODULE_INITIALIZATION_LEVEL_SERVERS).");
	MutexLock lock(mutex);
	ERR_FAIL_COND_V_MSG(providers.has(p_provider), ERR_ALREADY_EXISTS, "This Vulkan extension provider is already registered.");
	providers.push_back(p_provider);
	return OK;
}

void VulkanExtensionProviders::unregister_provider(VulkanExtensionProvider *p_provider) {
	MutexLock lock(mutex);
	providers.erase(p_provider);
	if (providers.is_empty()) {
		// Leave no allocation behind for the static's destructor at exit.
		providers.reset();
	}
}

uint32_t VulkanExtensionProviders::get_provider_count() {
	MutexLock lock(mutex);
	return providers.size();
}

#ifdef DEBUG_ENABLED
LocalVector<VulkanExtensionProvider *> VulkanExtensionProviders::detach_all_for_tests() {
	MutexLock lock(mutex);
	LocalVector<VulkanExtensionProvider *> saved(providers);
	providers.reset();
	return saved;
}

void VulkanExtensionProviders::restore_for_tests(const LocalVector<VulkanExtensionProvider *> &p_saved) {
	MutexLock lock(mutex);
	providers = p_saved;
}
#endif

// Providers are called on a copy of the list, outside the lock: a provider's own threads can raise
// validation messages, and those take the lock.
LocalVector<VulkanExtensionProvider *> VulkanExtensionProviders::_get_providers() {
	MutexLock lock(mutex);
	return LocalVector<VulkanExtensionProvider *>(providers);
}

void VulkanExtensionProviders::lock_registration() {
#ifdef DEBUG_ENABLED
	if (registration_locks.get() == 0 && test_provider == nullptr) {
		_create_test_provider();
		if (test_provider != nullptr) {
			register_provider(test_provider);
		}
	}
#endif
	registration_locks.increment();
}

void VulkanExtensionProviders::unlock_registration() {
	ERR_FAIL_COND(registration_locks.get() == 0);
	if (registration_locks.decrement() > 0) {
		return;
	}
#ifdef DEBUG_ENABLED
	if (test_provider != nullptr) {
		unregister_provider(test_provider);
		memdelete(test_provider);
		test_provider = nullptr;
	}
#endif
}

bool VulkanExtensionProviders::is_registration_locked() {
	return registration_locks.get() > 0;
}

void VulkanExtensionProviders::merge_requests(HashMap<CharString, bool> &r_requested, const Vector<CharString> &p_names) {
	for (const CharString &name : p_names) {
		if (name.length() > 0 && !r_requested.has(name)) {
			r_requested[name] = false;
		}
	}
}

Vector<CharString> VulkanExtensionProviders::missing_for(const Vector<CharString> &p_names, const HashSet<CharString> &p_enabled) {
	Vector<CharString> missing;
	for (const CharString &name : p_names) {
		if (name.length() > 0 && !p_enabled.has(name) && !missing.has(name)) {
			missing.push_back(name);
		}
	}
	return missing;
}

void VulkanExtensionProviders::merge_instance_extensions(HashMap<CharString, bool> &r_requested) {
	for (VulkanExtensionProvider *provider : _get_providers()) {
		merge_requests(r_requested, provider->get_instance_extensions());
	}
}

void VulkanExtensionProviders::merge_device_extensions(VkInstance p_instance, VkPhysicalDevice p_physical_device, HashMap<CharString, bool> &r_requested, DeviceRequests &r_provider_requests) {
	for (VulkanExtensionProvider *provider : _get_providers()) {
		const Vector<CharString> names = provider->get_device_extensions(p_instance, p_physical_device);
		merge_requests(r_requested, names);
		r_provider_requests[provider] = names;
	}
}

bool VulkanExtensionProviders::_accept_chained_features(const VulkanExtensionProvider *p_provider, void *p_previous_next, void *&r_next) {
	// The provider's structures must lead back to the chain it was given.
	uint32_t new_count = 0;
	const VkBaseOutStructure *structure = (const VkBaseOutStructure *)r_next;
	while (structure != p_previous_next && structure != nullptr && new_count < MAX_CHAINED_FEATURE_STRUCTURES) {
		structure = structure->pNext;
		new_count++;
	}
	if (structure != p_previous_next) {
		r_next = p_previous_next;
		ERR_FAIL_V_MSG(false, vformat("Vulkan extension provider \"%s\" did not keep the device's pNext chain. Its feature structures are not used.", p_provider->get_name()));
	}

	// A structure type can be in the chain once (VUID-VkDeviceCreateInfo-sType-unique).
	const VkBaseOutStructure *added = (const VkBaseOutStructure *)r_next;
	for (uint32_t i = 0; i < new_count; i++) {
		for (const VkBaseOutStructure *other = added->pNext; other != nullptr; other = other->pNext) {
			if (other->sType == added->sType) {
				r_next = p_previous_next;
				ERR_FAIL_V_MSG(false, vformat("Vulkan extension provider \"%s\" chained a feature structure (sType %d) that is already in the device's pNext chain. Its feature structures are not used.", p_provider->get_name(), (int64_t)added->sType));
			}
		}
		added = added->pNext;
	}

	return true;
}

void VulkanExtensionProviders::chain_device_features(VkPhysicalDevice p_physical_device, const DeviceRequests &p_provider_requests, const HashSet<CharString> &p_enabled_device_extensions, void *&r_next) {
	for (VulkanExtensionProvider *provider : _get_providers()) {
		const Vector<CharString> *requested = p_provider_requests.getptr(provider);
		if (requested == nullptr) {
			continue;
		}
		// A feature structure of an extension that is not enabled is a validation error.
		const Vector<CharString> missing = missing_for(*requested, p_enabled_device_extensions);
		if (!missing.is_empty()) {
			print_verbose(vformat("Vulkan extension provider \"%s\": device extension(s) not found: %s. Its feature structures are not chained.", provider->get_name(), _join_names(missing)));
			continue;
		}

		void *previous_next = r_next;
		provider->chain_device_features(p_physical_device, r_next);
		_accept_chained_features(provider, previous_next, r_next);
	}
}

void VulkanExtensionProviders::notify_device_created(const VulkanExtensionProvider::DeviceInfo &p_info) {
	for (VulkanExtensionProvider *provider : _get_providers()) {
		provider->on_device_created(p_info);
	}
}

void VulkanExtensionProviders::notify_device_destroying(VkDevice p_device) {
	for (VulkanExtensionProvider *provider : _get_providers()) {
		provider->on_device_destroying(p_device);
	}
}

bool VulkanExtensionProviders::filter_validation_message(const VkDebugUtilsMessengerCallbackDataEXT *p_callback_data, String &r_provider_name) {
#ifdef DEBUG_ENABLED
	// Held while the providers are asked, so a provider is not unregistered under a message.
	MutexLock lock(mutex);
	if (providers.is_empty()) {
		return false;
	}

	for (uint32_t i = 0; i < p_callback_data->objectCount; i++) {
		const VkDebugUtilsObjectNameInfoEXT &object = p_callback_data->pObjects[i];
		switch (object.objectType) {
			case VK_OBJECT_TYPE_INSTANCE:
			case VK_OBJECT_TYPE_PHYSICAL_DEVICE:
			case VK_OBJECT_TYPE_DEVICE:
			case VK_OBJECT_TYPE_QUEUE:
			case VK_OBJECT_TYPE_COMMAND_BUFFER:
				// Shared with the providers' own Vulkan calls.
				continue;
			case VK_OBJECT_TYPE_SURFACE_KHR:
				// Only the rendering context creates surfaces, one per window, so a surface is
				// always GodotNext's.
				return false;
			default:
				break;
		}
		if (VulkanCreatedHandles::is_created(object.objectHandle)) {
			// GodotNext's own object: no provider can allowlist the message.
			return false;
		}
	}

	for (VulkanExtensionProvider *provider : providers) {
		if (provider->filter_validation_message(p_callback_data) == VulkanExtensionProvider::VALIDATION_ALLOWLISTED) {
			r_provider_name = provider->get_name();
			return true;
		}
	}
#endif // DEBUG_ENABLED

	return false;
}

#ifdef DEBUG_ENABLED
BinaryMutex VulkanCreatedHandles::registries_mutex;
LocalVector<const VulkanCreatedHandles *> VulkanCreatedHandles::registries;

void VulkanCreatedHandles::add(uint64_t p_handle) {
	if (p_handle == 0) {
		return;
	}
	MutexLock<BinaryMutex> lock(mutex);
	uint32_t *count = handles.getptr(p_handle);
	if (count != nullptr) {
		(*count)++;
	} else {
		handles.insert(p_handle, 1);
	}
}

bool VulkanCreatedHandles::remove(uint64_t p_handle) {
	MutexLock<BinaryMutex> lock(mutex);
	HashMap<uint64_t, uint32_t>::Iterator handle = handles.find(p_handle);
	if (!handle) {
		return false;
	}
	handle->value--;
	if (handle->value == 0) {
		handles.remove(handle);
	}
	return true;
}

bool VulkanCreatedHandles::has(uint64_t p_handle) const {
	MutexLock<BinaryMutex> lock(mutex);
	return handles.has(p_handle);
}

uint32_t VulkanCreatedHandles::get_count() const {
	MutexLock<BinaryMutex> lock(mutex);
	return handles.size();
}

bool VulkanCreatedHandles::is_created(uint64_t p_handle) {
	MutexLock<BinaryMutex> lock(registries_mutex);
	for (const VulkanCreatedHandles *registry : registries) {
		if (registry->has(p_handle)) {
			return true;
		}
	}
	return false;
}

VulkanCreatedHandles::VulkanCreatedHandles() {
	MutexLock<BinaryMutex> lock(registries_mutex);
	registries.push_back(this);
}

VulkanCreatedHandles::~VulkanCreatedHandles() {
	MutexLock<BinaryMutex> lock(registries_mutex);
	registries.erase(this);
	if (registries.is_empty()) {
		registries.reset();
	}
}
#endif // DEBUG_ENABLED
