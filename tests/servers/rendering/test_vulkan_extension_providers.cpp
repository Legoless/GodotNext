/**************************************************************************/
/*  test_vulkan_extension_providers.cpp                                   */
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

TEST_FORCE_LINK(test_vulkan_extension_providers)

// GodotNext: pu_vk_extension_providers (#105). The Vulkan extension-provider registry (GU-1).
// No case needs a Vulkan loader or a GPU: the registry works on plain lists, the create infos go
// to a mock VulkanHooks owner instead of Vulkan, and the validation message is built by hand.
// The cases are [Upscalers] cases, but they live in their own file: on Linux the Vulkan headers
// declare Xlib's Window type, which cannot share a file with the scene's Window class that
// test_upscalers.cpp includes.

#ifdef VULKAN_ENABLED

#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "drivers/vulkan/vulkan_extension_provider.h"
#include "drivers/vulkan/vulkan_hooks.h"

namespace TestVkProviders {

class MockProvider : public VulkanExtensionProvider {
public:
	enum ChainMode {
		CHAIN_NOTHING,
		CHAIN_PREPEND,
		CHAIN_REPLACE, // Loses the chain it was given.
		CHAIN_CYCLE,
	};

	String name;
	Vector<CharString> instance_extensions;
	Vector<CharString> device_extensions;
	ChainMode chain_mode = CHAIN_NOTHING;
	VkBaseOutStructure feature = {};
	ValidationFilterResult filter_result = VALIDATION_DEFAULT;

	VkInstance seen_instance = VK_NULL_HANDLE;
	VkPhysicalDevice seen_physical_device = VK_NULL_HANDLE;
	VkDevice destroyed_device = VK_NULL_HANDLE;
	DeviceInfo created_info;
	int chain_calls = 0;
	int created_calls = 0;
	int destroying_calls = 0;
	int filter_calls = 0;
	// Shared by the providers of a case, to record the order they are called in.
	Vector<String> *call_order = nullptr;

	virtual String get_name() const override { return name; }
	virtual Vector<CharString> get_instance_extensions() override { return instance_extensions; }
	virtual Vector<CharString> get_device_extensions(VkInstance p_instance, VkPhysicalDevice p_physical_device) override {
		seen_instance = p_instance;
		seen_physical_device = p_physical_device;
		return device_extensions;
	}
	virtual void chain_device_features(VkPhysicalDevice p_physical_device, void *&r_next) override {
		chain_calls++;
		switch (chain_mode) {
			case CHAIN_NOTHING:
				break;
			case CHAIN_PREPEND:
				feature.pNext = (VkBaseOutStructure *)r_next;
				r_next = &feature;
				break;
			case CHAIN_REPLACE:
				feature.pNext = nullptr;
				r_next = &feature;
				break;
			case CHAIN_CYCLE:
				feature.pNext = &feature;
				r_next = &feature;
				break;
		}
	}
	virtual void on_device_created(const DeviceInfo &p_info) override {
		created_calls++;
		created_info = p_info;
		if (call_order != nullptr) {
			call_order->push_back(name);
		}
	}
	virtual void on_device_destroying(VkDevice p_device) override {
		destroying_calls++;
		destroyed_device = p_device;
	}
	virtual ValidationFilterResult filter_validation_message(const VkDebugUtilsMessengerCallbackDataEXT *p_callback_data) override {
		filter_calls++;
		if (call_order != nullptr) {
			call_order->push_back(name);
		}
		return filter_result;
	}

	explicit MockProvider(const String &p_name) :
			name(p_name) {}
	virtual ~MockProvider() override {
		// A case that fails midway must not leave a dangling provider behind.
		VulkanExtensionProviders::unregister_provider(this);
	}
};

// Stands for the single VulkanHooks owner (OpenXR): it records the create infos it would forward
// to its runtime, and creates nothing.
class MockVulkanHooks : public VulkanHooks {
public:
	Vector<String> instance_extensions;
	Vector<String> device_extensions;
	const void *device_next = nullptr;

