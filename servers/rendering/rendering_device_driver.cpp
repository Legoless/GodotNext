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

#include "servers/rendering/rendering_context_driver.h"

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

// GodotNext: rq_substrate (#22), rt_core_check (#283)
RenderingDeviceDriver::VulkanRayQueryCaps RenderingDeviceDriver::vulkan_ray_query_caps(bool p_acceleration_structure, bool p_ray_query, bool p_buffer_device_address, uint32_t p_vendor_id, uint32_t p_device_id, bool p_integrated) {
	VulkanRayQueryCaps caps;
	if (!p_acceleration_structure || !p_ray_query || !p_buffer_device_address) {
		caps.reason = RAY_QUERY_REASON_NO_RAY_QUERY;
		return caps;
	}
	const RenderingContextDriver::RayQueryHardwareCheck check = RenderingContextDriver::check_ray_query_hardware_device(p_vendor_id, p_device_id, p_integrated);
	caps.ray_query = true;
	caps.hardware = check.hardware;
	caps.hardware_note = check.note;
	caps.reason = RAY_QUERY_SUPPORTED;
	return caps;
}

// GodotNext: d3d12_dxr (#118), rt_core_check (#283)
RenderingDeviceDriver::D3D12RayQueryCaps RenderingDeviceDriver::d3d12_ray_query_caps(uint32_t p_raytracing_tier, uint32_t p_shader_model, bool p_ray_query_toolchain, uint32_t p_vendor_id, uint32_t p_device_id, bool p_integrated) {
	D3D12RayQueryCaps caps;
	// The first failing check wins.
	if (p_raytracing_tier < D3D12_RAY_QUERY_MIN_RAYTRACING_TIER) {
		caps.missing = "raytracing tier 1.1";
		return caps;
	}
	if (p_shader_model < D3D12_RAY_QUERY_MIN_SHADER_MODEL) {
		caps.missing = "shader model 6.5";
		return caps;
	}
	if (p_shader_model < D3D12_BUFFER_DEVICE_ADDRESS_MIN_SHADER_MODEL) {
		caps.missing = "shader model 6.6 (buffer device addresses)";
		return caps;
	}
	if (!p_ray_query_toolchain) {
		caps.missing = "a Mesa NIR library with ray query";
		return caps;
	}
	const RenderingContextDriver::RayQueryHardwareCheck check = RenderingContextDriver::check_ray_query_hardware_device(p_vendor_id, p_device_id, p_integrated);
	caps.ray_query = true;
	caps.hardware = check.hardware;
	caps.hardware_note = check.note;
	caps.reason = RAY_QUERY_SUPPORTED;
	return caps;
}

// GodotNext: d3d12_dxr (#118)
bool RenderingDeviceDriver::d3d12_acceleration_structure_vertex_format_supported(DataFormat p_format) {
	switch (p_format) {
		// Tier 1.0.
		case DATA_FORMAT_R32G32_SFLOAT:
		case DATA_FORMAT_R32G32B32_SFLOAT:
		case DATA_FORMAT_R16G16_SFLOAT:
		case DATA_FORMAT_R16G16B16A16_SFLOAT:
		case DATA_FORMAT_R16G16_SNORM:
		case DATA_FORMAT_R16G16B16A16_SNORM:
		// Tier 1.1. A2B10G10R10_PACK32 keeps R in the low bits, as DXGI_FORMAT_R10G10B10A2_UNORM does, and
		// A8B8G8R8_PACK32 has the bytes of R8G8B8A8.
		case DATA_FORMAT_R16G16B16A16_UNORM:
		case DATA_FORMAT_R16G16_UNORM:
		case DATA_FORMAT_A2B10G10R10_UNORM_PACK32:
		case DATA_FORMAT_R8G8B8A8_UNORM:
		case DATA_FORMAT_A8B8G8R8_UNORM_PACK32:
		case DATA_FORMAT_R8G8_UNORM:
		case DATA_FORMAT_R8G8B8A8_SNORM:
		case DATA_FORMAT_A8B8G8R8_SNORM_PACK32:
		case DATA_FORMAT_R8G8_SNORM:
			return true;
		default:
			return false;
	}
}

// GodotNext: rd_mesh_shaders (#178)
RenderingDeviceDriver::MeshShaderCaps RenderingDeviceDriver::metal_mesh_shader_caps(uint32_t p_apple_family, uint32_t p_msl_version, bool p_argument_buffers) {
	MeshShaderCaps caps;
	caps.mesh = p_apple_family >= METAL_MESH_SHADER_FAMILY && p_msl_version >= METAL_MESH_SHADER_MIN_MSL && p_argument_buffers;
	caps.indirect = caps.mesh && p_apple_family >= METAL_MESH_SHADER_INDIRECT_FAMILY;
	return caps;
}

