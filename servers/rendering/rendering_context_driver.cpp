/**************************************************************************/
/*  rendering_context_driver.cpp                                          */
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

#include "rendering_context_driver.h"

RenderingContextDriver::~RenderingContextDriver() {
}

RenderingContextDriver::SurfaceID RenderingContextDriver::surface_get_from_window(DisplayServerEnums::WindowID p_window) const {
	HashMap<DisplayServerEnums::WindowID, SurfaceID>::ConstIterator it = window_surface_map.find(p_window);
	if (it != window_surface_map.end()) {
		return it->value;
	} else {
		return SurfaceID();
	}
}

Error RenderingContextDriver::window_create(DisplayServerEnums::WindowID p_window, const void *p_platform_data) {
	SurfaceID surface = surface_create(p_platform_data);
	if (surface != 0) {
		window_surface_map[p_window] = surface;
		return OK;
	} else {
		return ERR_CANT_CREATE;
	}
}

void RenderingContextDriver::window_set_size(DisplayServerEnums::WindowID p_window, uint32_t p_width, uint32_t p_height) {
	SurfaceID surface = surface_get_from_window(p_window);
	if (surface) {
		surface_set_size(surface, p_width, p_height);
	}
}

void RenderingContextDriver::window_set_vsync_mode(DisplayServerEnums::WindowID p_window, DisplayServerEnums::VSyncMode p_vsync_mode) {
	SurfaceID surface = surface_get_from_window(p_window);
	if (surface) {
		surface_set_vsync_mode(surface, p_vsync_mode);
	}
}

DisplayServerEnums::VSyncMode RenderingContextDriver::window_get_vsync_mode(DisplayServerEnums::WindowID p_window) const {
	SurfaceID surface = surface_get_from_window(p_window);
	if (surface) {
		return surface_get_vsync_mode(surface);
	} else {
		return DisplayServerEnums::VSYNC_DISABLED;
	}
}

void RenderingContextDriver::window_set_hdr_output_enabled(DisplayServerEnums::WindowID p_window, bool p_enabled) {
	SurfaceID surface = surface_get_from_window(p_window);
	if (surface) {
		surface_set_hdr_output_enabled(surface, p_enabled);
	}
}

bool RenderingContextDriver::window_get_hdr_output_enabled(DisplayServerEnums::WindowID p_window) const {
	SurfaceID surface = surface_get_from_window(p_window);
	if (surface) {
		return surface_get_hdr_output_enabled(surface);
	} else {
		return false;
	}
}

void RenderingContextDriver::window_set_hdr_output_reference_luminance(DisplayServerEnums::WindowID p_window, float p_reference_luminance) {
	SurfaceID surface = surface_get_from_window(p_window);
	if (surface) {
		surface_set_hdr_output_reference_luminance(surface, p_reference_luminance);
	}
}

float RenderingContextDriver::window_get_hdr_output_reference_luminance(DisplayServerEnums::WindowID p_window) const {
	SurfaceID surface = surface_get_from_window(p_window);
	if (surface) {
		return surface_get_hdr_output_reference_luminance(surface);
	} else {
		return 0.0f;
	}
}

void RenderingContextDriver::window_set_hdr_output_max_luminance(DisplayServerEnums::WindowID p_window, float p_max_luminance) {
	SurfaceID surface = surface_get_from_window(p_window);
	if (surface) {
		surface_set_hdr_output_max_luminance(surface, p_max_luminance);
	}
}

float RenderingContextDriver::window_get_hdr_output_max_luminance(DisplayServerEnums::WindowID p_window) const {
	SurfaceID surface = surface_get_from_window(p_window);
	if (surface) {
		return surface_get_hdr_output_max_luminance(surface);
	} else {
		return 0.0f;
	}
}

void RenderingContextDriver::window_set_hdr_output_linear_luminance_scale(DisplayServerEnums::WindowID p_window, float p_linear_luminance_scale) {
	SurfaceID surface = surface_get_from_window(p_window);
	if (surface) {
		surface_set_hdr_output_linear_luminance_scale(surface, p_linear_luminance_scale);
	}
}

float RenderingContextDriver::window_get_hdr_output_linear_luminance_scale(DisplayServerEnums::WindowID p_window) const {
	SurfaceID surface = surface_get_from_window(p_window);
	if (surface) {
		return surface_get_hdr_output_linear_luminance_scale(surface);
	} else {
		return 0.0f;
	}
}

