/**************************************************************************/
/*  next_gi.cpp                                                           */
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

#include "next_gi.h"

#include "core/config/project_settings.h"
#include "core/string/print_string.h"
#include "servers/rendering/renderer_rd/shader_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_rq_selftest.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

using namespace RendererRD;

NextGI *NextGI::singleton = nullptr;

static const char *_nextgi_tier_names[NextGI::TIER_MAX] = {
	"DISABLED",
	"HWRT",
	"FALLBACK",
	"UNSUPPORTED_RENDERER",
};

static const char *_nextgi_reason_names[NextGI::TIER_REASON_MAX] = {
	"NONE",
	"NO_RAY_QUERY",
	"NOT_HW_ACCELERATED",
	"CPU_DEVICE",
	"D3D12_NO_DXR",
	"METAL_SYNC_MODE",
	"METAL_NO_ARGUMENT_BUFFERS",
	"SETTING_OFF",
	"AS_MEMORY",
	"SELFTEST_FAILED",
	"HWRT_FAULTED",
};

static const char *_nextgi_quality_names[NextGI::QUALITY_MAX] = {
	"AUTO",
	"MEDIUM",
	"HIGH",
	"EPIC",
};

static bool _nextgi_is_driver_reason(NextGI::TierReason p_reason) {
	switch (p_reason) {
		case NextGI::NO_RAY_QUERY:
		case NextGI::D3D12_NO_DXR:
		case NextGI::METAL_SYNC_MODE:
		case NextGI::METAL_NO_ARGUMENT_BUFFERS:
			return true;
		default:
			return false;
	}
}

NextGI::TierResult NextGI::evaluate_tier(const TierInputs &p_inputs) {
	TierResult r;
	const bool force = p_inputs.hwrt_setting == HARDWARE_RAY_TRACING_FORCE;

	if (!p_inputs.forward_plus) {
		r.tier = TIER_UNSUPPORTED_RENDERER;
		return r;
	}
	if (!p_inputs.project_enabled) {
		r.tier = TIER_DISABLED;
		return r;
	}

	r.tier = TIER_FALLBACK;
	if (p_inputs.hwrt_setting == HARDWARE_RAY_TRACING_OFF) {
		r.reason = SETTING_OFF;
		return r;
	}
	if (!p_inputs.ray_query) {
		// Only the driver's reason codes apply here. Anything else means the driver gave none.
		r.reason = _nextgi_is_driver_reason(p_inputs.driver_reason) ? p_inputs.driver_reason : NO_RAY_QUERY;
		r.warn_force = force;
		return r;
	}
	if (!force) {
		// The CPU device-type check lives in the rule alone (D11).
		if (p_inputs.cpu_device) {
			r.reason = CPU_DEVICE;
			return r;
		}
		if (!p_inputs.ray_query_hw) {
			r.reason = NOT_HW_ACCELERATED;
			return r;
		}
	}
	if (!p_inputs.selftest_passed) {
		r.reason = SELFTEST_FAILED;
		return r;
	}
	if (p_inputs.fault_latched && !force) {
		r.reason = HWRT_FAULTED;
		return r;
	}
	if (!p_inputs.as_memory_ok) {
		r.reason = AS_MEMORY;
		return r;
	}

	r.tier = TIER_HWRT;
	return r;
}

void NextGI::collect_device_inputs(const RenderingDevice *p_device, const String &p_api_name, RenderingDeviceEnums::DeviceType p_device_type, TierInputs &r_inputs) {
	r_inputs.ray_query = p_device != nullptr && p_device->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY);
	// Interim driver inputs (see the header). #22 and #48 replace both lines with the driver's own values.
	r_inputs.driver_reason = r_inputs.ray_query ? TIER_REASON_NONE : (p_api_name == "D3D12" ? D3D12_NO_DXR : NO_RAY_QUERY);
	r_inputs.ray_query_hw = r_inputs.ray_query;
	r_inputs.cpu_device = p_device_type == RenderingDeviceEnums::DEVICE_TYPE_CPU;
}

NextGI::Quality NextGI::resolve_quality(Quality p_setting) {
	if (p_setting <= QUALITY_AUTO || p_setting >= QUALITY_MAX) {
		return QUALITY_HIGH;
	}
	return p_setting;
}