	virtual bool create_vulkan_instance(const VkInstanceCreateInfo *p_vulkan_create_info, VkInstance *r_instance) override {
		for (uint32_t i = 0; i < p_vulkan_create_info->enabledExtensionCount; i++) {
			instance_extensions.push_back(String::utf8(p_vulkan_create_info->ppEnabledExtensionNames[i]));
		}
		return true;
	}
	virtual bool get_physical_device(VkPhysicalDevice *r_device) override { return false; }
	virtual bool create_vulkan_device(const VkDeviceCreateInfo *p_device_create_info, VkDevice *r_device) override {
		for (uint32_t i = 0; i < p_device_create_info->enabledExtensionCount; i++) {
			device_extensions.push_back(String::utf8(p_device_create_info->ppEnabledExtensionNames[i]));
		}
		device_next = p_device_create_info->pNext;
		return true;
	}
	virtual void set_direct_queue_family_and_index(uint32_t p_queue_family_index, uint32_t p_queue_index) override {}
	virtual bool use_fragment_density_offsets() override { return false; }
	virtual void get_fragment_density_offsets(LocalVector<VkOffset2D> &r_offsets, const Vector2i &p_granularity) override {}
	virtual bool use_subsampled_images() override { return false; }
};

// What the drivers do after the merge: enable each requested name the driver exposes, in the
// order the driver lists them.
HashSet<CharString> enable_available(const HashMap<CharString, bool> &p_requested, const Vector<CharString> &p_available) {
	HashSet<CharString> enabled;
	for (const CharString &name : p_available) {
		if (p_requested.has(name)) {
			enabled.insert(name);
		}
	}
	return enabled;
}

LocalVector<const char *> name_pointers(const HashSet<CharString> &p_enabled) {
	LocalVector<const char *> pointers;
	for (const CharString &name : p_enabled) {
		pointers.push_back(name.ptr());
	}
	return pointers;
}

// Whether the name is requested with that required flag.
bool is_requested(const HashMap<CharString, bool> &p_requested, const CharString &p_name, bool p_required) {
	const bool *required = p_requested.getptr(p_name);
	return required != nullptr && *required == p_required;
}

bool same_requests(const HashMap<CharString, bool> &p_a, const HashMap<CharString, bool> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	HashMap<CharString, bool>::ConstIterator a = p_a.begin();
	HashMap<CharString, bool>::ConstIterator b = p_b.begin();
	for (; a != p_a.end(); ++a, ++b) {
		if (a->key != b->key || a->value != b->value) {
			return false;
		}
	}
	return true;
}

VkDebugUtilsMessengerCallbackDataEXT make_message(const VkDebugUtilsObjectNameInfoEXT *p_objects, uint32_t p_object_count) {
	VkDebugUtilsMessengerCallbackDataEXT data = {};
	data.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
	data.pMessageIdName = "VUID-test";
	data.pMessage = "A test message.";
	data.objectCount = p_object_count;
	data.pObjects = p_objects;
	return data;
}

VkDebugUtilsObjectNameInfoEXT make_object(VkObjectType p_type, uint64_t p_handle) {
	VkDebugUtilsObjectNameInfoEXT object = {};
	object.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
	object.objectType = p_type;
	object.objectHandle = p_handle;
	return object;
}

// Sets an environment variable, or clears it when the value is empty, for the length of a case and
// restores it afterwards, also when a REQUIRE ends the case early. The first lock of the registry
// reads GODOTNEXT_VK_TEST_DEVICE_EXTENSION, so a case that locks it must not depend on the shell
// it runs in.
class ScopedEnvironment {
	String name;
	String previous;
	bool had_previous = false;

public:
	ScopedEnvironment(const String &p_name, const String &p_value) :
			name(p_name) {
		const OS *os = OS::get_singleton();
		had_previous = os->has_environment(name);
		if (had_previous) {
			previous = os->get_environment(name);
		}
		if (p_value.is_empty()) {
			os->unset_environment(name);
		} else {
			os->set_environment(name, p_value);
		}
	}
	~ScopedEnvironment() {
		const OS *os = OS::get_singleton();
		if (had_previous) {
			os->set_environment(name, previous);
		} else {
			os->unset_environment(name);
		}
	}
};

} // namespace TestVkProviders