float RenderingContextDriver::window_get_output_max_linear_value(DisplayServerEnums::WindowID p_window) const {
	SurfaceID surface = surface_get_from_window(p_window);
	if (surface) {
		if (surface_get_hdr_output_enabled(surface)) {
			return surface_get_hdr_output_max_value(surface);
		}
	}
	return 1.0f; // SDR
}

void RenderingContextDriver::window_destroy(DisplayServerEnums::WindowID p_window) {
	SurfaceID surface = surface_get_from_window(p_window);
	if (surface) {
		surface_destroy(surface);
	}

	window_surface_map.erase(p_window);
}

String RenderingContextDriver::get_driver_and_device_memory_report() const {
	String report;

	const uint32_t num_tracked_obj_types = static_cast<uint32_t>(get_tracked_object_type_count());

	report += "=== Driver Memory Report ===";

	report += "\nLaunch with --extra-gpu-memory-tracking and build with "
			  "DEBUG_ENABLED for this functionality to work.";
	report += "\nDevice memory may be unavailable if the API does not support it"
			  "(e.g. VK_EXT_device_memory_report is unsupported).";
	report += "\n";

	report += "\nTotal Driver Memory:";
	report += String::num_real(double(get_driver_total_memory()) / (1024.0 * 1024.0));
	report += " MB";
	report += "\nTotal Driver Num Allocations: ";
	report += String::num_uint64(get_driver_allocation_count());

	report += "\nTotal Device Memory:";
	report += String::num_real(double(get_device_total_memory()) / (1024.0 * 1024.0));
	report += " MB";
	report += "\nTotal Device Num Allocations: ";
	report += String::num_uint64(get_device_allocation_count());

	report += "\n\nMemory use by object type (CSV format):";
	report += "\n\nCategory; Driver memory in MB; Driver Allocation Count; "
			  "Device memory in MB; Device Allocation Count";

	for (uint32_t i = 0u; i < num_tracked_obj_types; ++i) {
		report += "\n";
		report += get_tracked_object_name(i);
		report += ";";
		report += String::num_real(double(get_driver_memory_by_object_type(i)) / (1024.0 * 1024.0));
		report += ";";
		report += String::num_uint64(get_driver_allocs_by_object_type(i));
		report += ";";
		report += String::num_real(double(get_device_memory_by_object_type(i)) / (1024.0 * 1024.0));
		report += ";";
		report += String::num_uint64(get_device_allocs_by_object_type(i));
	}

	return report;
}

const char *RenderingContextDriver::get_tracked_object_name(uint32_t p_type_index) const {
	return "Tracking Unsupported by API";
}

uint64_t RenderingContextDriver::get_tracked_object_type_count() const {
	return 0;
}

uint64_t RenderingContextDriver::get_driver_total_memory() const {
	return 0;
}

uint64_t RenderingContextDriver::get_driver_allocation_count() const {
	return 0;
}

uint64_t RenderingContextDriver::get_driver_memory_by_object_type(uint32_t) const {
	return 0;
}

uint64_t RenderingContextDriver::get_driver_allocs_by_object_type(uint32_t) const {
	return 0;
}

uint64_t RenderingContextDriver::get_device_total_memory() const {
	return 0;
}

uint64_t RenderingContextDriver::get_device_allocation_count() const {
	return 0;
}

uint64_t RenderingContextDriver::get_device_memory_by_object_type(uint32_t) const {
	return 0;
}

uint64_t RenderingContextDriver::get_device_allocs_by_object_type(uint32_t) const {
	return 0;
}

