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

#include "core/os/mutex.h"
#include "core/templates/local_vector.h"

namespace {
struct ExtraMemoryReportSource {
	String category;
	RenderingContextDriver::MemoryReportBytesCallback callback = nullptr;
	void *userdata = nullptr;
};

Mutex extra_memory_report_mutex;
LocalVector<ExtraMemoryReportSource> extra_memory_report_sources;
} // namespace

void RenderingContextDriver::add_memory_report_source(const String &p_category, MemoryReportBytesCallback p_callback, void *p_userdata) {
	ERR_FAIL_COND(p_category.is_empty() || p_callback == nullptr);
	MutexLock lock(extra_memory_report_mutex);
	for (ExtraMemoryReportSource &src : extra_memory_report_sources) {
		if (src.category == p_category) {
			src.callback = p_callback;
			src.userdata = p_userdata;
			return;
		}
	}
	extra_memory_report_sources.push_back({ p_category, p_callback, p_userdata });
}

void RenderingContextDriver::remove_memory_report_source(const String &p_category) {
	MutexLock lock(extra_memory_report_mutex);
	for (uint32_t i = 0; i < extra_memory_report_sources.size(); i++) {
		if (extra_memory_report_sources[i].category == p_category) {
			extra_memory_report_sources.remove_at(i);
			return;
		}
	}
}

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

	{
		MutexLock lock(extra_memory_report_mutex);
		for (const ExtraMemoryReportSource &src : extra_memory_report_sources) {
			const uint64_t bytes = src.callback != nullptr ? src.callback(src.userdata) : 0;
			const uint64_t allocs = bytes > 0 ? 1u : 0u;
			report += "\n";
			report += src.category;
			report += ";0;0;";
			report += String::num_real(double(bytes) / (1024.0 * 1024.0));
			report += ";";
			report += String::num_uint64(allocs);
		}
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

// GodotNext: rq_substrate (#22), rt_core_check (#283). The RT-core denylist: device-ID blocks of
// NVIDIA, AMD and Intel parts without hardware ray-tracing units. Every other device of these three
// vendors counts, so a later RT generation needs no new row, as UE 5.9 words its classes: "NVIDIA
// RTX-2000 series or newer", "AMD RX-6000 series or newer" and "Intel Arc A-Series Graphics Cards or
// newer" (plan §26.1). The IDs come from the PCI ID database (pci-ids.ucw.cz, version 2026.11.16), the
// Linux kernel's amdgpu, radeon and i915/xe ID tables and Mesa's Intel tables, whose per-platform
// has_ray_tracing flag decides the Intel rows (docs/godotnext/source-log/rhi-vulkan.md). NVIDIA and AMD
// number their dies in order and give each die a block, so a row covers a die's whole block or every ID
// below the first RT die. A single part without RT units on an RT die is a row of its own. Intel rows
// span one platform's listed IDs inside a 0x100 block that holds no RT part, and list IDs exactly
// where RT and non-RT parts share a block (Nova Lake). A new part without RT units whose driver exposes
// ray query needs a row; until it has one, the startup self-test and the HWRT fault latch stay the
// safety net. Run on hardware with this rule: the cloud L4 and T4 (plan §26.5).
namespace {
struct RayQueryHardwareDenyRange {
	uint32_t vendor;
	uint32_t first_id;
	uint32_t last_id;
	const char *parts;
};

const RayQueryHardwareDenyRange RAY_QUERY_HARDWARE_DENYLIST[] = {
	// NVIDIA: every ID below TU102's block, then the Turing and later dies and single parts without RT cores.
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x0000, 0x1E01, "NVIDIA before Turing (Tesla to Pascal) and Volta" },
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x1E09, 0x1E09, "CMP 50HX (TU102)" },
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x1F09, 0x1F0B, "GeForce GTX 1660 SUPER, GTX 1650 and CMP 40HX (TU106)" },
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x1F80, 0x1FFF, "TU117: GeForce GTX 1650 and 1630, MX450, MX550, T400 to T1200" },
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2080, 0x20FF, "GA100 compute: A100, A30, A800, CMP 170HX" },
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2180, 0x21FF, "TU116: GeForce GTX 1660 (Ti, SUPER), GTX 1650 (SUPER), CMP 30HX" },
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x220D, 0x220D, "CMP 90HX (GA102)" },
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2300, 0x23FF, "GH100 compute: H100, H200, H800, H20, GH200" },
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x248A, 0x248A, "CMP 70HX (GA104)" },
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x25A6, 0x25A7, "GeForce MX570 (GA107)" },
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x25AA, 0x25AA, "GeForce MX570 A (GA107)" },
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x2900, 0x29FF, "GB100 and GB102 compute: B200, B100, GB200" },
	{ RenderingContextDriver::Vendor::VENDOR_NVIDIA, 0x3180, 0x31FF, "GB110 compute: B300, GB300" },

	// AMD discrete parts before RDNA2, and the CDNA compute dies. The APU block 0x1000-0x1FFF is decided by
	// the integrated rule first (AMD_APU_DEVICE_IDS).
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x2000, 0x739F, "AMD before RDNA2: up to Polaris, Vega, RX 5000 (RDNA1), Instinct MI100" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x7400, 0x741F, "CDNA2 compute: Instinct MI200" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x74A0, 0x74BF, "CDNA3 compute: Instinct MI300" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x75A0, 0x75BF, "CDNA4 compute: Instinct MI350" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x7834, 0x7835, "RS350 chipset graphics" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x791E, 0x796E, "RS690, RS600 and RS740 chipset graphics" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x9400, 0x95CF, "R600 to RV790 (Radeon HD 2000 to 4000)" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x9610, 0x964F, "RS780 chipset graphics, Sumo APUs" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x9710, 0x9715, "RS880 chipset graphics" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x9802, 0x98E4, "Wrestler, Kabini, Mullins, Carrizo and Stoney APUs" },
	{ RenderingContextDriver::Vendor::VENDOR_AMD, 0x9900, 0x99A4, "Trinity and Richland APUs, console APUs" },

	// Intel: every platform before Xe-HPG, and the later platforms Mesa marks without ray tracing.
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x0042, 0x004A, "Ironlake" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x0102, 0x0176, "Sandy Bridge, Ivy Bridge, Bay Trail" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x0402, 0x042E, "Haswell" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x08CF, 0x08CF, "Clover Trail" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x0A02, 0x0A84, "Haswell, Broxton" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x0C02, 0x0C2E, "Haswell" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x0D02, 0x0D36, "Haswell, Crystal Well" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x0F30, 0x0F33, "Bay Trail" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x1132, 0x1132, "i815" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x1240, 0x1240, "i752" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x1602, 0x163E, "Broadwell" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x1902, 0x193D, "Skylake" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x1A12, 0x1A85, "Timna, Broxton" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x22B0, 0x22B3, "Cherry View" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x2562, 0x2592, "i845G to i915GM" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x2772, 0x27AE, "i945G, i945GM" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x2972, 0x29D3, "i965, G33" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x2A02, 0x2A43, "i965, G4x" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x2E02, 0x2E93, "G4x" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x3184, 0x3185, "Gemini Lake" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x3577, 0x358E, "i830, i85x" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x3E90, 0x3EA9, "Coffee Lake" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x4100, 0x410F, "Moorestown, Atom E6xx" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x4541, 0x4571, "Elkhart Lake" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x4626, 0x46D4, "Alder Lake (UHD Graphics, Iris Xe)" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x4905, 0x4909, "DG1 (Iris Xe MAX), SG1" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x4C80, 0x4C9A, "Rocket Lake" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x4E51, 0x4E71, "Jasper Lake, Elkhart Lake" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x5002, 0x5009, "CE 3100" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x5902, 0x593B, "Kaby Lake" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x5A40, 0x5A85, "Cannon Lake, Broxton" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x7120, 0x712A, "i810" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x7800, 0x7800, "i740" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x8108, 0x8109, "Poulsbo" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x8182, 0x8182, "Atom E6xx" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x87C0, 0x87CA, "Kaby Lake, Coffee Lake" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x8A50, 0x8A71, "Ice Lake" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x9840, 0x9841, "Lakefield" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x9A40, 0x9AF8, "Tiger Lake (Iris Xe, UHD Graphics)" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0x9B21, 0x9BF6, "Comet Lake" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0xA001, 0xA012, "Pineview" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0xA720, 0xA7AD, "Raptor Lake (UHD Graphics 770, Iris Xe)" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0xD740, 0xD740, "Nova Lake S/HX/UL" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0xD743, 0xD743, "Nova Lake S/HX/UL" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0xD745, 0xD745, "Nova Lake S/HX/UL" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0xD74A, 0xD74B, "Nova Lake S" },
	{ RenderingContextDriver::Vendor::VENDOR_INTEL, 0xFD80, 0xFD81, "Wildcat Lake" },
};