TEST_CASE("[Upscalers][VkProviders] Registration closes while a rendering context exists") {
	using namespace TestVkProviders;
	const ScopedEnvironment no_test_provider("GODOTNEXT_VK_TEST_DEVICE_EXTENSION", String());
	REQUIRE_FALSE(VulkanExtensionProviders::is_registration_locked());
	REQUIRE_EQ(VulkanExtensionProviders::get_provider_count(), 0u);

	MockProvider first("first");
	MockProvider second("second");
	CHECK(VulkanExtensionProviders::register_provider(&first) == OK);
	CHECK(VulkanExtensionProviders::register_provider(&second) == OK);
	ERR_PRINT_OFF;
	CHECK(VulkanExtensionProviders::register_provider(&first) == ERR_ALREADY_EXISTS);
	CHECK(VulkanExtensionProviders::register_provider(nullptr) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
	CHECK_EQ(VulkanExtensionProviders::get_provider_count(), 2u);

	// What RenderingContextDriverVulkan does from initialize() until it is destroyed.
	VulkanExtensionProviders::lock_registration();
	CHECK(VulkanExtensionProviders::is_registration_locked());
	MockProvider late("late");
	ERR_PRINT_OFF;
	CHECK(VulkanExtensionProviders::register_provider(&late) == ERR_LOCKED);
	ERR_PRINT_ON;
	CHECK_EQ(VulkanExtensionProviders::get_provider_count(), 2u);

	// Modules are uninitialized before the rendering device goes, so unregistering stays open.
	VulkanExtensionProviders::unregister_provider(&second);
	CHECK_EQ(VulkanExtensionProviders::get_provider_count(), 1u);

	// A second context keeps the registry closed until both are gone.
	VulkanExtensionProviders::lock_registration();
	VulkanExtensionProviders::unlock_registration();
	CHECK(VulkanExtensionProviders::is_registration_locked());
	VulkanExtensionProviders::unlock_registration();
	CHECK_FALSE(VulkanExtensionProviders::is_registration_locked());

	CHECK(VulkanExtensionProviders::register_provider(&late) == OK);
	CHECK_EQ(VulkanExtensionProviders::get_provider_count(), 2u);
	VulkanExtensionProviders::unregister_provider(&late);
	VulkanExtensionProviders::unregister_provider(&first);
	CHECK_EQ(VulkanExtensionProviders::get_provider_count(), 0u);
}

#ifdef DEBUG_ENABLED
TEST_CASE("[Upscalers][VkProviders] GODOTNEXT_VK_TEST_DEVICE_EXTENSION registers the test provider while a context exists") {
	using namespace TestVkProviders;
	REQUIRE_FALSE(VulkanExtensionProviders::is_registration_locked());
	REQUIRE_EQ(VulkanExtensionProviders::get_provider_count(), 0u);

	{
		// The first context to lock the registry registers it, and the last one to go removes it.
		const ScopedEnvironment test_extension("GODOTNEXT_VK_TEST_DEVICE_EXTENSION", "VK_GODOTNEXT_doctest");
		VulkanExtensionProviders::lock_registration();
		CHECK_EQ(VulkanExtensionProviders::get_provider_count(), 1u);
		HashMap<CharString, bool> requested;
		VulkanExtensionProviders::DeviceRequests provider_requests;
		VulkanExtensionProviders::merge_device_extensions(VK_NULL_HANDLE, VK_NULL_HANDLE, requested, provider_requests);
		CHECK_EQ(requested.size(), 1u);
		CHECK(is_requested(requested, "VK_GODOTNEXT_doctest", false));
		CHECK_EQ(provider_requests.size(), 1u);
		VulkanExtensionProviders::unlock_registration();
		CHECK_EQ(VulkanExtensionProviders::get_provider_count(), 0u);
	}

	{
		// A value that is not an extension name registers nothing.
		const ScopedEnvironment test_extension("GODOTNEXT_VK_TEST_DEVICE_EXTENSION", "not a name");
		ERR_PRINT_OFF;
		VulkanExtensionProviders::lock_registration();
		ERR_PRINT_ON;
		CHECK_EQ(VulkanExtensionProviders::get_provider_count(), 0u);
		VulkanExtensionProviders::unlock_registration();
	}

	CHECK_FALSE(VulkanExtensionProviders::is_registration_locked());
}
#endif // DEBUG_ENABLED

TEST_CASE("[Upscalers][VkProviders] Merged extension lists reach the VulkanHooks owner once each") {
	using namespace TestVkProviders;
	REQUIRE_EQ(VulkanExtensionProviders::get_provider_count(), 0u);

	const VkInstance instance = (VkInstance)(uintptr_t)0x1000;
	const VkPhysicalDevice physical_device = (VkPhysicalDevice)(uintptr_t)0x2000;
	const VkDevice device = (VkDevice)(uintptr_t)0x3000;

	// Godot's own requests, with the required flags the drivers register them with.
	HashMap<CharString, bool> godot_instance;
	godot_instance[VK_KHR_SURFACE_EXTENSION_NAME] = true;
	godot_instance[VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME] = false;
	godot_instance[VK_EXT_DEBUG_UTILS_EXTENSION_NAME] = false;
	HashMap<CharString, bool> godot_device;
	godot_device[VK_KHR_SWAPCHAIN_EXTENSION_NAME] = true;
	godot_device[VK_KHR_MULTIVIEW_EXTENSION_NAME] = false;
	godot_device[VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME] = false;

	// What the loader and the device expose: not the name the second provider asks for.
	const Vector<CharString> available_instance = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, "VK_KHR_display" };
	const Vector<CharString> available_device = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, "VK_NVX_image_view_handle", "VK_KHR_push_descriptor", "VK_EXT_memory_budget" };

	SUBCASE("No provider gives Godot's lists") {
		HashMap<CharString, bool> requested_instance(godot_instance);
		VulkanExtensionProviders::merge_instance_extensions(requested_instance);
		CHECK(same_requests(requested_instance, godot_instance));

		HashMap<CharString, bool> requested_device(godot_device);
		VulkanExtensionProviders::DeviceRequests provider_requests;
		VulkanExtensionProviders::merge_device_extensions(instance, physical_device, requested_device, provider_requests);
		CHECK(same_requests(requested_device, godot_device));
		CHECK(provider_requests.is_empty());

		VkBaseOutStructure godot_features = {};
		godot_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
		void *next = &godot_features;
		const HashSet<CharString> enabled_device = enable_available(requested_device, available_device);
		VulkanExtensionProviders::chain_device_features(physical_device, provider_requests, enabled_device, next);
		CHECK(next == &godot_features);

		MockVulkanHooks hooks;
		REQUIRE(VulkanHooks::get_singleton() == &hooks);
		const LocalVector<const char *> names = name_pointers(enabled_device);
		VkDeviceCreateInfo device_create_info = {};
		device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		device_create_info.pNext = next;
		device_create_info.enabledExtensionCount = names.size();
		device_create_info.ppEnabledExtensionNames = names.ptr();
		VkDevice created = VK_NULL_HANDLE;
		CHECK(VulkanHooks::get_singleton()->create_vulkan_device(&device_create_info, &created));
		const Vector<String> godot_names = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME };
		CHECK(hooks.device_extensions == godot_names);
		CHECK(hooks.device_next == &godot_features);
	}

	SUBCASE("Two providers") {
		// The first asks for a name Godot already requires, one Godot already has as optional
		// and two new ones. The second repeats one of the first's (and itself), and asks for a
		// name the device does not expose.
		MockProvider first("first");
		first.instance_extensions = { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME };
		first.device_extensions = { "VK_NVX_image_view_handle", VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_MULTIVIEW_EXTENSION_NAME, "VK_KHR_push_descriptor" };
		first.chain_mode = MockProvider::CHAIN_PREPEND;
		first.feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
		MockProvider second("second");
		second.instance_extensions = { VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, "VK_GODOTNEXT_unavailable_instance" };
		second.device_extensions = { "VK_KHR_push_descriptor", "VK_GODOTNEXT_unavailable", "VK_KHR_push_descriptor", "" };
		second.chain_mode = MockProvider::CHAIN_PREPEND;
		second.feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
		Vector<String> call_order;
		first.call_order = &call_order;
		second.call_order = &call_order;
		REQUIRE(VulkanExtensionProviders::register_provider(&first) == OK);
		REQUIRE(VulkanExtensionProviders::register_provider(&second) == OK);

		// Instance.
		HashMap<CharString, bool> requested_instance(godot_instance);
		VulkanExtensionProviders::merge_instance_extensions(requested_instance);
		CHECK_EQ(requested_instance.size(), godot_instance.size() + 2);
		// Godot's entries come first and keep their flags; the new ones are optional.
		{
			HashMap<CharString, bool>::ConstIterator merged = requested_instance.begin();
			for (const KeyValue<CharString, bool> &own : godot_instance) {
				CHECK(merged->key == own.key);
				CHECK(merged->value == own.value);
				++merged;
			}
		}
		CHECK(is_requested(requested_instance, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, false));
		CHECK(is_requested(requested_instance, "VK_GODOTNEXT_unavailable_instance", false));

		const HashSet<CharString> enabled_instance = enable_available(requested_instance, available_instance);
		MockVulkanHooks hooks;
		REQUIRE(VulkanHooks::get_singleton() == &hooks);
		const LocalVector<const char *> instance_names = name_pointers(enabled_instance);
		VkInstanceCreateInfo instance_create_info = {};
		instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instance_create_info.enabledExtensionCount = instance_names.size();
		instance_create_info.ppEnabledExtensionNames = instance_names.ptr();
		VkInstance created_instance = VK_NULL_HANDLE;
		CHECK(VulkanHooks::get_singleton()->create_vulkan_instance(&instance_create_info, &created_instance));
		const Vector<String> expected_instance = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME };
		CHECK(hooks.instance_extensions == expected_instance);

		// Device.
		HashMap<CharString, bool> requested_device(godot_device);
		VulkanExtensionProviders::DeviceRequests provider_requests;
		VulkanExtensionProviders::merge_device_extensions(instance, physical_device, requested_device, provider_requests);
		CHECK(first.seen_instance == instance);
		CHECK(first.seen_physical_device == physical_device);
		CHECK(second.seen_physical_device == physical_device);
		CHECK_EQ(requested_device.size(), godot_device.size() + 3);
		{
			HashMap<CharString, bool>::ConstIterator merged = requested_device.begin();
			for (const KeyValue<CharString, bool> &own : godot_device) {
				CHECK(merged->key == own.key);
				CHECK(merged->value == own.value);
				++merged;
			}
		}
		// Godot's required flag wins over the provider's optional request.
		CHECK(is_requested(requested_device, VK_KHR_SWAPCHAIN_EXTENSION_NAME, true));
		CHECK(is_requested(requested_device, "VK_NVX_image_view_handle", false));
		CHECK(is_requested(requested_device, "VK_KHR_push_descriptor", false));
		CHECK(is_requested(requested_device, "VK_GODOTNEXT_unavailable", false));
		CHECK_FALSE(requested_device.has(""));
		CHECK_EQ(provider_requests.size(), 2u);

		const HashSet<CharString> enabled_device = enable_available(requested_device, available_device);
		// The unavailable name is reported missing, once, to the provider that asked for it.
		CHECK(VulkanExtensionProviders::missing_for(first.device_extensions, enabled_device).is_empty());
		const Vector<CharString> expected_missing = { "VK_GODOTNEXT_unavailable" };
		CHECK(VulkanExtensionProviders::missing_for(second.device_extensions, enabled_device) == expected_missing);
		const Vector<CharString> expected_missing_instance = { "VK_GODOTNEXT_unavailable_instance" };
		CHECK(VulkanExtensionProviders::missing_for(second.instance_extensions, enabled_instance) == expected_missing_instance);

		// Only the provider with every extension enabled chains its feature structure.
		VkBaseOutStructure godot_features = {};
		godot_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
		void *next = &godot_features;
		VulkanExtensionProviders::chain_device_features(physical_device, provider_requests, enabled_device, next);
		CHECK_EQ(first.chain_calls, 1);
		CHECK_EQ(second.chain_calls, 0);
		CHECK(next == &first.feature);
		CHECK(first.feature.pNext == &godot_features);

		const LocalVector<const char *> device_names = name_pointers(enabled_device);
		VkDeviceCreateInfo device_create_info = {};
		device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		device_create_info.pNext = next;
		device_create_info.enabledExtensionCount = device_names.size();
		device_create_info.ppEnabledExtensionNames = device_names.ptr();
		VkDevice created_device = VK_NULL_HANDLE;
		CHECK(VulkanHooks::get_singleton()->create_vulkan_device(&device_create_info, &created_device));
		// Each name once, in the device's order, without the unavailable one.
		const Vector<String> expected_device = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, "VK_NVX_image_view_handle", "VK_KHR_push_descriptor" };
		CHECK(hooks.device_extensions == expected_device);
		CHECK(hooks.device_next == &first.feature);

		// The device and its main queue exist: both providers are told, in registration order.
		VulkanExtensionProvider::DeviceInfo info;
		info.instance = instance;
		info.physical_device = physical_device;
		info.device = device;
		info.queue_family_index = 2;
		info.queue_index = 1;
		info.enabled_instance_extensions = enabled_instance;
		info.enabled_device_extensions = enabled_device;
		VulkanExtensionProviders::notify_device_created(info);
		const Vector<String> expected_order = { "first", "second" };
		CHECK(call_order == expected_order);
		CHECK_EQ(second.created_calls, 1);
		CHECK(second.created_info.device == device);
		CHECK_EQ(second.created_info.queue_family_index, 2u);
		CHECK_EQ(second.created_info.queue_index, 1u);
		CHECK(second.created_info.enabled_device_extensions.has("VK_KHR_push_descriptor"));
		CHECK(VulkanExtensionProviders::missing_for(second.device_extensions, second.created_info.enabled_device_extensions) == expected_missing);

		VulkanExtensionProviders::notify_device_destroying(device);
		CHECK_EQ(first.destroying_calls, 1);
		CHECK(first.destroyed_device == device);
		CHECK(second.destroyed_device == device);

		// An unregistered provider gets no further calls.
		VulkanExtensionProviders::unregister_provider(&second);
		VulkanExtensionProviders::notify_device_destroying(device);
		CHECK_EQ(first.destroying_calls, 2);
		CHECK_EQ(second.destroying_calls, 1);
	}

	CHECK(VulkanHooks::get_singleton() == nullptr);
}