// GodotNext: rd_mesh_shaders (#178)
RenderingDeviceDriver::MeshShaderCaps RenderingDeviceDriver::d3d12_mesh_shader_caps(uint32_t p_mesh_shader_tier, bool p_dxil_mesh_stages, String &r_log_line) {
	MeshShaderCaps caps;
	const String tier = p_mesh_shader_tier >= D3D12_MESH_SHADER_TIER_1 ? String("1") : String("none");
	if (p_mesh_shader_tier < D3D12_MESH_SHADER_TIER_1) {
		r_log_line = vformat("D3D12: Mesh shaders are off: the device has no mesh shader support (mesh shader tier %s).", tier);
	} else if (!p_dxil_mesh_stages) {
		r_log_line = vformat("D3D12: Mesh shaders are off: the shader compiler does not translate task and mesh shaders to DXIL yet (mesh shader tier %s).", tier);
	} else {
		caps.mesh = true;
		// ExecuteIndirect takes D3D12_DISPATCH_MESH_ARGUMENTS records, with a count buffer too.
		caps.indirect = true;
		r_log_line = vformat("D3D12: Mesh shaders are on (mesh shader tier %s).", tier);
	}
	return caps;
}

// GodotNext: rd_mesh_shaders (#178)
bool RenderingDeviceDriver::mesh_shader_limit_get(Limit p_limit, const MeshShaderLimits &p_limits, uint64_t &r_value) {
	switch (p_limit) {
		case LIMIT_MAX_TASK_WORKGROUP_COUNT_X:
		case LIMIT_MAX_TASK_WORKGROUP_COUNT_Y:
		case LIMIT_MAX_TASK_WORKGROUP_COUNT_Z:
			r_value = p_limits.max_task_workgroup_count[p_limit - LIMIT_MAX_TASK_WORKGROUP_COUNT_X];
			return true;
		case LIMIT_MAX_TASK_WORKGROUP_TOTAL_COUNT:
			r_value = p_limits.max_task_workgroup_total_count;
			return true;
		case LIMIT_MAX_MESH_WORKGROUP_COUNT_X:
		case LIMIT_MAX_MESH_WORKGROUP_COUNT_Y:
		case LIMIT_MAX_MESH_WORKGROUP_COUNT_Z:
			r_value = p_limits.max_mesh_workgroup_count[p_limit - LIMIT_MAX_MESH_WORKGROUP_COUNT_X];
			return true;
		case LIMIT_MAX_MESH_WORKGROUP_TOTAL_COUNT:
			r_value = p_limits.max_mesh_workgroup_total_count;
			return true;
		default:
			return false;
	}
}

// GodotNext: rd_mesh_shaders (#178)
RenderingDeviceDriver::MeshShaderLimits RenderingDeviceDriver::metal_mesh_shader_limits(uint32_t p_apple_family, bool p_mesh_shader) {
	MeshShaderLimits limits;
	if (!p_mesh_shader) {
		return limits;
	}
	for (uint64_t &count : limits.max_task_workgroup_count) {
		count = UINT32_MAX;
	}
	limits.max_task_workgroup_total_count = UINT32_MAX;
	uint64_t mesh_grid = 1024; // Apple7 and Apple8.
	if (p_apple_family >= 1010) {
		mesh_grid = 4194303;
	} else if (p_apple_family >= 1009) {
		mesh_grid = 1048575;
	}
	for (uint64_t &count : limits.max_mesh_workgroup_count) {
		count = mesh_grid;
	}
	limits.max_mesh_workgroup_total_count = mesh_grid;
	return limits;
}

// GodotNext: rd_mesh_shaders (#178)
RenderingDeviceDriver::MeshShaderLimits RenderingDeviceDriver::d3d12_mesh_shader_limits(bool p_mesh_shader) {
	MeshShaderLimits limits;
	if (!p_mesh_shader) {
		return limits;
	}
	for (uint32_t i = 0; i < 3; i++) {
		limits.max_task_workgroup_count[i] = 65535;
		limits.max_mesh_workgroup_count[i] = 65535;
	}
	limits.max_task_workgroup_total_count = uint64_t(1) << 22;
	limits.max_mesh_workgroup_total_count = uint64_t(1) << 22;
	return limits;
}

