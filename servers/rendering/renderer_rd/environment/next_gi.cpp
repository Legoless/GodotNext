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

#include "servers/rendering/renderer_rd/environment/next_gi_reference.h"
#include "servers/rendering/renderer_rd/environment/next_gi_wrc.h"

// GodotNext: nextgi_rt_scene
#include "core/config/project_settings.h"
#include "core/io/image.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "servers/rendering/renderer_rd/effects/ss_effects.h"
#include "servers/rendering/renderer_rd/environment/gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_far_field.h"
#include "servers/rendering/renderer_rd/environment/next_gi_front_layer.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shader_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_rq_selftest.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/rendering_server_globals.h"

// GodotNext: nextgi_src_cards
#include "servers/rendering/renderer_rd/environment/next_gi_gather.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/environment/next_gi_swrt_trace.h"
#include "servers/rendering/renderer_rd/environment/next_gi_voxel_lighting.h"

using namespace RendererRD;

NextGI *NextGI::singleton = nullptr;

// GodotNext: nextgi_quality_presets (#55). Indexed by Quality; AUTO resolves to High first.
const NextGI::QualityParams NextGI::QUALITY_PARAMS[NextGI::QUALITY_MAX] = {
	// QUALITY_AUTO: the resolved preset supplies the values.
	{ 16, 2, 0.4f, 256 * 1024, 256 * 1024, 64, 128, { 140, 60, 140, 100, 140, 300, 240, 160, 320 }, { 350, 150, 350, 250, 350, 750, 600, 400, 800 } },
	// QUALITY_MEDIUM: High x 0.5 except line 1a.
	{ 8, 4, 0.25f, 128 * 1024, 128 * 1024, 64, 128, { 140, 30, 70, 50, 70, 150, 120, 80, 160 }, { 350, 75, 175, 125, 175, 375, 300, 200, 400 } },
	// QUALITY_HIGH.
	{ 16, 2, 0.4f, 256 * 1024, 256 * 1024, 64, 128, { 140, 60, 140, 100, 140, 300, 240, 160, 320 }, { 350, 150, 350, 250, 350, 750, 600, 400, 800 } },
	// QUALITY_EPIC: 2x High; 2a + 2b share the 300/750 us entry.
	{ 64, 1, 0.4f, 512 * 1024, 512 * 1024, 128, 256, { 140, 150, 150, 160, 240, 800, 800, 300, 460 }, { 350, 375, 375, 400, 600, 2000, 2000, 750, 1150 } },
};

const NextGI::QualityParams &NextGI::get_quality_params(Quality p_quality) {
	const Quality resolved = resolve_quality(p_quality);
	return QUALITY_PARAMS[resolved];
}

uint32_t NextGI::nextgi_quality_total_us(Quality p_quality, bool p_apple) {
	const QualityParams &params = get_quality_params(p_quality);
	uint32_t total = 0;
	for (int i = 0; i < 9; i++) {
		total += p_apple ? params.apple_lines_us[i] : params.ada_lines_us[i];
	}
	return total;
}

const NextGI::PassSlotInfo NextGI::PASS_SLOTS[NextGI::PASS_SLOT_MAX] = {
	{ "> NextGI", "" },
	{ "NextGI: SRC capture", "2a" },
	{ "NextGI: AS updates", "1a" },
	{ "NextGI: AS builds", "1b" },
	{ "NextGI: SRC relight", "2b" },
	{ "NextGI: WRC update", "3" },
	{ "NextGI: Screen traces", "4" },
	{ "NextGI: Diffuse gather", "5" },
	{ "NextGI: Reflection traces", "6" },
	{ "NextGI: Integration", "7" },
	{ "NextGI: Denoise", "8" },
	{ "< NextGI", "" },
};

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

static NextGI::TierReason _nextgi_reason_from_driver(RenderingDeviceCommons::RayQueryUnsupportedReason p_reason) {
	switch (p_reason) {
		case RenderingDeviceCommons::RAY_QUERY_SUPPORTED:
			return NextGI::TIER_REASON_NONE;
		case RenderingDeviceCommons::RAY_QUERY_REASON_NO_RAY_QUERY:
			return NextGI::NO_RAY_QUERY;
		case RenderingDeviceCommons::RAY_QUERY_REASON_D3D12_NO_DXR:
			return NextGI::D3D12_NO_DXR;
		case RenderingDeviceCommons::RAY_QUERY_REASON_METAL_SYNC_MODE:
			return NextGI::METAL_SYNC_MODE;
		case RenderingDeviceCommons::RAY_QUERY_REASON_METAL_NO_ARGUMENT_BUFFERS:
			return NextGI::METAL_NO_ARGUMENT_BUFFERS;
	}
	return NextGI::NO_RAY_QUERY;
}

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
	r_inputs.cpu_device = p_device_type == RenderingDeviceEnums::DEVICE_TYPE_CPU;

	// Metal reports the helper's reason and the hardware flag. Other drivers keep the interim
	// mapping until #22.
	RenderingDeviceDriver *driver = p_device != nullptr ? p_device->get_device_driver() : nullptr;
	if (driver != nullptr && p_api_name == "Metal") {
		r_inputs.ray_query_hw = p_device->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY_HARDWARE);
		r_inputs.driver_reason = _nextgi_reason_from_driver(driver->get_ray_query_unsupported_reason());
		if (r_inputs.ray_query) {
			r_inputs.driver_reason = TIER_REASON_NONE;
		}
		return;
	}

	r_inputs.driver_reason = r_inputs.ray_query ? TIER_REASON_NONE : (p_api_name == "D3D12" ? D3D12_NO_DXR : NO_RAY_QUERY);
	r_inputs.ray_query_hw = r_inputs.ray_query;
}