TEST_CASE("[Upscalers][VkProviders] A provider cannot break the device's pNext chain") {
	using namespace TestVkProviders;
	REQUIRE_EQ(VulkanExtensionProviders::get_provider_count(), 0u);

	const VkPhysicalDevice physical_device = (VkPhysicalDevice)(uintptr_t)0x2000;
	HashSet<CharString> enabled;
	VkBaseOutStructure godot_tail = {};
	godot_tail.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
	VkBaseOutStructure godot_head = {};
	godot_head.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
	godot_head.pNext = &godot_tail;

	MockProvider provider("provider");
	provider.feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
	REQUIRE(VulkanExtensionProviders::register_provider(&provider) == OK);
	VulkanExtensionProviders::DeviceRequests provider_requests;
	provider_requests[&provider] = Vector<CharString>();

	SUBCASE("A structure in front of the chain is kept") {
		provider.chain_mode = MockProvider::CHAIN_PREPEND;
		void *next = &godot_head;
		VulkanExtensionProviders::chain_device_features(physical_device, provider_requests, enabled, next);
		CHECK(next == &provider.feature);
		CHECK(provider.feature.pNext == &godot_head);
	}

	SUBCASE("A structure in front of an empty chain is kept") {
		provider.chain_mode = MockProvider::CHAIN_PREPEND;
		void *next = nullptr;
		VulkanExtensionProviders::chain_device_features(physical_device, provider_requests, enabled, next);
		CHECK(next == &provider.feature);
		CHECK(provider.feature.pNext == nullptr);
	}

	SUBCASE("Losing Godot's chain is undone") {
		provider.chain_mode = MockProvider::CHAIN_REPLACE;
		void *next = &godot_head;
		ERR_PRINT_OFF;
		VulkanExtensionProviders::chain_device_features(physical_device, provider_requests, enabled, next);
		ERR_PRINT_ON;
		CHECK_EQ(provider.chain_calls, 1);
		CHECK(next == &godot_head);
	}

	SUBCASE("A cycle is undone") {
		provider.chain_mode = MockProvider::CHAIN_CYCLE;
		void *next = &godot_head;
		ERR_PRINT_OFF;
		VulkanExtensionProviders::chain_device_features(physical_device, provider_requests, enabled, next);
		ERR_PRINT_ON;
		CHECK(next == &godot_head);
	}

	SUBCASE("A structure type Godot already chains is undone") {
		provider.chain_mode = MockProvider::CHAIN_PREPEND;
		provider.feature.sType = godot_tail.sType;
		void *next = &godot_head;
		ERR_PRINT_OFF;
		VulkanExtensionProviders::chain_device_features(physical_device, provider_requests, enabled, next);
		ERR_PRINT_ON;
		CHECK(next == &godot_head);
	}

	SUBCASE("A provider the device was not prepared with is not asked") {
		provider.chain_mode = MockProvider::CHAIN_PREPEND;
		void *next = &godot_head;
		VulkanExtensionProviders::chain_device_features(physical_device, VulkanExtensionProviders::DeviceRequests(), enabled, next);
		CHECK_EQ(provider.chain_calls, 0);
		CHECK(next == &godot_head);
	}

	VulkanExtensionProviders::unregister_provider(&provider);
}