bool RenderingDeviceDriver::acceleration_structure_limit_get(Limit p_limit, const AccelerationStructureLimits &p_limits, uint64_t &r_value) {
	switch (p_limit) {
		case LIMIT_MAX_ACCELERATION_STRUCTURE_INSTANCES:
			r_value = p_limits.max_instances;
			return true;
		case LIMIT_MAX_ACCELERATION_STRUCTURE_GEOMETRIES:
			r_value = p_limits.max_geometries;
			return true;
		case LIMIT_MAX_ACCELERATION_STRUCTURE_PRIMITIVES:
			r_value = p_limits.max_primitives;
			return true;
		case LIMIT_MAX_PER_STAGE_ACCELERATION_STRUCTURES:
			r_value = p_limits.max_per_stage;
			return true;
		case LIMIT_MIN_ACCELERATION_STRUCTURE_SCRATCH_OFFSET_ALIGNMENT:
			r_value = p_limits.min_scratch_offset_alignment;
			return true;
		default:
			return false;
	}
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

// GodotNext: d3d12_dxr (#118)
String RenderingDeviceDriver::format_d3d12_driver_version_key(uint32_t p_vendor_id, uint32_t p_device_id, uint64_t p_driver_version) {
	const String version = vformat("%d.%d.%d.%d", (p_driver_version >> 48) & 0xFFFF, (p_driver_version >> 32) & 0xFFFF, (p_driver_version >> 16) & 0xFFFF, p_driver_version & 0xFFFF);
	return "D3D12:" + String::num_uint64(p_vendor_id) + ":" + String::num_uint64(p_device_id) + ":" + version;
}

// GodotNext: linux_driver_minimums (#285)
// Reads up to three dot-separated numbers at the start of p_text ("580.178.04", "24.2.8-1ubuntu1") and
// returns how many it read, 0 when p_text does not start with a digit. r_end is where they end.
static int _parse_driver_version_numbers(const String &p_text, uint32_t r_numbers[3], int &r_end) {
	int count = 0;
	int pos = 0;
	const int length = p_text.length();
	while (count < 3 && pos < length && is_digit(p_text[pos])) {
		uint64_t value = 0;
		while (pos < length && is_digit(p_text[pos])) {
			value = MIN(value * 10 + uint64_t(p_text[pos] - '0'), uint64_t(UINT32_MAX));
			pos++;
		}
		r_numbers[count++] = uint32_t(value);
		r_end = pos;
		if (count < 3 && pos + 1 < length && p_text[pos] == '.' && is_digit(p_text[pos + 1])) {
			pos++;
		} else {
			break;
		}
	}
	return count;
}

RenderingDeviceDriver::VulkanDriverMinimumCheck RenderingDeviceDriver::vulkan_linux_driver_minimum_check(uint32_t p_driver_id, const String &p_driver_info) {
	VulkanDriverMinimumCheck check;
	uint32_t numbers[3] = { 0, 0, 0 };
	int end = 0;
	String text = p_driver_info.strip_edges();
	if (p_driver_id == VULKAN_DRIVER_ID_NVIDIA_PROPRIETARY) {
		check.driver = "NVIDIA";
		check.minimum = itos(VULKAN_LINUX_NVIDIA_MIN_MAJOR);
		if (_parse_driver_version_numbers(text, numbers, end) == 0) {
			return check;
		}
		check.below_minimum = numbers[0] < VULKAN_LINUX_NVIDIA_MIN_MAJOR;
	} else if (p_driver_id == VULKAN_DRIVER_ID_MESA_RADV) {
		check.driver = "RADV";
		check.minimum = vformat("%d.%d.%d", VULKAN_LINUX_RADV_MIN_MAJOR, VULKAN_LINUX_RADV_MIN_MINOR, VULKAN_LINUX_RADV_MIN_PATCH);
		if (!text.begins_with("Mesa ")) {
			return check;
		}
		text = text.substr(5);
		// Mesa versions are major.minor.patch; a missing patch reads as 0.
		if (_parse_driver_version_numbers(text, numbers, end) < 2) {
			return check;
		}
		if (numbers[0] != VULKAN_LINUX_RADV_MIN_MAJOR) {
			check.below_minimum = numbers[0] < VULKAN_LINUX_RADV_MIN_MAJOR;
		} else if (numbers[1] != VULKAN_LINUX_RADV_MIN_MINOR) {
			check.below_minimum = numbers[1] < VULKAN_LINUX_RADV_MIN_MINOR;
		} else {
			check.below_minimum = numbers[2] < VULKAN_LINUX_RADV_MIN_PATCH;
		}
	} else {
		return check;
	}
	check.checked = true;
	check.version = text.substr(0, end);
	if (check.below_minimum) {
		check.warning = vformat("Vulkan: The %s driver %s is older than %s, the minimum supported version on Linux. Rendering continues, but updating the graphics driver is recommended.", check.driver, check.version, check.minimum);
	}
	return check;
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