// GodotNext: nextgi_quality_presets (#55). Startup benchmark rule: <= 5 ms picks High, else Medium.
NextGI::Quality NextGI::nextgi_auto_quality_from_usec(uint64_t p_usec) {
	return p_usec > 0 && p_usec <= 5000 ? QUALITY_HIGH : QUALITY_MEDIUM;
}

NextGI::Quality NextGI::auto_quality = NextGI::QUALITY_AUTO;
String NextGI::auto_reason;

NextGI::Quality NextGI::resolve_quality(Quality p_setting) {
	if (p_setting == QUALITY_AUTO) {
		return auto_quality == QUALITY_AUTO ? QUALITY_HIGH : auto_quality;
	}
	if (p_setting == QUALITY_EPIC && nextgi_epic_cut()) {
		return QUALITY_HIGH;
	}
	if (p_setting < QUALITY_MEDIUM || p_setting >= QUALITY_MAX) {
		return QUALITY_HIGH;
	}
	return p_setting;
}

void NextGI::_settings_changed() {
	const Quality new_quality = Quality(CLAMP(int(GLOBAL_GET("rendering/global_illumination/nextgi/quality")), 0, QUALITY_MAX - 1));
	if (new_quality == quality_setting) {
		return;
	}
	quality_setting = new_quality;
	// The gather reallocates its probe targets when the ray stride changes; the SRC atlas sizes
	// only change with the preset at creation, so nothing is rebuilt mid-frame.
	print_line(vformat("NextGI: quality=%s (%s)", get_quality_name(resolve_quality(quality_setting)), quality_setting == QUALITY_AUTO ? "setting auto" : "setting change"));
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
			line += auto_reason.is_empty() ? " (auto selection pending)" : " (" + auto_reason + ")";
		}
	}
	return line;
}

bool NextGI::update_tier(const TierInputs &p_inputs, Quality p_quality_setting, String &r_line) {
	inputs = p_inputs;
	quality_setting = p_quality_setting;
	result = evaluate_tier(inputs);

	// GodotNext: ps_mesh_sdf_atlas (#185). keep_when_hwrt=false releases the
	// brick pool while HWRT is active; the runtime tier switch is then disabled.
	if (result.tier == TIER_HWRT && !bool(GLOBAL_GET("rendering/global_illumination/mesh_sdf/keep_when_hwrt"))) {
		MeshStorage *mesh_storage = MeshStorage::get_singleton();
		if (mesh_storage != nullptr && mesh_storage->get_sdf_atlas().get_resident_bytes() > 0) {
			mesh_storage->get_sdf_atlas().free_all();
		}
	}

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
	_apply_safety_net(new_inputs);

	String line;
	if (update_tier(new_inputs, new_quality, line)) {
		log_tier_selection(line);
	}
	_sync_device_lost_callback();

	// GodotNext: nextgi_rt_scene
	_ensure_rt_scene();
}

void NextGI::set_hardware_ray_tracing(int p_mode) {
	ERR_FAIL_INDEX(p_mode, HARDWARE_RAY_TRACING_MAX);
	TierInputs new_inputs = inputs;
	new_inputs.hwrt_setting = HardwareRayTracing(p_mode);
	// Doctests build their own NextGI. Only the renderer instance talks to the live device.
	if (singleton == this) {
		_apply_safety_net(new_inputs);
	}
	String line;
	if (update_tier(new_inputs, quality_setting, line)) {
		log_tier_selection(line);
	}
	if (singleton == this) {
		_sync_device_lost_callback();
	}

	// GodotNext: nextgi_rt_scene
	_ensure_rt_scene();
}

// GodotNext: nextgi_rt_scene
void NextGI::_ensure_rt_scene() {
	// The tier can be HWRT in doctests with mocked inputs and no live device; the RT scene
	// exists only on a real ray-query device.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const bool hwrt_live = result.tier == TIER_HWRT && rd != nullptr && rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY);
	if (singleton == this && MeshStorage::get_singleton() != nullptr) {
		MeshStorage::get_singleton()->mesh_nextgi_set_hwrt_active(hwrt_live);
	}
	if (hwrt_live && rt_scene == nullptr) {
		rt_scene = memnew(NextGIRTScene);
		rt_scene->init();
	} else if (!hwrt_live && rt_scene != nullptr && singleton == this) {
		rt_scene->free();
		memdelete(rt_scene);
		rt_scene = nullptr;
	}
}

void NextGI::notify_ray_query_shader_initialized() {
	ERR_FAIL_COND_MSG(!can_initialize_ray_query_shaders(), "Ray-query ShaderRDs are initialized only under the NextGI HWRT tier.");
	ray_query_shaders_initialized++;
}

const char *NextGI::get_mode_name(Mode p_mode) {
	return p_mode == MODE_REFLECTIONS_ONLY ? "reflections_only" : "gi_and_reflections";
}

bool NextGI::is_reflections_only() {
	return nextgi_mode_from_setting(int(GLOBAL_GET("rendering/global_illumination/nextgi/mode"))) == MODE_REFLECTIONS_ONLY;
}

bool NextGI::is_reflections_enabled() {
	return bool(GLOBAL_GET("rendering/global_illumination/nextgi/reflections"));
}

NextGIWRC *NextGI::ensure_wrc() {
	if (wrc == nullptr) {
		wrc = memnew(NextGIWRC);
	}
	return wrc;
}

Dictionary NextGI::get_status() const {
	Dictionary status;
	status["mode"] = get_mode_name(nextgi_mode_from_setting(int(GLOBAL_GET("rendering/global_illumination/nextgi/mode"))));
	status["reflections"] = is_reflections_enabled();
	const bool active = result.tier == TIER_HWRT || result.tier == TIER_FALLBACK;
	status["tier"] = get_tier_name(result.tier);
	status["reason"] = get_reason_name(result.reason);
	status["quality"] = active ? get_quality_name(resolve_quality(quality_setting)) : String("NONE");
	if (!active) {
		status["quality_reason"] = "NONE";
	} else if (quality_setting == QUALITY_AUTO) {
		status["quality_reason"] = auto_reason.is_empty() ? "auto selection pending" : auto_reason;
	} else {
		status["quality_reason"] = "setting";
	}
	String driver;
	if (OS::get_singleton()) {
		driver = OS::get_singleton()->get_current_rendering_driver_name();
	}
	status["driver"] = driver;
	return status;
}