TEST_CASE("[Upscalers][VkProviders] A message about a GodotNext handle reaches no provider") {
	using namespace TestVkProviders;
	REQUIRE_EQ(VulkanExtensionProviders::get_provider_count(), 0u);

	const uint64_t godot_image = 0x51000;
	const uint64_t foreign_image = 0x52000;
	const uint64_t shared_handle = 0x53000;

	const VkDebugUtilsObjectNameInfoEXT foreign_objects[2] = { make_object(VK_OBJECT_TYPE_DEVICE, shared_handle), make_object(VK_OBJECT_TYPE_IMAGE, foreign_image) };
	const VkDebugUtilsObjectNameInfoEXT godot_objects[3] = { make_object(VK_OBJECT_TYPE_DEVICE, shared_handle), make_object(VK_OBJECT_TYPE_IMAGE, foreign_image), make_object(VK_OBJECT_TYPE_IMAGE, godot_image) };
	const VkDebugUtilsMessengerCallbackDataEXT foreign_message = make_message(foreign_objects, 2);
	const VkDebugUtilsMessengerCallbackDataEXT godot_message = make_message(godot_objects, 3);

	String provider_name;
	// With no provider, nothing is allowlisted.
	CHECK_FALSE(VulkanExtensionProviders::filter_validation_message(&foreign_message, provider_name));

	MockProvider first("first");
	MockProvider second("second");
	second.filter_result = VulkanExtensionProvider::VALIDATION_ALLOWLISTED;
	Vector<String> call_order;
	first.call_order = &call_order;
	second.call_order = &call_order;
	REQUIRE(VulkanExtensionProviders::register_provider(&first) == OK);
	REQUIRE(VulkanExtensionProviders::register_provider(&second) == OK);

#ifdef DEBUG_ENABLED
	{
		const VkDebugUtilsObjectNameInfoEXT shared_objects[5] = { make_object(VK_OBJECT_TYPE_INSTANCE, shared_handle), make_object(VK_OBJECT_TYPE_PHYSICAL_DEVICE, shared_handle), make_object(VK_OBJECT_TYPE_DEVICE, shared_handle), make_object(VK_OBJECT_TYPE_QUEUE, shared_handle), make_object(VK_OBJECT_TYPE_COMMAND_BUFFER, shared_handle) };
		const VkDebugUtilsMessengerCallbackDataEXT shared_message = make_message(shared_objects, 5);
		const VkDebugUtilsMessengerCallbackDataEXT no_object_message = make_message(nullptr, 0);
		// The registry of a device driver that created one image and, as the tracked handle of
		// another type, a value that messages also carry as the device.
		VulkanCreatedHandles created_handles;
		created_handles.add(godot_image);
		created_handles.add(shared_handle);
		CHECK(VulkanCreatedHandles::is_created(godot_image));
		CHECK_FALSE(VulkanCreatedHandles::is_created(foreign_image));
		CHECK_EQ(created_handles.get_count(), 2u);

		// A message about someone else's objects: the providers are asked in order, and the
		// second allowlists it.
		CHECK(VulkanExtensionProviders::filter_validation_message(&foreign_message, provider_name));
		CHECK(provider_name == "second");
		const Vector<String> expected_order = { "first", "second" };
		CHECK(call_order == expected_order);

		// The same message with a GodotNext image among its objects reaches no provider.
		provider_name = String();
		CHECK_FALSE(VulkanExtensionProviders::filter_validation_message(&godot_message, provider_name));
		CHECK(provider_name.is_empty());
		CHECK_EQ(first.filter_calls, 1);
		CHECK_EQ(second.filter_calls, 1);

		// The instance, physical device, device, queue and command buffer are shared with the
		// providers' own calls: they never attribute a message, and neither does no object.
		CHECK(VulkanExtensionProviders::filter_validation_message(&shared_message, provider_name));
		CHECK(VulkanExtensionProviders::filter_validation_message(&no_object_message, provider_name));
		CHECK_EQ(second.filter_calls, 3);

		// Only the rendering context creates surfaces: a message about one reaches no provider,
		// though no device driver's registry holds it.
		const uint64_t surface_handle = 0x54000;
		CHECK_FALSE(VulkanCreatedHandles::is_created(surface_handle));
		const VkDebugUtilsObjectNameInfoEXT surface_objects[2] = { make_object(VK_OBJECT_TYPE_PHYSICAL_DEVICE, shared_handle), make_object(VK_OBJECT_TYPE_SURFACE_KHR, surface_handle) };
		const VkDebugUtilsMessengerCallbackDataEXT surface_message = make_message(surface_objects, 2);
		CHECK_FALSE(VulkanExtensionProviders::filter_validation_message(&surface_message, provider_name));
		CHECK_EQ(first.filter_calls, 3);
		CHECK_EQ(second.filter_calls, 3);

		// The first provider to allowlist ends the question.
		first.filter_result = VulkanExtensionProvider::VALIDATION_ALLOWLISTED;
		CHECK(VulkanExtensionProviders::filter_validation_message(&foreign_message, provider_name));
		CHECK(provider_name == "first");
		CHECK_EQ(second.filter_calls, 3);

		// A handle is in the registry until its last owner is gone.
		created_handles.add(godot_image);
		CHECK(created_handles.remove(godot_image));
		CHECK_FALSE(VulkanExtensionProviders::filter_validation_message(&godot_message, provider_name));
		CHECK(created_handles.remove(godot_image));
		CHECK_FALSE(created_handles.remove(godot_image));
		CHECK(VulkanExtensionProviders::filter_validation_message(&godot_message, provider_name));
		CHECK_EQ(created_handles.get_count(), 1u);
		created_handles.add(0);
		CHECK_EQ(created_handles.get_count(), 1u);
	}
	// The registry went with its driver.
	CHECK_FALSE(VulkanCreatedHandles::is_created(shared_handle));
#else
	// Without the created-handle registry (template_release) no message can be attributed, so no
	// provider is asked and nothing is allowlisted.
	CHECK_FALSE(VulkanExtensionProviders::filter_validation_message(&foreign_message, provider_name));
	CHECK_FALSE(VulkanExtensionProviders::filter_validation_message(&godot_message, provider_name));
	CHECK_EQ(first.filter_calls, 0);
	CHECK_EQ(second.filter_calls, 0);
#endif

	VulkanExtensionProviders::unregister_provider(&second);
	VulkanExtensionProviders::unregister_provider(&first);
}

#endif // VULKAN_ENABLED