String NextGI::get_tier_name(Tier p_tier) {
	ERR_FAIL_INDEX_V(p_tier, TIER_MAX, String());
	return _nextgi_tier_names[p_tier];
}

String NextGI::get_reason_name(TierReason p_reason) {
	ERR_FAIL_INDEX_V(p_reason, TIER_REASON_MAX, String());
	return _nextgi_reason_names[p_reason];
}

String NextGI::get_quality_name(Quality p_quality) {
	ERR_FAIL_INDEX_V(p_quality, QUALITY_MAX, String());
	return _nextgi_quality_names[p_quality];
}

String NextGI::format_tier_line(const TierResult &p_result, Quality p_quality_setting) {
	String line = vformat("NextGI: tier=%s reason=%s", get_tier_name(p_result.tier), get_reason_name(p_result.reason));
	if (p_result.tier == TIER_FALLBACK) {
		line += " (SDFGI fallback)";
	}
	if (p_result.tier == TIER_HWRT || p_result.tier == TIER_FALLBACK) {
		line += " quality=" + get_quality_name(resolve_quality(p_quality_setting));
		if (p_quality_setting == QUALITY_AUTO) {
			line += " (auto selection pending)";
		}
	}
	return line;
}

bool NextGI::update_tier(const TierInputs &p_inputs, Quality p_quality_setting, String &r_line) {
	inputs = p_inputs;
	quality_setting = p_quality_setting;
	result = evaluate_tier(inputs);

	if (!inputs.project_enabled) {
		// A later enable is a new selection, so it prints again.
		reported = false;
		return false;
	}
	if (reported && reported_result == result) {
		return false;
	}
	reported = true;
	reported_result = result;
	r_line = format_tier_line(result, quality_setting);
	return true;
}

void NextGI::_report(const String &p_line, const TierResult &p_result) const {
	print_line(p_line);
	if (p_result.warn_force) {
		// Worded without the "NextGI:" prefix, so the tier stays the only such line in the log.
		WARN_PRINT(vformat("hardware_ray_tracing is Force, but the device has no ray query (%s). NextGI uses the SDFGI fallback.", get_reason_name(p_result.reason)));
	}
}

void NextGI::init(bool p_forward_plus) {
	TierInputs new_inputs;
	new_inputs.forward_plus = p_forward_plus;
	new_inputs.project_enabled = GLOBAL_GET("rendering/global_illumination/nextgi/enabled");
	new_inputs.hwrt_setting = HardwareRayTracing(CLAMP(int(GLOBAL_GET("rendering/global_illumination/nextgi/hardware_ray_tracing")), 0, HARDWARE_RAY_TRACING_MAX - 1));
	const Quality new_quality = Quality(CLAMP(int(GLOBAL_GET("rendering/global_illumination/nextgi/quality")), 0, QUALITY_MAX - 1));

	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr) {
		collect_device_inputs(rd, rd->get_device_api_name(), rd->get_device_type(), new_inputs);
	}

	String line;
	if (update_tier(new_inputs, new_quality, line)) {
		_report(line, result);
	}
}

void NextGI::set_hardware_ray_tracing(int p_mode) {
	ERR_FAIL_INDEX(p_mode, HARDWARE_RAY_TRACING_MAX);
	TierInputs new_inputs = inputs;
	new_inputs.hwrt_setting = HardwareRayTracing(p_mode);
	String line;
	if (update_tier(new_inputs, quality_setting, line)) {
		_report(line, result);
	}
}

void NextGI::notify_ray_query_shader_initialized() {
	ERR_FAIL_COND_MSG(!can_initialize_ray_query_shaders(), "Ray-query ShaderRDs are initialized only under the NextGI HWRT tier.");
	ray_query_shaders_initialized++;
}

Dictionary NextGI::get_tier_info() const {
	Dictionary info;
	info["tier"] = get_tier_name(result.tier);
	info["reason"] = get_reason_name(result.reason);
	const bool active = result.tier == TIER_HWRT || result.tier == TIER_FALLBACK;
	info["quality"] = active ? get_quality_name(resolve_quality(quality_setting)) : String("NONE");
	info["ray_query_shaders_initialized"] = ray_query_shaders_initialized;
	return info;
}