Dictionary NextGI::get_counters() const {
	Dictionary counters = RendererGI::nextgi_zero_counters();
	// GodotNext: nextgi_rt_scene
	if (rt_scene != nullptr) {
		counters["tlas_instances"] = int64_t(rt_scene->get_stats().instance_count);
		counters["skinned_refit_triangles"] = int64_t(rt_scene->get_stats().skinned_refit_triangles);
		const NextGIRTCoverage &coverage = rt_scene->get_stats().coverage;
		const uint64_t frame = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0;
		counters["rt_coverage"] = frame >= 120 ? nextgi_rt_coverage_percent(coverage) : int64_t(0);
		counters["rt_coverage_numerator"] = int64_t(coverage.numerator);
		counters["rt_coverage_denominator"] = int64_t(coverage.denominator);
		counters["rt_not_in_scene"] = int64_t(coverage.not_in_scene);
		counters["rt_instance_cap"] = int64_t(coverage.instance_cap);
		counters["rt_dynamic_excluded"] = int64_t(coverage.dynamic_excluded);
		counters["rt_blas_pending"] = int64_t(coverage.blas_pending);
	}
	if (MeshStorage::get_singleton() != nullptr) {
		const MeshStorage::NextGIRTStats ms_stats = MeshStorage::get_singleton()->mesh_nextgi_get_rt_stats();
		counters["blas_bytes_compacted"] = int64_t(ms_stats.blas_bytes_compacted);
		counters["blas_bytes_uncompacted"] = int64_t(ms_stats.blas_bytes_uncompacted);
		counters["rt_arena_bytes"] = int64_t(ms_stats.rt_arena_bytes);
		counters["proxy_blas_bytes_compacted"] = int64_t(ms_stats.proxy_blas_bytes_compacted);
		counters["proxy_blas_bytes_uncompacted"] = int64_t(ms_stats.proxy_blas_bytes_uncompacted);
		counters["proxy_triangles"] = int64_t(ms_stats.proxy_triangles);
		counters["proxy_blas_parts"] = int64_t(ms_stats.proxy_blas_parts);
	}
	if (src != nullptr) {
		counters["src_capture_texels"] = int64_t(src->get_stats().capture_texels);
		counters["src_stage2_cards_captured"] = int64_t(src->get_stats().stage2_captured_card_count);
	}
	if (relight != nullptr) {
		counters["src_relight_texels"] = int64_t(relight->get_last_texels());
		counters["src_shadow_rays"] = int64_t(relight->get_last_rays());
		counters["src_dirty_pages"] = int64_t(relight->get_dirty_pages());
		counters["src_max_relight_age_frames"] = int64_t(relight->get_max_relight_age());
	}
	// GodotNext: ps_voxel_lighting (#188)
	if (voxel_lighting != nullptr) {
		counters["voxel_lighting_retrace"] = int64_t(voxel_lighting->get_retrace_rays());
		counters["voxel_lighting_coverage_ppm"] = voxel_lighting->get_coverage_ppm();
		counters["voxel_lighting_volume_bytes"] = int64_t(voxel_lighting->get_allocated_bytes());
		counters["voxel_lighting_stored_hits"] = int64_t(voxel_lighting->get_stored_hits());
		counters["voxel_lighting_resampled_nonzero"] = int64_t(voxel_lighting->get_resampled_nonzero());
	}
	// GodotNext: ps_swrt_trace (#186)
	if (swrt_trace != nullptr) {
		counters["swrt_trace_steps"] = int64_t(swrt_trace->get_trace_steps());
		counters["swrt_trace_hits"] = int64_t(swrt_trace->get_trace_hits());
		counters["swrt_trace_misses"] = int64_t(swrt_trace->get_trace_misses());
		counters["swrt_shadow_rays"] = int64_t(swrt_trace->get_shadow_rays());
		counters["swrt_mode"] = int64_t(swrt_trace->get_mode());
	}
	return counters;
}

int NextGI::nextgi_multiview_choose_anchor(const ViewAnchorCandidate *p_views, uint32_t p_count, int p_previous) {
	if (p_views == nullptr || p_count == 0) {
		return -1;
	}
	// Keep last frame's anchor while it still renders, so the scenario center never flips.
	if (p_previous >= 0 && uint32_t(p_previous) < p_count && p_views[p_previous].valid) {
		return p_previous;
	}
	int best_attached = -1;
	int best_hint = -1;
	int best_area = -1;
	for (uint32_t i = 0; i < p_count; i++) {
		if (!p_views[i].valid) {
			continue;
		}
		if (p_views[i].screen_attached && (best_attached < 0 || p_views[i].area > p_views[best_attached].area)) {
			best_attached = int(i);
		}
		if (p_views[i].editor_hint && best_hint < 0) {
			best_hint = int(i);
		}
		if (best_area < 0 || p_views[i].area > p_views[best_area].area || (p_views[i].area == p_views[best_area].area && p_views[i].order < p_views[best_area].order)) {
			best_area = int(i);
		}
	}
	if (best_attached >= 0) {
		return best_attached;
	}
	if (best_hint >= 0) {
		return best_hint;
	}
	return best_area;
}

String NextGI::nextgi_multiview_pass_name(const String &p_name, uint32_t p_view, bool p_multiview) {
	if (!p_multiview) {
		return p_name;
	}
	String stripped = p_name;
	if (stripped.begins_with("NextGI:")) {
		stripped = stripped.substr(7).strip_edges();
	}
	return vformat("NextGI:View%d:%s", p_view, stripped);
}