// Every AMD GPU the PCI ID database lists in this block is integrated: the APUs from Kaveri on, RDNA2
// and later ones (Rembrandt, Phoenix, Strix, Krackan) included, and the console APUs.
constexpr uint32_t AMD_APU_DEVICE_ID_FIRST = 0x1000;
constexpr uint32_t AMD_APU_DEVICE_ID_LAST = 0x1FFF;

// The Vulkan specification puts a PCI device ID in the low 16 bits of deviceID with the other bits zero;
// a device that is not primarily a PCI device reports a platform value instead, such as 0x93BA03D7 for
// the Tegra X2's integrated GPU. The denylist rows are PCI IDs, so such a device has no row and does not
// count, as under the #22 allowlist.
constexpr uint32_t PCI_DEVICE_ID_LAST = 0xFFFF;
} // namespace

RenderingContextDriver::RayQueryHardwareCheck RenderingContextDriver::check_ray_query_hardware_device(uint32_t p_vendor_id, uint32_t p_device_id, bool p_integrated) {
	RayQueryHardwareCheck check;
	if (p_vendor_id != Vendor::VENDOR_NVIDIA && p_vendor_id != Vendor::VENDOR_AMD && p_vendor_id != Vendor::VENDOR_INTEL) {
		check.rule = RAY_QUERY_HARDWARE_RULE_OTHER_VENDOR;
		check.note = "no RT-core rule for this vendor";
		return check;
	}
	if (p_device_id > PCI_DEVICE_ID_LAST) {
		check.rule = RAY_QUERY_HARDWARE_RULE_NOT_PCI_DEVICE_ID;
		check.note = "not a PCI device ID (a system-on-chip GPU)";
		return check;
	}
	if (p_vendor_id == Vendor::VENDOR_AMD && (p_integrated || (p_device_id >= AMD_APU_DEVICE_ID_FIRST && p_device_id <= AMD_APU_DEVICE_ID_LAST))) {
		check.rule = RAY_QUERY_HARDWARE_RULE_AMD_INTEGRATED;
		check.note = "AMD integrated GPU; hardware ray tracing needs a discrete AMD RX 6000 series or newer GPU";
		return check;
	}
	for (const RayQueryHardwareDenyRange &range : RAY_QUERY_HARDWARE_DENYLIST) {
		if (range.vendor == p_vendor_id && p_device_id >= range.first_id && p_device_id <= range.last_id) {
			check.rule = RAY_QUERY_HARDWARE_RULE_DENYLIST;
			check.note = range.parts;
			return check;
		}
	}
	check.hardware = true;
	check.rule = RAY_QUERY_HARDWARE_RULE_RT_GENERATION;
	check.note = p_vendor_id == Vendor::VENDOR_NVIDIA ? "NVIDIA Turing or newer with RT cores" : (p_vendor_id == Vendor::VENDOR_AMD ? "AMD RDNA2 or newer, discrete" : "Intel Xe-HPG or newer with RT units");
	return check;
}

bool RenderingContextDriver::is_ray_query_hardware_device(uint32_t p_vendor_id, uint32_t p_device_id, bool p_integrated) {
	return check_ray_query_hardware_device(p_vendor_id, p_device_id, p_integrated).hardware;
}