NextGI::NextGI() {
	// Doctests build their own instances; the renderer's instance is the first one.
	if (singleton == nullptr) {
		singleton = this;
	}
}

NextGI::~NextGI() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

static uint32_t _nextgi_spirv_version_word(const Vector<uint8_t> &p_spirv) {
	if (p_spirv.size() < 8) {
		return 0;
	}
	const uint8_t *bytes = p_spirv.ptr();
	return uint32_t(bytes[4]) | (uint32_t(bytes[5]) << 8) | (uint32_t(bytes[6]) << 16) | (uint32_t(bytes[7]) << 24);
}

String RendererRD::NextGI::build_toolchain_selftest_source() {
	NextgiRqSelftestShaderRD shader;
	shader.initialize({ "" });
	RID version = shader.version_create(false);
	ERR_FAIL_COND_V(!version.is_valid(), String());

	Vector<String> sources = shader.version_build_variant_stage_sources(version, 0);
	shader.version_free(version);

	if (sources.size() <= RenderingDevice::SHADER_STAGE_COMPUTE) {
		return String();
	}
	return sources[RenderingDevice::SHADER_STAGE_COMPUTE];
}

bool RendererRD::NextGI::run_toolchain_selftest(String &r_report) {
	// Metal and D3D12 do not expose ray query yet (#48, #47, PD). Leave the ShaderRD untouched.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || !rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY)) {
		r_report = "{\"pass\":false,\"reason\":\"NO_RAY_QUERY\"}";
		return false;
	}

	// Ray-query ShaderRDs are initialized only under the HWRT tier (plan §3.1).
	NextGI *next_gi = get_singleton();
	if (next_gi == nullptr || !next_gi->can_initialize_ray_query_shaders()) {
		String reason = "DISABLED";
		if (next_gi != nullptr) {
			const TierResult tier = next_gi->get_tier_result();
			reason = tier.tier == TIER_FALLBACK ? get_reason_name(tier.reason) : get_tier_name(tier.tier);
		}
		r_report = vformat("{\"pass\":false,\"reason\":\"%s\"}", reason);
		return false;
	}

	NextgiRqSelftestShaderRD shader;
	shader.initialize({ "" });
	RID version = shader.version_create(false);
	if (!version.is_valid()) {
		r_report = vformat("{\"pass\":false,\"reason\":\"COMPILE_FAILED\",\"shader\":\"%s\"}", shader.get_name());
		return false;
	}
	next_gi->notify_ray_query_shader_initialized();

	// Non-embedded: the shader baker never bakes this self-test.
	RID shader_rid = shader.version_get_shader(version, 0);

	uint32_t spirv_word = 0;
	{
		Vector<String> sources = shader.version_build_variant_stage_sources(version, 0);
		Vector<RenderingDevice::ShaderStageSPIRVData> stages = ShaderRD::compile_stages(sources, Vector<uint64_t>());
		for (int i = 0; i < stages.size(); i++) {
			if (stages[i].shader_stage == RenderingDevice::SHADER_STAGE_COMPUTE) {
				spirv_word = _nextgi_spirv_version_word(stages[i].spirv);
				break;
			}
		}
	}

	RID pipeline;
	if (shader_rid.is_valid()) {
		pipeline = rd->compute_pipeline_create(shader_rid);
	}
	const bool pipeline_ok = pipeline.is_valid();
	if (pipeline_ok) {
		rd->free_rid(pipeline);
	}
	shader.version_free(version);

	const uint32_t major = (spirv_word >> 16) & 0xff;
	const uint32_t minor = (spirv_word >> 8) & 0xff;
	r_report = vformat(
			"{\"pass\":%s,\"spirv\":\"%d.%d\",\"spirv_word\":\"0x%08x\",\"pipeline\":%s,\"shader\":\"%s\"}",
			pipeline_ok ? "true" : "false",
			major,
			minor,
			spirv_word,
			pipeline_ok ? "true" : "false",
			shader.get_name());
	return pipeline_ok;
}