bool NextGI::nextgi_multiview_should_run_scenario(uint64_t p_last_frame, uint64_t p_frame) {
	// Once per scenario per frame, whoever the first NextGI view is.
	return p_last_frame != p_frame;
}

NextGI::DebugViewId NextGI::debug_view_from_viewport(int p_viewport_debug_draw) {
	switch (static_cast<RSE::ViewportDebugDraw>(p_viewport_debug_draw)) {
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_TIER:
			return DEBUG_VIEW_TIER;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_TLAS_HEAT:
			return DEBUG_VIEW_TLAS_HEAT;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_BLAS_COST:
			return DEBUG_VIEW_BLAS_COST;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_HIT_INSTANCE:
			return DEBUG_VIEW_HIT_INSTANCE;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_RT_PROXY:
			return DEBUG_VIEW_RT_PROXY;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SRC_CARDS:
			return DEBUG_VIEW_SRC_CARDS;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_WRC_PROBES:
			return DEBUG_VIEW_WRC_PROBES;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_CARD_COVERAGE:
			return DEBUG_VIEW_CARD_COVERAGE;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SCREEN_PROBES:
			return DEBUG_VIEW_SCREEN_PROBES;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_FALLBACK_REASONS:
			return DEBUG_VIEW_FALLBACK_REASONS;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_RAW:
			return DEBUG_VIEW_DENOISE_RAW;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_VARIANCE:
			return DEBUG_VIEW_DENOISE_VARIANCE;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_HISTORY:
			return DEBUG_VIEW_DENOISE_HISTORY;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_REJECTED:
			return DEBUG_VIEW_DENOISE_REJECTED;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_DENOISED:
			return DEBUG_VIEW_DENOISE_DENOISED;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_REFLECTION:
			return DEBUG_VIEW_REFLECTION;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SPECULAR_HIT:
			return DEBUG_VIEW_SPECULAR_HIT;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SPECULAR_RAW:
			return DEBUG_VIEW_SPECULAR_RAW;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SPECULAR_VARIANCE:
			return DEBUG_VIEW_SPECULAR_VARIANCE;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SPECULAR_HISTORY:
			return DEBUG_VIEW_SPECULAR_HISTORY;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SPECULAR_REJECTED:
			return DEBUG_VIEW_SPECULAR_REJECTED;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SPECULAR_DENOISED:
			return DEBUG_VIEW_SPECULAR_DENOISED;
		default:
			return DEBUG_VIEW_MAX;
	}
}

NextGI::DebugView NextGI::resolve_debug_view(DebugViewId p_view, const TierResult &p_result) {
	DebugView view;
	if (p_view == DEBUG_VIEW_TIER) {
		view.has_data = true;
		view.tint = p_result.tier == TIER_HWRT ? debug_tint_hwrt() : debug_tint_fallback();
		view.label = vformat("%s %s", get_tier_name(p_result.tier), get_reason_name(p_result.reason));
		return view;
	}

	view.has_data = false;
	view.tint = debug_tint_no_data();
	view.label = "no data";
	if (p_view >= DEBUG_VIEW_DENOISE_RAW && p_view <= DEBUG_VIEW_DENOISE_DENOISED) {
		// The denoise passes write these sources; the renderer copies the texture for the view.
		static const char *const denoise_labels[] = {
			"denoise raw",
			"denoise variance",
			"denoise history length",
			"denoise rejected history",
			"denoise result",
		};
		view.label = denoise_labels[p_view - DEBUG_VIEW_DENOISE_RAW];
	}
	if (p_view == DEBUG_VIEW_REFLECTION) {
		view.label = "reflection only";
	}
	if (p_view == DEBUG_VIEW_SPECULAR_HIT) {
		view.label = "specular hit distance";
	}
	if (p_view == DEBUG_VIEW_SPECULAR_RAW) {
		view.label = "specular raw";
	}
	if (p_view == DEBUG_VIEW_SPECULAR_VARIANCE) {
		view.label = "specular variance";
	}
	if (p_view == DEBUG_VIEW_SPECULAR_HISTORY) {
		view.label = "specular history length";
	}
	if (p_view == DEBUG_VIEW_SPECULAR_REJECTED) {
		view.label = "specular rejected history";
	}
	if (p_view == DEBUG_VIEW_SPECULAR_DENOISED) {
		view.label = "specular denoised";
	}
	if (p_view == DEBUG_VIEW_FALLBACK_REASONS) {
		// The per-instance reasons are the legend of this view. They are not tier reasons.
		view.label = "no data: NOT_IN_RT_SCENE, RT_INSTANCE_CAP, RT_DYNAMIC_EXCLUDED";
	}
	return view;
}

bool NextGI::is_tier_reason_name(const String &p_name) {
	for (int i = 0; i < TIER_REASON_MAX; i++) {
		if (p_name == _nextgi_reason_names[i]) {
			return true;
		}
	}
	return false;
}

Vector<String> NextGI::get_instance_fallback_reason_names() {
	Vector<String> names;
	names.push_back("NOT_IN_RT_SCENE");
	names.push_back("RT_INSTANCE_CAP");
	names.push_back("RT_DYNAMIC_EXCLUDED");
	names.push_back("ALPHA_TESTED_RT_EXCLUDED");
	return names;
}

