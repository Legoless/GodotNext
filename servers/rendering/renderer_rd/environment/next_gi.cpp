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
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "servers/rendering/renderer_rd/environment/gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/shader_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_rq_selftest.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/rendering_server_globals.h"

// GodotNext: nextgi_src_cards
#include "servers/rendering/renderer_rd/environment/next_gi_gather.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"

using namespace RendererRD;

NextGI *NextGI::singleton = nullptr;

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
	if (result.tier == TIER_HWRT && rt_scene == nullptr && rd != nullptr && rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY)) {
		rt_scene = memnew(NextGIRTScene);
		rt_scene->init();
	}
}

void NextGI::notify_ray_query_shader_initialized() {
	ERR_FAIL_COND_MSG(!can_initialize_ray_query_shaders(), "Ray-query ShaderRDs are initialized only under the NextGI HWRT tier.");
	ray_query_shaders_initialized++;
}

Dictionary NextGI::get_status() const {
	Dictionary status;
	const bool active = result.tier == TIER_HWRT || result.tier == TIER_FALLBACK;
	status["tier"] = get_tier_name(result.tier);
	status["reason"] = get_reason_name(result.reason);
	status["quality"] = active ? get_quality_name(resolve_quality(quality_setting)) : String("NONE");
	if (!active) {
		status["quality_reason"] = "NONE";
	} else if (quality_setting == QUALITY_AUTO) {
		status["quality_reason"] = "auto selection pending";
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
	}
	return counters;
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
	return names;
}

Dictionary NextGI::get_tier_info() const {
	Dictionary info;
	info["tier"] = get_tier_name(result.tier);
	info["reason"] = get_reason_name(result.reason);
	const bool active = result.tier == TIER_HWRT || result.tier == TIER_FALLBACK;
	info["quality"] = active ? get_quality_name(resolve_quality(quality_setting)) : String("NONE");
	info["ray_query_shaders_initialized"] = ray_query_shaders_initialized;
	// GodotNext: nextgi_rt_scene
	if (rt_scene != nullptr) {
		const NextGIRTScene::Stats &rt_stats = rt_scene->get_stats();
		info["rt_fed_instance_count"] = int64_t(rt_stats.fed_instance_count);
		info["rt_instance_count"] = int64_t(rt_stats.instance_count);
		info["rt_blas_build_count"] = int64_t(rt_stats.blas_build_count);
		info["rt_blas_build_triangles"] = int64_t(rt_stats.blas_build_triangles);
		info["rt_compaction_copies"] = int64_t(rt_stats.compaction_copies);
		info["rt_refit_triangles"] = int64_t(rt_stats.refit_triangles);
		info["rt_as_bytes"] = int64_t(rt_stats.as_bytes);
		info["rt_tlas_fill_ms"] = rt_stats.tlas_fill_ms;
		info["rt_scene_update_ms"] = rt_stats.scene_update_ms;
		info["rt_tlas_build_count"] = int64_t(rt_stats.tlas_build_count);
		info["rt_as_memory_exceeded"] = rt_scene->is_as_memory_exceeded();
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

void NextGI::_gather(RenderDataRD *p_render_data) {
	if (gather != nullptr) {
		gather->gather(p_render_data);
	}
}

void NextGI::_integrate(RenderDataRD *p_render_data) {
	if (gather != nullptr) {
		gather->integrate(p_render_data);
	}
}

void NextGI::_relight_src(RenderDataRD *p_render_data) {
	if (relight != nullptr) {
		relight->relight(p_render_data);
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
		p_render_buffers->create_texture(RB_SCOPE_GI, RB_TEX_AMBIENT, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RenderingDevice::TEXTURE_SAMPLES_1, size);
		p_render_buffers->create_texture(RB_SCOPE_GI, RB_TEX_REFLECTION, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RenderingDevice::TEXTURE_SAMPLES_1, size);
		rbgi->using_half_size_gi = p_half_resolution;
	}
}

void NextGI::_clear_gi_buffers(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_COND(!p_render_buffers->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT) || !p_render_buffers->has_texture(RB_SCOPE_GI, RB_TEX_REFLECTION));
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const uint32_t layers = p_render_buffers->get_view_count();
	// Alpha 0: the opaque pass mixes the buffers in by their alpha, so it keeps its own ambient and specular.
	rd->texture_clear(p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT), Color(0, 0, 0, 0), 0, 1, 0, layers);
	rd->texture_clear(p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION), Color(0, 0, 0, 0), 0, 1, 0, layers);
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

	// Every slot emits its timestamp, also while it has no work yet.
	for (int slot = PASS_SLOT_BEGIN + 1; slot < PASS_SLOT_END; slot++) {
		RENDER_TIMESTAMP(PASS_SLOTS[slot].name);
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
		} else if (slot == PASS_SLOT_WRC_UPDATE) {
			if (wrc == nullptr) {
				wrc = memnew(NextGIWRC);
			}
			wrc->update(p_render_data);
		} else if (slot == PASS_SLOT_DIFFUSE_GATHER) {
			_gather(p_render_data);
		} else if (slot == PASS_SLOT_INTEGRATION) {
			// Integration (§2.1 line 7) writes RB_TEX_AMBIENT from the gather probes. Reflection stays cleared.
			_clear_gi_buffers(rb.ptr());
			_integrate(p_render_data);
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

	rd->draw_command_end_label();
	RENDER_TIMESTAMP(PASS_SLOTS[PASS_SLOT_END].name);
}

NextGI::NextGI() {
	// Doctests build their own instances; the renderer's instance is the first one.
	if (singleton == nullptr) {
		singleton = this;
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

	// GodotNext: nextgi_rt_scene
	if (rt_scene != nullptr) {
		rt_scene->free();
		memdelete(rt_scene);
		rt_scene = nullptr;
	}

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
