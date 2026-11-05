/**************************************************************************/
/*  rendering_device_driver.cpp                                           */
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

#include "rendering_device_driver.h"

/**************/
/**** MISC ****/
/**************/

uint64_t RenderingDeviceDriver::command_buffer_get_native_handle(CommandBufferID p_cmd_buffer) {
	return 0;
}

Error RenderingDeviceDriver::texture_get_native_info(TextureID p_texture, TextureNativeInfo &r_info) {
	return ERR_UNAVAILABLE;
}

uint64_t RenderingDeviceDriver::api_trait_get(ApiTrait p_trait) {
	// Sensible canonical defaults.
	switch (p_trait) {
		case API_TRAIT_HONORS_PIPELINE_BARRIERS:
			return 1;
		case API_TRAIT_SHADER_CHANGE_INVALIDATION:
			return SHADER_CHANGE_INVALIDATION_ALL_BOUND_UNIFORM_SETS;
		case API_TRAIT_TEXTURE_TRANSFER_ALIGNMENT:
			return 1;
		case API_TRAIT_TEXTURE_DATA_ROW_PITCH_STEP:
			return 1;
		case API_TRAIT_SECONDARY_VIEWPORT_SCISSOR:
			return 1;
		case API_TRAIT_BUFFER_CLEARS_WITH_COPY_ENGINE:
			return true;
		case API_TRAIT_TEXTURE_CLEARS_WITH_COPY_ENGINE:
			return true;
		case API_TRAIT_USE_GENERAL_IN_COPY_QUEUES:
			return false;
		case API_TRAIT_BUFFERS_REQUIRE_TRANSITIONS:
			return false;
		case API_TRAIT_TEXTURES_REQUIRE_LAYOUT_TRANSITIONS:
			return true;
		case API_TRAIT_TEXTURE_OUTPUTS_REQUIRE_CLEARS:
			return false;
		default:
			ERR_FAIL_V(0);
	}
}

// GodotNext: metal_ray_query
RenderingDeviceDriver::MetalRayQueryCaps RenderingDeviceDriver::metal_ray_query_caps(uint32_t p_apple_family, bool p_is_macos, uint32_t p_os_major, bool p_supports_raytracing, uint32_t p_msl_version, bool p_gpu_address, bool p_barriers, bool p_argument_buffers) {
	MetalRayQueryCaps caps;
	// The first failing check wins.
	if (!p_is_macos || p_os_major < METAL_RAY_QUERY_MIN_OS_MAJOR || !p_supports_raytracing || p_msl_version < METAL_RAY_QUERY_MIN_MSL || !p_gpu_address) {
		caps.reason = RAY_QUERY_REASON_NO_RAY_QUERY;
		return caps;
	}
	if (!p_barriers) {
		caps.reason = RAY_QUERY_REASON_METAL_SYNC_MODE;
		return caps;
	}
	if (!p_argument_buffers) {
		caps.reason = RAY_QUERY_REASON_METAL_NO_ARGUMENT_BUFFERS;
		return caps;
	}
	caps.ray_query = true;
	caps.hardware = p_apple_family >= METAL_RAY_QUERY_HARDWARE_FAMILY;
	caps.reason = RAY_QUERY_SUPPORTED;
	return caps;
}

/******************/

void RenderingDeviceDriver::command_begin_compute_pass(CommandBufferID p_cmd_buffer) {
}

void RenderingDeviceDriver::command_end_compute_pass(CommandBufferID p_cmd_buffer) {
}

// GodotNext: hwrt_safety_net
String RenderingDeviceDriver::format_driver_version_key(const String &p_vendor, const String &p_device_name, const String &p_api_version) {
	return p_vendor + "|" + p_device_name + "|" + p_api_version;
}

String RenderingDeviceDriver::format_vulkan_driver_version_key(uint32_t p_vendor_id, uint32_t p_device_id, uint32_t p_driver_version) {
	return String::num_uint64(p_vendor_id) + ":" + String::num_uint64(p_device_id) + ":" + String::num_uint64(p_driver_version);
}

void RenderingDeviceDriver::set_driver_version_identity(const String &p_vendor, const String &p_device_name) {
	driver_version_vendor = p_vendor;
	driver_version_device = p_device_name;
}

String RenderingDeviceDriver::get_driver_version_key() const {
	return format_driver_version_key(driver_version_vendor, driver_version_device, get_api_version());
}

void RenderingDeviceDriver::set_device_lost_callback(DeviceLostCallback p_callback, void *p_userdata) {
	device_lost_callback = p_callback;
	device_lost_userdata = p_userdata;
}

void RenderingDeviceDriver::notify_device_lost() {
	if (device_lost_callback != nullptr) {
		device_lost_callback(device_lost_userdata);
	}
}

RenderingDeviceDriver::~RenderingDeviceDriver() {}