Dictionary NextGI::get_tier_info() const {
	Dictionary info;
	info["tier"] = get_tier_name(result.tier);
	info["reason"] = get_reason_name(result.reason);
	const bool active = result.tier == TIER_HWRT || result.tier == TIER_FALLBACK;
	info["quality"] = active ? get_quality_name(resolve_quality(quality_setting)) : String("NONE");
	info["ray_query_shaders_initialized"] = ray_query_shaders_initialized;
	info["rough_reflection_mip_count"] = int64_t(rough_reflection_mip_count);
	// GodotNext: nextgi_rt_scene
	if (rt_scene != nullptr) {
		const NextGIRTScene::Stats &rt_stats = rt_scene->get_stats();
		info["rt_fed_instance_count"] = int64_t(rt_stats.fed_instance_count);
		info["rt_instance_count"] = int64_t(rt_stats.instance_count);
		info["rt_blas_build_count"] = int64_t(rt_stats.blas_build_count);
		info["rt_blas_build_triangles"] = int64_t(rt_stats.blas_build_triangles);
		info["rt_compaction_copies"] = int64_t(rt_stats.compaction_copies);
		info["rt_refit_triangles"] = int64_t(rt_stats.refit_triangles);
		// GodotNext: nextgi_skinned_actors (#59).
		info["rt_skinned_instances"] = int64_t(rt_stats.skinned_instances);
		info["rt_skinned_refit_triangles"] = int64_t(rt_stats.skinned_refit_triangles);
		info["rt_skinned_build_triangles"] = int64_t(rt_stats.skinned_build_triangles);
		info["rt_as_bytes"] = int64_t(rt_stats.as_bytes);
		info["rt_tlas_fill_ms"] = rt_stats.tlas_fill_ms;
		info["rt_scene_update_ms"] = rt_stats.scene_update_ms;
		info["rt_tlas_build_count"] = int64_t(rt_stats.tlas_build_count);
		info["rt_as_memory_exceeded"] = rt_scene->is_as_memory_exceeded();
		if (rt_scene->get_far_field() != nullptr) {
			const NextGIFarField::Stats &far_stats = rt_scene->get_far_field()->get_stats();
			info["far_field_cell_count"] = int64_t(far_stats.cell_count);
			info["far_field_pending_count"] = int64_t(far_stats.pending_count);
			info["far_field_proxy_bytes"] = int64_t(far_stats.proxy_bytes);
			info["far_field_budget_exceeded"] = far_stats.budget_exceeded;
			info["far_field_tlas_build_count"] = int64_t(far_stats.tlas_build_count);
			info["far_field_trace_ready"] = rt_scene->get_far_field()->is_trace_ready();
			info["far_field_near_cap"] = int64_t(NEXTGI_RT_TLAS_CAPACITY);
			info["far_field_occlusion_only"] = bool(GLOBAL_GET("rendering/global_illumination/nextgi/far_field_occlusion_only"));
		}
		if (bool(GLOBAL_GET("rendering/global_illumination/nextgi/debug/global_sdf"))) {
			info.merge(rt_scene->get_global_sdf_debug());
		}
		// GodotNext: ps_mesh_sdf_atlas (#185).
		if (MeshStorage::get_singleton() != nullptr) {
			NextGISDFAtlas &sdf_atlas = MeshStorage::get_singleton()->get_sdf_atlas();
			info["mesh_sdf_resident_bytes"] = int64_t(sdf_atlas.get_resident_bytes());
			info["mesh_sdf_pool_used"] = int64_t(sdf_atlas.get_pool_used());
			info["mesh_sdf_pool_capacity"] = int64_t(sdf_atlas.get_pool_capacity());
			info["mesh_sdf_jobs"] = int64_t(MeshStorage::get_singleton()->get_sdf_job_count());
			info["mesh_sdf_pending"] = int64_t(MeshStorage::get_singleton()->get_sdf_pending_count());
			if (rt_scene != nullptr) {
				const NextGISDFInstances &sdf_instances = rt_scene->get_sdf_instances();
				info["sdf_instance_count"] = int64_t(sdf_instances.get_count());
				info["sdf_unsupported_deformation"] = int64_t(sdf_instances.get_reason_count(NEXTGI_SDF_UNSUPPORTED_DEFORMATION));
				info["sdf_no_field"] = int64_t(sdf_instances.get_reason_count(NEXTGI_SDF_NO_FIELD));
			}
		}
	}
	if (MeshStorage::get_singleton() != nullptr) {
		const MeshStorage::NextGIRTStats ms_stats = MeshStorage::get_singleton()->mesh_nextgi_get_rt_stats();
		info["blas_bytes_compacted"] = int64_t(ms_stats.blas_bytes_compacted);
		info["blas_bytes_uncompacted"] = int64_t(ms_stats.blas_bytes_uncompacted);
		info["rt_arena_bytes"] = int64_t(ms_stats.rt_arena_bytes);
		info["proxy_blas_bytes_compacted"] = int64_t(ms_stats.proxy_blas_bytes_compacted);
		info["proxy_blas_bytes_uncompacted"] = int64_t(ms_stats.proxy_blas_bytes_uncompacted);
		info["proxy_triangles"] = int64_t(ms_stats.proxy_triangles);
		info["proxy_blas_parts"] = int64_t(ms_stats.proxy_blas_parts);
	}
	// GodotNext: nextgi_src_cards
	if (src != nullptr) {
		src->fill_info(info);
	}
	if (relight != nullptr) {
		relight->fill_info(info);
	}
	if (gather != nullptr) {
		gather->fill_info(info);
	}
	if (reference != nullptr) {
		reference->fill_info(info);
	}
	if (wrc != nullptr) {
		wrc->fill_info(info);
	}
	if (front_layer != nullptr) {
		front_layer->fill_info(info);
	}
	// GodotNext: ps_voxel_lighting (#188)
	if (voxel_lighting != nullptr) {
		info["voxel_lighting_retrace"] = int64_t(voxel_lighting->get_retrace_rays());
		info["voxel_lighting_coverage"] = voxel_lighting->get_coverage();
		info["voxel_lighting_coverage_ppm"] = voxel_lighting->get_coverage_ppm();
		info["voxel_lighting_volume_bytes"] = int64_t(voxel_lighting->get_allocated_bytes());
		info["voxel_lighting_stored_hits"] = int64_t(voxel_lighting->get_stored_hits());
		info["voxel_lighting_resampled_nonzero"] = int64_t(voxel_lighting->get_resampled_nonzero());
	}
	return info;
}

