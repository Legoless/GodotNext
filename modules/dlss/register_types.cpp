/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "dlss_backend.h"
#include "dlss_backend_mock.h"
#include "dlss_upscaler.h"

#ifdef VULKAN_ENABLED
#include "dlss_vulkan_extensions.h"
#endif

#include "core/error/error_macros.h"
#include "core/os/memory.h"
#include "servers/rendering/renderer_rd/effects/temporal_upscaler.h"

static DLSSBackend *dlss_active_backend = nullptr;
static DLSSUpscaler *dlss_upscaler = nullptr;
#ifdef VULKAN_ENABLED
static DLSSVulkanExtensionProvider *dlss_vk_provider = nullptr;

static void _dlss_on_vk_device_destroying(void *p_userdata) {
	DLSSUpscaler *upscaler = static_cast<DLSSUpscaler *>(p_userdata);
	if (upscaler != nullptr) {
		upscaler->shutdown();
	}
}
#endif

DLSSBackend *dlss_get_active_backend() {
	return dlss_active_backend;
}

void initialize_dlss_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

#ifdef DLSS_DEV_RUNTIME
	WARN_PRINT_ONCE("DLSS dev runtime build is active: local debugging only; never distribute or export dev runtimes.");
#endif

	if (dlss_active_backend == nullptr) {
#ifdef DLSS_BACKEND_NGX
		dlss_active_backend = dlss_backend_create_ngx();
#else
		dlss_active_backend = memnew(DLSSBackendMock);
#endif
	}

	if (dlss_upscaler == nullptr) {
		dlss_upscaler = memnew(DLSSUpscaler(dlss_active_backend));
		TemporalUpscalerRegistry::register_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS, dlss_upscaler);
	}

#ifdef VULKAN_ENABLED
	if (dlss_vk_provider == nullptr) {
		dlss_vk_provider = memnew(DLSSVulkanExtensionProvider(dlss_active_backend));
		dlss_vk_provider->set_device_destroying_callback(_dlss_on_vk_device_destroying, dlss_upscaler);
		dlss_upscaler->set_vulkan_extension_provider(dlss_vk_provider);
		VulkanExtensionProviders::register_provider(dlss_vk_provider);
	}
#endif
}

void uninitialize_dlss_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		if (dlss_upscaler != nullptr) {
			TemporalUpscalerRegistry::unregister_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
		}
		// Keep dlss_upscaler, dlss_active_backend, and dlss_vk_provider alive until
		// finalize_display() finishes destroying RD command pools and invokes
		// on_device_destroying() before vkDestroyDevice; final cleanup runs at CORE.
		return;
	}

	if (p_level == MODULE_INITIALIZATION_LEVEL_CORE) {
#ifdef VULKAN_ENABLED
		if (dlss_vk_provider != nullptr) {
			dlss_vk_provider->set_device_destroying_callback(nullptr, nullptr);
			dlss_vk_provider->set_backend(nullptr);
			VulkanExtensionProviders::unregister_provider(dlss_vk_provider);
			memdelete(dlss_vk_provider);
			dlss_vk_provider = nullptr;
		}
#endif
		if (dlss_upscaler != nullptr) {
			dlss_upscaler->shutdown();
			memdelete(dlss_upscaler);
			dlss_upscaler = nullptr;
		}
		if (dlss_active_backend != nullptr) {
			dlss_active_backend->shutdown();
			memdelete(dlss_active_backend);
			dlss_active_backend = nullptr;
		}
	}
}