// GodotNext: rq_substrate (#22). The RT-core allowlist: contiguous device-ID blocks of parts with
// hardware ray-tracing units, from the PCI ID database (pci-ids.ucw.cz, version 2026.11.13). Each
// row names what it covers. A block is split where a part without RT units sits inside it (GTX 16
// and CMP parts on Turing and Ampere dies, the MX570), and compute-only dies are left out (GA100,
// GH100, GB100, GB102). Rows not checked on hardware: everything except the RTX 4080 SUPER
// (0x10DE:0x2702) of the test fleet. Mid-range and other-vendor parts ship enabled but unvalidated
// (plan §23.2).
namespace {
struct RayQueryHardwareRange {
	uint32_t vendor;
	uint32_t first_id;
	uint32_t last_id;
	const char *architecture;
};

const RayQueryHardwareRange RAY_QUERY_HARDWARE_RANGES[] = {
	// NVIDIA Turing. TU102: TITAN RTX, GeForce RTX 2080 Ti (0x1E02-0x1E07; 0x1E09 is CMP 50HX).
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x1E02, 0x1E07, "Turing TU102" },
	// TU102: RTX 2080 Ti engineering samples, Quadro RTX 6000/8000, Tesla T10/T40.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x1E2D, 0x1E78, "Turing TU102" },
	// TU104: GeForce RTX 2080 (SUPER), 2070 SUPER, 2060, mobile parts, Quadro RTX 4000/5000, Tesla T4.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x1E81, 0x1EF5, "Turing TU104" },
	// TU106: GeForce RTX 2070, 2060 (0x1F09 GTX 1660 SUPER, 0x1F0A GTX 1650, 0x1F0B CMP 40HX follow).
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x1F02, 0x1F08, "Turing TU106" },
	// TU106: GeForce RTX 2070/2060 mobile, RTX 2060 SUPER, Quadro RTX 3000 mobile. TU116 and TU117
	// (GTX 16, MX, Quadro T) have no RT units.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x1F10, 0x1F76, "Turing TU106" },
	// NVIDIA Ampere. GA102: GeForce RTX 3090 Ti, 3090, 3080 Ti, 3080, 3070 Ti (0x220D is CMP 90HX).
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2203, 0x220A, "Ampere GA102" },
	// GA102: RTX 3080 LHR, engineering samples, RTX A6000/A5500/A5000/A4500, A40, A10/A10G/A10M.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2216, 0x223F, "Ampere GA102" },
	// GA103: GeForce RTX 3060 Ti, RTX 3080 Ti mobile, RTX A5500 laptop.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2414, 0x2460, "Ampere GA103" },
	// GA104: GeForce RTX 3070 Ti, 3070, 3060 Ti, 3060 (0x248A is CMP 70HX).
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2482, 0x2489, "Ampere GA104" },
	// GA104: RTX 3070 Ti/3070/3060 Ti, mobile and laptop parts, RTX A4000/A3000/A4500.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x248C, 0x24FA, "Ampere GA104" },
	// GA106: GeForce RTX 3060, 3050, mobile parts, RTX A2000.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2501, 0x2571, "Ampere GA106" },
	// GA107: GeForce RTX 3050 desktop and mobile (0x25A6/0x25A7 are MX570).
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2582, 0x25A5, "Ampere GA107" },
	// GA107: GeForce RTX 2050 (0x25AA is MX570 A).
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x25A9, 0x25A9, "Ampere GA107" },
	// GA107: RTX 3050 laptops, RTX 2050, RTX A1000/A400/A500/A2000 mobile, A2/A16, embedded parts.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x25AB, 0x25FB, "Ampere GA107" },
	// NVIDIA Ada Lovelace. AD102: RTX TITAN Ada, GeForce RTX 4090, RTX 6000/5000 Ada, L40, L20.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2681, 0x26F5, "Ada AD102" },
	// AD103: GeForce RTX 4080 SUPER, 4080, 4070 Ti SUPER, RTX 4090 laptop, RTX 5000 Ada laptop.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2702, 0x2770, "Ada AD103" },
	// AD104: GeForce RTX 4070 Ti, 4070 SUPER, 4070, 4060 Ti, RTX 4000/4500 Ada, L4.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2782, 0x27FB, "Ada AD104" },
	// AD106: GeForce RTX 4060 Ti, 4060, RTX 4070 mobile, RTX 3000 Ada laptop.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2803, 0x2878, "Ada AD106" },
	// AD107: GeForce RTX 4060, RTX 4050 mobile, RTX 2000/1000/500 Ada.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2882, 0x28F8, "Ada AD107" },
	// NVIDIA Blackwell. GB202: GeForce RTX 5090, 5090 D, RTX PRO 6000/5000/5500 Blackwell.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2B85, 0x2BBD, "Blackwell GB202" },
	// GB203: GeForce RTX 5080, 5070 Ti, RTX 5090/5080 mobile, RTX PRO 4000/4500/5000 Blackwell.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2C02, 0x2C79, "Blackwell GB203" },
	// GB206: GeForce RTX 5060 Ti, 5060, RTX 5070 mobile, RTX PRO 2000 Blackwell.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2D04, 0x2D79, "Blackwell GB206" },
	// GB207: GeForce RTX 5050, RTX PRO 1000/500 Blackwell.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2D83, 0x2DF9, "Blackwell GB207" },
	// GB205: GeForce RTX 5070 SUPER, 5070, 5060, RTX 5070 Ti mobile, RTX PRO 3000 Blackwell laptop.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2F02, 0x2F58, "Blackwell GB205" },

	// AMD RDNA2. Navi 21: Radeon RX 6950 XT, 6900 XT, 6800 (XT), Radeon Pro W6800/W6900X, V620.
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x73A1, 0x73BF, "RDNA2 Navi 21" },
	// Navi 22: Radeon RX 6700/6700 XT/6750 XT, 6800M/6850M XT.
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x73C3, 0x73DF, "RDNA2 Navi 22" },
	// Navi 23: Radeon RX 6650 XT, 6600 (XT), Radeon PRO W6600; 0x73F0 is Navi 33 (RDNA3), RX 7600M XT.
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x73E0, 0x73FF, "RDNA2 Navi 23" },
	// Navi 24: Radeon RX 6500 XT, 6400, 6300, Radeon PRO W6400/W6300.
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x7421, 0x743F, "RDNA2 Navi 24" },
	// AMD RDNA3. Navi 31: Radeon RX 7900 XTX/XT/GRE, Radeon Pro W7900/W7800.
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x7448, 0x745E, "RDNA3 Navi 31" },
	// Navi 32: Radeon RX 7800 XT, 7700 XT, Radeon PRO W7700, V710.
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x7460, 0x747E, "RDNA3 Navi 32" },
	// Navi 33: Radeon RX 7600 (XT), 7500, 7400, Radeon Pro W7600/W7500/W7400.
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x7480, 0x749F, "RDNA3 Navi 33" },
	// AMD RDNA4. Navi 48: Radeon RX 9070 XT/9070/9070 GRE, Radeon AI PRO R9700.
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x7550, 0x7551, "RDNA4 Navi 48" },
	// Navi 44: Radeon RX 9060 XT, 9050.
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x7590, 0x7590, "RDNA4 Navi 44" },
	// AMD APUs, RDNA2: Mendocino (Radeon 610M), Van Gogh, Rembrandt (Radeon 660M/680M), Raphael and
	// Granite Ridge (Radeon 610M).
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x1506, 0x1506, "RDNA2 Mendocino" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x163F, 0x163F, "RDNA2 Van Gogh" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x164D, 0x164E, "RDNA2 Rembrandt, Raphael" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x1681, 0x1681, "RDNA2 Rembrandt" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x13C0, 0x13C0, "RDNA2 Granite Ridge" },
	// AMD APUs, RDNA3 and 3.5: Phoenix and Hawk Point (Radeon 740M/760M/780M), Strix (Radeon
	// 880M/890M), Strix Halo (Radeon 8050S/8060S), Krackan (Radeon 820M/840M/860M).
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x15BF, 0x15BF, "RDNA3 Phoenix" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x15C8, 0x15C8, "RDNA3 Phoenix" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x164F, 0x164F, "RDNA3 Phoenix" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x1900, 0x1902, "RDNA3 Hawk Point, RDNA3.5 Krackan" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x150E, 0x150E, "RDNA3.5 Strix" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x1586, 0x1586, "RDNA3.5 Strix Halo" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x1114, 0x1114, "RDNA3.5 Krackan" },

	// Intel Xe-HPG. DG2: Arc A770M/A730M/A550M/A370M/A350M/A570M/A530M.
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x5690, 0x5698, "Xe-HPG DG2" },
	// DG2: Arc A770, A750, A580, A380, A310, Arc Pro A30M/A40/A50/A60, embedded parts.
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x56A0, 0x56BF, "Xe-HPG DG2" },
	// Intel Xe2. Lunar Lake: Arc 130V/140V.
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x6420, 0x6420, "Xe2 Lunar Lake" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x64A0, 0x64A0, "Xe2 Lunar Lake" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x64B0, 0x64B0, "Xe2 Lunar Lake" },
	// Battlemage G21 and G31: Arc B580, B570, Arc Pro B50/B60/B65/B70.
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0xE202, 0xE223, "Xe2 Battlemage" },
};
} // namespace

bool RenderingContextDriver::is_ray_query_hardware_device(uint32_t p_vendor_id, uint32_t p_device_id) {
	for (const RayQueryHardwareRange &range : RAY_QUERY_HARDWARE_RANGES) {
		if (range.vendor == p_vendor_id && p_device_id >= range.first_id && p_device_id <= range.last_id) {
			return true;
		}
	}
	return false;
}