void NextGI::composite_reference(RID p_color) {
	if (int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_mode")) == 0) {
		return;
	}
	// The frame block does not run when the HWRT tier is inactive, so this is the path that logs that case.
	if (reference == nullptr) {
		reference = memnew(NextGIReference);
	}
	reference->composite(p_color);
}

RID NextGI::get_specular_filter_debug_texture(DebugViewId p_view) const {
	return gather != nullptr ? gather->get_specular_filter_debug_texture(int(p_view)) : RID();
}

RID NextGI::get_specular_debug_texture() const {
	return gather != nullptr ? gather->get_specular_debug_texture() : RID();
}

RID NextGI::get_denoise_debug_texture(DebugViewId p_view) const {
	return gather != nullptr ? gather->get_debug_texture(int(p_view)) : RID();
}

void NextGI::_gather(RenderDataRD *p_render_data) {
	if (gather != nullptr) {
		gather->gather(p_render_data, !is_reflections_only());
	}
}

void NextGI::_integrate(RenderDataRD *p_render_data) {
	if (gather != nullptr) {
		gather->integrate(p_render_data);
	}
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	if (rb.is_null() || is_reflections_only() || is_reflections_enabled()) {
		return;
	}
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	ERR_FAIL_NULL(scene);
	const uint32_t layers = rb->get_view_count();
	const uint32_t mipmaps = rb->get_texture_format(RB_SCOPE_GI, RB_TEX_AMBIENT).mipmaps;
	for (uint32_t layer = 0; layer < layers; layer++) {
		RID source = rb->get_texture_slice(RB_SCOPE_GI, RB_TEX_AMBIENT, layer, 0);
		for (uint32_t mip = 1; mip < mipmaps; mip++) {
			const RID destination = rb->get_texture_slice(RB_SCOPE_GI, RB_TEX_AMBIENT, layer, mip);
			scene->get_copy_effects()->make_mipmap(source, destination, rb->get_texture_slice_size(RB_SCOPE_GI, RB_TEX_AMBIENT, mip));
			source = destination;
		}
	}
}

void NextGI::_relight_src(RenderDataRD *p_render_data) {
	if (relight != nullptr) {
		relight->relight(p_render_data);
	}
	// GodotNext: ps_voxel_lighting (#188). Same slot as SRC relight, after capture.
	update_voxel_lighting(p_render_data);
}

// GodotNext: ps_voxel_lighting (#188)
void NextGI::update_voxel_lighting(RenderDataRD *p_render_data) {
	// A tier change drops the volume before any early-out, including probe frames.
	if (voxel_lighting_tier_valid && voxel_lighting_tier != result.tier) {
		if (voxel_lighting != nullptr) {
			memdelete(voxel_lighting);
			voxel_lighting = nullptr;
		}
		if (swrt_trace != nullptr) {
			memdelete(swrt_trace);
			swrt_trace = nullptr;
		}
		voxel_lighting_frame = UINT64_MAX;
	}
	voxel_lighting_tier = result.tier;
	voxel_lighting_tier_valid = true;
	if (result.tier != TIER_HWRT && result.tier != TIER_FALLBACK) {
		return;
	}
	// Empty scenes and reflection probes are not the scenario view. Stamp the
	// frame only after this check, so they cannot consume the frame.
	if (p_render_data == nullptr || p_render_data->scene_data == nullptr || p_render_data->reflection_probe.is_valid() || p_render_data->sdfgi_update_data == nullptr) {
		return;
	}
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	if (scene == nullptr) {
		return;
	}
	const RSE::ViewportDebugDraw debug_draw = scene->get_debug_draw_mode();
	if (debug_draw == RSE::VIEWPORT_DEBUG_DRAW_UNSHADED || debug_draw == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW) {
		return;
	}
	// Nothing is allocated until the Environment actually enables NextGI.
	if (!p_render_data->environment.is_valid() || !scene->environment_get_nextgi_enabled(p_render_data->environment)) {
		return;
	}
	const uint64_t frame = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0;
	if (frame == voxel_lighting_frame) {
		return;
	}
	voxel_lighting_frame = frame;
	if (voxel_lighting == nullptr) {
		voxel_lighting = memnew(NextGIVoxelLighting);
	}
	voxel_lighting->update_render(p_render_data, rt_scene, relight, src);
	// GodotNext: ps_swrt_trace (#186). The ray-query builder marks a whole ShaderRD,
	// so the software trace is a sibling of the HWRT gather and runs only while
	// the fallback tier is active. #187 points the frame block at it.
	if (result.tier == TIER_FALLBACK) {
		if (swrt_trace == nullptr) {
			swrt_trace = memnew(NextGISWRTTrace);
		}
		swrt_trace->update_frame(p_render_data, voxel_lighting);
	} else if (swrt_trace != nullptr) {
		memdelete(swrt_trace);
		swrt_trace = nullptr;
	}
}

bool NextGI::view_uses_frame_block(Tier p_tier, bool p_reflection_probe, bool p_environment_enabled, bool p_unshaded) {
	// Reflection probes sample the WRC and the sky only (plan §6.1, multiple views).
	return p_tier == TIER_HWRT && !p_reflection_probe && p_environment_enabled && !p_unshaded;
}

Size2i NextGI::get_gi_buffer_size(const Size2i &p_internal_size, bool p_half_resolution) {
	Size2i size = p_internal_size;
	if (p_half_resolution) {
		size.x >>= 1;
		size.y >>= 1;
	}
	return size;
}

uint32_t NextGI::get_gi_buffer_usage_bits() {
	return RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
}

