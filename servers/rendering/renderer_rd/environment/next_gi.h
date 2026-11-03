/**************************************************************************/
/*  next_gi.h                                                             */
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

#include "core/string/ustring.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/rendering_device_enums.h"

class RenderingDevice;

namespace RendererRD {

// NextGI host entry: the §3 tier selection (#15) and the ray-query toolchain self-test (#20).
// #36 grows run_toolchain_selftest into the startup 64-ray self-test.
class NextGI {
public:
	// The NextGI tier (plan §3.1). P0 falls back to SDFGI; #187 replaces the fallback with TIER_SOFTWARE.
	enum Tier {
		TIER_DISABLED, // rendering/global_illumination/nextgi/enabled is false.
		TIER_HWRT,
		TIER_FALLBACK, // SDFGI for every Environment with nextgi_enabled.
		TIER_UNSUPPORTED_RENDERER, // Mobile, Compatibility and the dummy renderer.
		TIER_MAX,
	};

	// Why HWRT was not selected (plan §6.3). Per-instance RT exclusions belong to the separate
	// per-(instance, surface) Fallback enum (P1b), not here.
	enum TierReason {
		TIER_REASON_NONE,
		NO_RAY_QUERY,
		NOT_HW_ACCELERATED,
		CPU_DEVICE,
		D3D12_NO_DXR,
		METAL_SYNC_MODE,
		METAL_NO_ARGUMENT_BUFFERS,
		SETTING_OFF,
		AS_MEMORY,
		SELFTEST_FAILED,
		HWRT_FAULTED,
		TIER_REASON_MAX,
	};

	// rendering/global_illumination/nextgi/hardware_ray_tracing.
	enum HardwareRayTracing {
		HARDWARE_RAY_TRACING_AUTO,
		HARDWARE_RAY_TRACING_OFF,
		HARDWARE_RAY_TRACING_FORCE,
		HARDWARE_RAY_TRACING_MAX,
	};

	// rendering/global_illumination/nextgi/quality.
	enum Quality {
		QUALITY_AUTO,
		QUALITY_MEDIUM,
		QUALITY_HIGH,
		QUALITY_EPIC,
		QUALITY_MAX,
	};

	struct TierInputs {
		bool forward_plus = true;
		bool project_enabled = false;
		HardwareRayTracing hwrt_setting = HARDWARE_RAY_TRACING_AUTO;
		bool ray_query = false; // has_feature(SUPPORTS_RAY_QUERY).
		TierReason driver_reason = NO_RAY_QUERY; // The driver's reason code when ray_query is false.
		bool cpu_device = false; // get_device_type() == DEVICE_TYPE_CPU. Only the rule checks it.
		bool ray_query_hw = false; // SUPPORTS_RAY_QUERY_HARDWARE: driver knowledge only.
		// Pass defaults until #36 (self-test and crash latch) and the P1b AS budget (#39) feed them.
		bool selftest_passed = true;
		bool fault_latched = false;
		bool as_memory_ok = true;
	};

	struct TierResult {
		Tier tier = TIER_DISABLED;
		TierReason reason = TIER_REASON_NONE;
		bool warn_force = false; // Force was requested on a device without ray query.

		bool operator==(const TierResult &p_other) const { return tier == p_other.tier && reason == p_other.reason && warn_force == p_other.warn_force; }
		bool operator!=(const TierResult &p_other) const { return !(*this == p_other); }
	};

private:
	static NextGI *singleton;

	TierInputs inputs;
	TierResult result;
	Quality quality_setting = QUALITY_AUTO;
	bool reported = false;
	TierResult reported_result;
	int ray_query_shaders_initialized = 0;

	void _report(const String &p_line, const TierResult &p_result) const;

public:
	static NextGI *get_singleton() { return singleton; }

	// The §3.1 rule. Pure, so doctests need no device. The first failing check wins:
	// !forward_plus -> UNSUPPORTED_RENDERER; !project_enabled -> DISABLED; Off -> SETTING_OFF;
	// !ray_query -> the driver's reason (warn_force under Force); unless Force, cpu_device -> CPU_DEVICE,
	// then !ray_query_hw -> NOT_HW_ACCELERATED; !selftest_passed -> SELFTEST_FAILED;
	// fault_latched without Force -> HWRT_FAULTED; !as_memory_ok -> AS_MEMORY; else HWRT.
	static TierResult evaluate_tier(const TierInputs &p_inputs);

	// Fills the device side of the inputs from a RenderingDevice (which may have mocked features)
	// plus the driver's API name and device type.
	// Metal (#48) reports its reason and SUPPORTS_RAY_QUERY_HARDWARE. Until #22, other drivers still
	// take the reason from the API name (D3D12 -> D3D12_NO_DXR, else NO_RAY_QUERY) and every ray-query
	// device counts as hardware accelerated.
	static void collect_device_inputs(const RenderingDevice *p_device, const String &p_api_name, RenderingDeviceEnums::DeviceType p_device_type, TierInputs &r_inputs);

	// Auto resolves to High until the startup microbenchmark lands (#55).
	static Quality resolve_quality(Quality p_setting);

	static String get_tier_name(Tier p_tier);
	static String get_reason_name(TierReason p_reason);
	static String get_quality_name(Quality p_quality);
	// The one printed line, e.g. "NextGI: tier=FALLBACK reason=D3D12_NO_DXR (SDFGI fallback) quality=HIGH (auto selection pending)".
	static String format_tier_line(const TierResult &p_result, Quality p_quality_setting);

	// Reads the nextgi/* project settings and the device, then evaluates and reports the tier.
	void init(bool p_forward_plus);
	// Re-evaluates the tier with a new hardware_ray_tracing value (0 Auto, 1 Off, 2 Force).
	void set_hardware_ray_tracing(int p_mode);

	// Stores the inputs and evaluates the tier. Returns true and sets r_line when a line is due:
	// NextGI is enabled in the project and the result differs from the last one reported.
	bool update_tier(const TierInputs &p_inputs, Quality p_quality_setting, String &r_line);

	const TierInputs &get_tier_inputs() const { return inputs; }
	TierResult get_tier_result() const { return result; }
	// Every ray-query ShaderRD checks this before it initializes, and creates its versions with
	// version_create(false) (non-embedded). The shader baker filter and export check are #40.
	bool can_initialize_ray_query_shaders() const { return result.tier == TIER_HWRT; }
	// Counts ray-query ShaderRD versions created under the HWRT tier, for nextgi_get_tier_info().
	void notify_ray_query_shader_initialized();
	int get_ray_query_shaders_initialized() const { return ray_query_shaders_initialized; }
	// True when Environments with nextgi_enabled run SDFGI instead.
	bool is_sdfgi_fallback() const { return result.tier == TIER_FALLBACK; }
	// {tier, reason, quality, ray_query_shaders_initialized}, the names as in the printed line.
	Dictionary get_tier_info() const;

	// Compile the self-test compute source the way ShaderRD would, without creating
	// a GPU pipeline. Used by the offline toolchain doctest. Does not check ray-query support.
	static String build_toolchain_selftest_source();

	// Without SUPPORTS_RAY_QUERY, returns false and does not construct or initialize the ShaderRD.
	// With ray query but outside the HWRT tier, it does the same and reports the tier or its reason.
	// The report is JSON. On success it includes the SPIR-V version word, pipeline creation and the shader name.
	static bool run_toolchain_selftest(String &r_report);

	NextGI();
	~NextGI();
};

} // namespace RendererRD
