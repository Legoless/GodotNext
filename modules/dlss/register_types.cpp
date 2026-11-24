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
#include "dlss_frame_generation.h"
#include "dlss_ngx_d3d12.h"
#include "dlss_streamline_d3d12.h"
#include "dlss_upscaler.h"

#ifdef VULKAN_ENABLED
#include "dlss_vulkan_extensions.h"
#endif

#ifdef D3D12_ENABLED
#include "drivers/d3d12/d3d12_hooks.h"
#endif

#ifdef TOOLS_ENABLED
void dlss_register_editor_plugins();
#endif

#include "core/error/error_macros.h"
#include "core/object/class_db.h"
#include "core/os/memory.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/effects/temporal_upscaler.h"

static DLSSBackend *dlss_active_backend = nullptr;
static DLSSBackend *dlss_d3d12_backend = nullptr;
static DLSSUpscaler *dlss_upscaler = nullptr;
static Ref<DLSSFrameGeneration> dlss_frame_generation;
#ifdef VULKAN_ENABLED
static DLSSVulkanExtensionProvider *dlss_vk_provider = nullptr;

static void _dlss_on_vk_device_destroying(void *p_userdata) {
	DLSSUpscaler *upscaler = static_cast<DLSSUpscaler *>(p_userdata);
	if (upscaler != nullptr) {
		upscaler->shutdown();
	}
}
#endif

#ifdef D3D12_ENABLED
static void _dlss_on_d3d12_device_destroying(ID3D12Device *p_device, void *p_userdata) {
	(void)p_device;
	DLSSUpscaler *upscaler = static_cast<DLSSUpscaler *>(p_userdata);
	if (upscaler != nullptr) {
		upscaler->shutdown();
	}
}
#endif

DLSSBackend *dlss_get_active_backend() {
#ifdef D3D12_ENABLED
	if (dlss_d3d12_backend != nullptr && OS::get_singleton() != nullptr && OS::get_singleton()->get_current_rendering_driver_name() == "d3d12") {
		return dlss_d3d12_backend;
	}
#endif
	return dlss_active_backend != nullptr ? dlss_active_backend : dlss_d3d12_backend;
}

void initialize_dlss_module(ModuleInitializationLevel p_level) {
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		dlss_register_editor_plugins();
		return;
	}
#endif

	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

#ifdef DLSS_DEV_RUNTIME
	WARN_PRINT_ONCE("DLSS dev runtime build is active: local debugging only; never distribute or export dev runtimes.");
#endif

	if (dlss_active_backend == nullptr && dlss_d3d12_backend == nullptr) {
#ifdef DLSS_BACKEND_NGX
#ifdef VULKAN_ENABLED
		dlss_active_backend = dlss_backend_create_ngx();
#endif
#ifdef D3D12_ENABLED
		dlss_d3d12_backend = dlss_backend_create_ngx_d3d12();
#endif
#else
		dlss_active_backend = memnew(DLSSBackendMock);
#endif
	}

	if (dlss_upscaler == nullptr) {
		dlss_upscaler = memnew(DLSSUpscaler(dlss_active_backend, dlss_d3d12_backend));
		TemporalUpscalerRegistry::register_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS, dlss_upscaler);
	}

#ifdef D3D12_ENABLED
	if (dlss_d3d12_backend != nullptr && !D3D12Hooks::is_registration_locked()) {
		D3D12Hooks::set_device_destroying_callback(_dlss_on_d3d12_device_destroying, dlss_upscaler);
	}
#endif

	// #209 / #200: Initialize Streamline on D3D12 (game-only, when FG or Reflex is requested)
	// before DisplayServer::create, and register the DLSS-G FrameGenerationExtension provider.
	GDREGISTER_CLASS(DLSSFrameGeneration);
	const bool sl_loaded = dlss_streamline_init_if_requested();
	if (sl_loaded && dlss_frame_generation.is_null()) {
		dlss_frame_generation.instantiate();
		dlss_frame_generation->set_backend_ops(dlss_streamline_get_fg_ops());
		FrameGenerationRegistry::register_extension(dlss_frame_generation);
	}

#ifdef VULKAN_ENABLED
	if (dlss_vk_provider == nullptr && dlss_active_backend != nullptr) {
		dlss_vk_provider = memnew(DLSSVulkanExtensionProvider(dlss_active_backend));
		dlss_vk_provider->set_device_destroying_callback(_dlss_on_vk_device_destroying, dlss_upscaler);
		dlss_upscaler->set_vulkan_extension_provider(dlss_vk_provider);
		VulkanExtensionProviders::register_provider(dlss_vk_provider);
	}
#endif
}

void uninitialize_dlss_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		if (dlss_frame_generation.is_valid()) {
			FrameGenerationRegistry::unregister_extension(dlss_frame_generation->get_instance_id());
			dlss_frame_generation.unref();
		}
		if (dlss_upscaler != nullptr) {
			TemporalUpscalerRegistry::unregister_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
		}
		// Keep dlss_upscaler, backends, Streamline, and dlss_vk_provider alive until
		// finalize_display() finishes destroying RD command pools and invokes
		// on_device_destroying() before device destruction; final cleanup runs at CORE.
		return;
	}

	if (p_level == MODULE_INITIALIZATION_LEVEL_CORE) {
#ifdef D3D12_ENABLED
		if (!D3D12Hooks::is_registration_locked()) {
			D3D12Hooks::set_device_destroying_callback(nullptr, nullptr);
		}
#endif
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
		if (dlss_d3d12_backend != nullptr) {
			dlss_d3d12_backend->shutdown();
			memdelete(dlss_d3d12_backend);
			dlss_d3d12_backend = nullptr;
		}
		if (dlss_active_backend != nullptr) {
			dlss_active_backend->shutdown();
			memdelete(dlss_active_backend);
			dlss_active_backend = nullptr;
		}
		dlss_streamline_shutdown();
	}
}