bool NextGI::gi_buffers_need_reset(bool p_has_buffers, uint32_t p_usage_bits, bool p_using_half_size, bool p_half_resolution) {
	if (p_using_half_size != p_half_resolution) {
		return true;
	}
	const uint32_t required = get_gi_buffer_usage_bits();
	return p_has_buffers && (p_usage_bits & required) != required;
}

void NextGI::ensure_gi_buffers(RenderSceneBuffersRD *p_render_buffers, bool p_half_resolution) {
	ERR_FAIL_NULL(p_render_buffers);
	Ref<GI::RenderBuffersGI> rbgi = p_render_buffers->get_custom_data(RB_SCOPE_GI);
	ERR_FAIL_COND(rbgi.is_null());

	const bool has_buffers = p_render_buffers->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT);
	uint32_t usage_bits = 0;
	if (has_buffers) {
		usage_bits = RenderingDevice::get_singleton()->texture_get_format(p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)).usage_bits;
	}
	if (gi_buffers_need_reset(has_buffers, usage_bits, rbgi->using_half_size_gi, p_half_resolution)) {
		p_render_buffers->clear_context(RB_SCOPE_GI);
	}

	if (!p_render_buffers->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)) {
		const Size2i size = get_gi_buffer_size(p_render_buffers->get_internal_size(), p_half_resolution);
		const uint32_t usage = get_gi_buffer_usage_bits();
		const uint32_t mipmaps = Image::get_image_required_mipmaps(size.x, size.y, Image::FORMAT_RGBAH);
		p_render_buffers->create_texture(RB_SCOPE_GI, RB_TEX_AMBIENT, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RenderingDevice::TEXTURE_SAMPLES_1, size, 0, mipmaps);
		p_render_buffers->create_texture(RB_SCOPE_GI, RB_TEX_REFLECTION, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RenderingDevice::TEXTURE_SAMPLES_1, size);
		p_render_buffers->create_texture(RB_SCOPE_GI, RB_TEX_SPECULAR_HIT, RenderingDevice::DATA_FORMAT_R16_SFLOAT, usage, RenderingDevice::TEXTURE_SAMPLES_1, size);
		rough_reflection_mip_count = mipmaps;
		rbgi->using_half_size_gi = p_half_resolution;
	}
}

void NextGI::_clear_gi_buffers(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_COND(!p_render_buffers->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT));
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const uint32_t layers = p_render_buffers->get_view_count();
	// Alpha 0: the opaque pass mixes the buffers in by their alpha, so it keeps its own ambient and specular.
	rd->texture_clear(p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT), Color(0, 0, 0, 0), 0, 1, 0, layers);
}

void NextGI::_clear_gi_reflection(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_COND(!p_render_buffers->has_texture(RB_SCOPE_GI, RB_TEX_REFLECTION));
	// The reflect pass (#60) writes every trace pixel, so the clear only covers frames where the
	// specular trace is unavailable. Alpha 0 keeps the opaque pass's own specular.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const uint32_t layers = p_render_buffers->get_view_count();
	rd->texture_clear(p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION), Color(0, 0, 0, 0), 0, 1, 0, layers);
	if (p_render_buffers->has_texture(RB_SCOPE_GI, RB_TEX_SPECULAR_HIT)) {
		// Misses are 65504 in the trace; clear to the same value so a skipped frame stays a miss.
		rd->texture_clear(p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_SPECULAR_HIT), Color(65504.0, 0, 0, 0), 0, 1, 0, layers);
	}
}

RID NextGI::get_specular_hit_distance(RenderSceneBuffersRD *p_render_buffers, int p_view) const {
	if (p_render_buffers == nullptr || !p_render_buffers->has_texture(RB_SCOPE_GI, RB_TEX_SPECULAR_HIT)) {
		return RID();
	}
	return p_render_buffers->get_texture_slice(RB_SCOPE_GI, RB_TEX_SPECULAR_HIT, MAX(p_view, 0), 0);
}

void NextGI::render_frame(RenderDataRD *p_render_data, const RID *p_normal_roughness_slices, bool p_half_resolution) {
	// The shadow batch fills RENDER_LIST_SECONDARY, which SRC card capture shares (plan §4.1).
	DEV_ASSERT(!shadow_batch_open);
	ERR_FAIL_COND_MSG(shadow_batch_open, "The NextGI frame block must not run inside a shadow batch.");
	ERR_FAIL_NULL(p_render_data);
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());
	ERR_FAIL_COND_MSG(p_normal_roughness_slices == nullptr || p_normal_roughness_slices[0].is_null(), "The NextGI frame block needs the depth + normal-roughness prepass.");

	_maybe_simulate_device_lost();
	// GodotNext: nextgi_quality_presets (#55). Poll the preset so a runtime change reallocates.
	_settings_changed();

	// GodotNext: nextgi_src_relight
	// Pages are assigned during the TLAS phase, which runs after card capture and before relight.
	if (relight == nullptr) {
		relight = memnew(NextGIRelight);
		relight->init();
	}
	if (gather == nullptr) {
		gather = memnew(NextGIGather);
		gather->init();
	}
	if (reference == nullptr) {
		reference = memnew(NextGIReference);
	}
	reference->invalidate_tables();

	RenderingDevice *rd = RenderingDevice::get_singleton();
	RENDER_TIMESTAMP(PASS_SLOTS[PASS_SLOT_BEGIN].name);
	rd->draw_command_begin_label("NextGI");

	ensure_gi_buffers(rb.ptr(), p_half_resolution);

	// GodotNext: nextgi_multiview (#58). Scenario passes run once per scenario per frame; the
	// per-view passes (screen traces, gather, reflections, integration, denoise) run per view.
	const uint64_t render_frame_number = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0;
	const bool new_scenario = nextgi_multiview_should_run_scenario(scenario_frame, render_frame_number);
	if (new_scenario) {
		scenario_frame = render_frame_number;
		view_ordinal = 0;
	}
	const uint32_t view_index = view_ordinal++;
	const bool multiview = view_index > 0 || rb->get_view_count() > 1;

	// Every slot emits its timestamp, also while it has no work yet.
	for (int slot = PASS_SLOT_BEGIN + 1; slot < PASS_SLOT_END; slot++) {
		const bool scenario_slot = slot == PASS_SLOT_AS_UPDATES || slot == PASS_SLOT_AS_BUILDS || slot == PASS_SLOT_SRC_CAPTURE || slot == PASS_SLOT_SRC_RELIGHT || slot == PASS_SLOT_WRC_UPDATE;
		if (scenario_slot && !new_scenario) {
			// Secondary views reuse this frame's AS, SRC and WRC work.
			continue;
		}
		RENDER_TIMESTAMP(nextgi_multiview_pass_name(PASS_SLOTS[slot].name, view_index, multiview));
		rd->draw_command_begin_label(Span<char>(PASS_SLOTS[slot].name, strlen(PASS_SLOTS[slot].name)));
		// GodotNext: nextgi_as_slots
		// The RT scene runs inside its two slots (#243): the TLAS rebuild in "AS updates"
		// (line 1a) and the BLAS builds in "AS builds" (line 1b), so the slot timestamps
		// carry the AS cost. "AS updates" precedes "AS builds", so a BLAS built this frame
		// reaches the TLAS through next frame's update: a one-frame lag. An instance enters
		// the TLAS only once its BLAS exists (plan §8.2), and rebuild_tlas skips dirty
		// meshes until then, which keeps every TLAS build correct.
		// GodotNext: nextgi_src_cards — card capture is the "SRC capture" slot (line 2a).
		if (slot == PASS_SLOT_AS_UPDATES && rt_scene != nullptr) {
			rt_scene->rebuild_tlas(p_render_data);
		} else if (slot == PASS_SLOT_AS_BUILDS && rt_scene != nullptr) {
			rt_scene->build_dirty_blas(p_render_data);
			TierInputs budget_inputs = inputs;
			budget_inputs.as_memory_ok = !rt_scene->is_as_memory_exceeded();
			String budget_line;
			if (update_tier(budget_inputs, quality_setting, budget_line)) {
				log_tier_selection(budget_line);
			}
		} else if (slot == PASS_SLOT_SRC_CAPTURE) {
			_capture_src_cards(p_render_data);
		} else if (slot == PASS_SLOT_SRC_RELIGHT) {
			_relight_src(p_render_data);
		} else if (slot == PASS_SLOT_SCREEN_TRACES) {
			Ref<RenderSceneBuffersRD> buffers = p_render_data->render_buffers;
			SSEffects *ss_effects = SSEffects::get_singleton();
			if (buffers.is_valid() && ss_effects != nullptr) {
				ss_effects->build_hiz(buffers);
			}
		} else if (slot == PASS_SLOT_WRC_UPDATE) {
			ensure_wrc()->update(p_render_data);
		} else if (slot == PASS_SLOT_DIFFUSE_GATHER) {
			_clear_gi_reflection(rb.ptr());
			_gather(p_render_data);
		} else if (slot == PASS_SLOT_INTEGRATION) {
			// Integration (§2.1 line 7) writes RB_TEX_AMBIENT from the gather probes. Reflection stays cleared.
			// Reflections-only keeps the ambient writer (LightmapGI/VoxelGI) untouched.
			if (!is_reflections_only()) {
				_clear_gi_buffers(rb.ptr());
				_integrate(p_render_data);
			}
		}
		rd->draw_command_end_label();
	}

	// The reference sample uses the TLAS built earlier in this frame, before compaction.
	reference->render(p_render_data);
	// Compaction copies invalidate the TLAS when the device frame restarts. Card capture can
	// force that restart with a readback, so the copy is queued only after this frame's traces.
	if (rt_scene != nullptr) {
		rt_scene->compact_after_traces();
	}
	if (result.tier != TIER_HWRT) {
		_ensure_rt_scene();
	}

	rd->draw_command_end_label();
	RENDER_TIMESTAMP(nextgi_multiview_pass_name(PASS_SLOTS[PASS_SLOT_END].name, view_index, multiview));
}

NextGI::NextGI() {
	// Doctests build their own instances; the renderer's instance is the first one.
	if (singleton == nullptr) {
		singleton = this;
	}
}

void NextGI::free_far_field() {
	if (rt_scene != nullptr) {
		rt_scene->free_far_field();
	}
}

NextGI::~NextGI() {
	_unregister_device_lost_callback();
	// GodotNext: nextgi_src_cards
	if (src != nullptr) {
		src->free();
		memdelete(src);
		src = nullptr;
	}
	if (gather != nullptr) {
		memdelete(gather);
		gather = nullptr;
	}
	if (reference != nullptr) {
		reference->free();
		memdelete(reference);
		reference = nullptr;
	}
	if (wrc != nullptr) {
		wrc->free_resources();
		memdelete(wrc);
		wrc = nullptr;
	}
	if (relight != nullptr) {
		memdelete(relight);
		relight = nullptr;
	}
	// GodotNext: ps_voxel_lighting (#188)
	if (voxel_lighting != nullptr) {
		memdelete(voxel_lighting);
		voxel_lighting = nullptr;
	}
	// GodotNext: ps_swrt_trace (#186)
	if (swrt_trace != nullptr) {
		memdelete(swrt_trace);
		swrt_trace = nullptr;
	}

	// GodotNext: nextgi_rt_scene
	if (rt_scene != nullptr) {
		rt_scene->free();
		memdelete(rt_scene);
		rt_scene = nullptr;
	}

	if (singleton == this) {
		if (MeshStorage::get_singleton() != nullptr) {
			MeshStorage::get_singleton()->mesh_nextgi_set_hwrt_active(false);
		}
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
