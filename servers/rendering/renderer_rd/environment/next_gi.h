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

#include "core/math/color.h"
#include "core/math/vector2i.h"
#include "core/string/ustring.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/rendering_device_enums.h"

class RenderDataRD;
class RenderingDevice;
class RenderSceneBuffersRD;

// GodotNext: nextgi_rt_scene
class RenderDataRD;

namespace RendererRD {

// GodotNext: nextgi_rt_scene
class NextGIRTScene;

// GodotNext: nextgi_src_cards
class NextGISRC;
struct NextGICardClusterResult;

// GodotNext: nextgi_src_relight
class NextGIRelight;

// GodotNext: nextgi_gather
class NextGIGather;
class NextGIReference;
class NextGIWRC;

// NextGI host entry: the §3 tier selection (#15), the ray-query toolchain self-test (#20), the
// Forward+ frame block (#26), the RT scene (#28) and the HWRT safety net (#36): the startup
// 64-ray self-test and the device-loss crash latch.
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
		// #36 feeds selftest_passed and fault_latched. The AS budget stays a pass until #39.
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

	// The frame block's fixed slots, in plan §6.1 step 5 order. Each slot opens one RD timestamp
	// named for its §2.1 line (Q11). Lines 1a/1b and 2a/2b have separate slots for G1-3.
	enum PassSlot {
		PASS_SLOT_BEGIN, // "> NextGI"
		PASS_SLOT_SRC_CAPTURE, // 2a
		PASS_SLOT_AS_UPDATES, // 1a
		PASS_SLOT_AS_BUILDS, // 1b
		PASS_SLOT_SRC_RELIGHT, // 2b
		PASS_SLOT_WRC_UPDATE, // 3
		PASS_SLOT_SCREEN_TRACES, // 4
		PASS_SLOT_DIFFUSE_GATHER, // 5
		PASS_SLOT_REFLECTION_TRACES, // 6
		PASS_SLOT_INTEGRATION, // 7
		PASS_SLOT_DENOISE, // 8
		PASS_SLOT_END, // "< NextGI"
		PASS_SLOT_MAX,
	};

	struct PassSlotInfo {
		const char *name; // The RD timestamp and the debug label.
		const char *line; // The §2.1 line; empty for the two block markers.
	};

	// The name-to-line table, in emission order. The timing runner maps timestamp names to §2.1
	// lines with it and keeps line 1b out of the steady-state p95 (Q11).
	static const PassSlotInfo PASS_SLOTS[PASS_SLOT_MAX];

	// Startup self-test (#36). 48 hits, each at least 1e-3 in barycentric distance from every
	// edge and with a distinct t, then 16 misses. One triangle, one instance, hit_sbt_range 0.
	static constexpr int HWRT_SELFTEST_RAY_COUNT = 64;
	static constexpr int HWRT_SELFTEST_HIT_COUNT = 48;
	static constexpr int HWRT_SELFTEST_MISS_COUNT = 16;
	static constexpr double HWRT_SELFTEST_REL_T = 1e-5;
	static constexpr double HWRT_SELFTEST_ABS_BARY = 1e-5;
	static constexpr double HWRT_SELFTEST_MIN_EDGE = 1e-3;

	// std430 layout of the self-test ray: origin.xyz, tmin, dir.xyz, tmax. 32 bytes.
	struct HWRTSelfTestRay {
		float origin_x = 0.0f;
		float origin_y = 0.0f;
		float origin_z = 0.0f;
		float tmin = 0.0f;
		float dir_x = 0.0f;
		float dir_y = 0.0f;
		float dir_z = 0.0f;
		float tmax = 0.0f;
	};

	// std430 layout of NextGIRayResult. 32 bytes.
	struct HWRTSelfTestRecord {
		float t = 0.0f;
		uint32_t custom_index = 0;
		uint32_t geometry_index = 0;
		uint32_t primitive = 0;
		float bary_u = 0.0f;
		float bary_v = 0.0f;
		uint32_t front_face = 0;
		uint32_t hit = 0;
	};

	struct HWRTFaultFile {
		bool valid = false;
		String driver_key;
		int64_t time = 0;
	};

	struct HWRTFaultLatch {
		bool latched = false;
		bool file_present = false;
	};

	// Captured when the callback is registered. The callback only writes this file.
	struct HWRTFaultCallbackState {
		String path;
		String driver_key;
	};

	static void fill_hwrt_selftest(HWRTSelfTestRay (&r_rays)[HWRT_SELFTEST_RAY_COUNT], HWRTSelfTestRecord (&r_expected)[HWRT_SELFTEST_RAY_COUNT]);
	// IDs exact, t within HWRT_SELFTEST_REL_T relative, barycentrics within HWRT_SELFTEST_ABS_BARY.
	// p_corrupt_expected_t adds 1 to the first expected t (GODOTNEXT_NEXTGI_SELFTEST_CORRUPT).
	static bool hwrt_selftest_records_match(const HWRTSelfTestRecord *p_got, const HWRTSelfTestRecord *p_expected, int p_count, bool p_corrupt_expected_t, String *r_reason = nullptr);
	static String hwrt_fault_file_path();
	static bool write_hwrt_fault_file(const String &p_path, const String &p_driver_key, int64_t p_time);
	static HWRTFaultFile read_hwrt_fault_file(const String &p_path);
	static void remove_hwrt_fault_file(const String &p_path);
	// Same key and not Force latches. A different key deletes the file. Force keeps the file and does not latch.
	static HWRTFaultLatch resolve_hwrt_fault_latch(const String &p_path, const String &p_driver_key, HardwareRayTracing p_setting);
	static void hwrt_device_lost_callback(void *p_userdata);

private:
	static NextGI *singleton;

	TierInputs inputs;
	TierResult result;
	Quality quality_setting = QUALITY_AUTO;
	bool reported = false;
	TierResult reported_result;
	int ray_query_shaders_initialized = 0;
	bool shadow_batch_open = false;
	// GodotNext: hwrt_safety_net
	bool selftest_report_time = false;
	uint64_t selftest_usec = 0;
	int hwrt_frame_index = 0;
	bool fault_callback_registered = false;
	HWRTFaultCallbackState fault_callback_state;

	void _report(const String &p_line, const TierResult &p_result) const;
	void _clear_gi_buffers(RenderSceneBuffersRD *p_render_buffers) const;
	void _apply_safety_net(TierInputs &r_inputs);
	void _sync_device_lost_callback();
	void _unregister_device_lost_callback();
	void _maybe_simulate_device_lost();
	bool _run_hwrt_selftest(RenderingDevice *p_device, uint64_t &r_usec);

	// GodotNext: nextgi_rt_scene
	NextGIRTScene *rt_scene = nullptr;
	void _ensure_rt_scene();

	// GodotNext: nextgi_src_cards
	// The SRC card sets and material atlas (next_gi_src.h), created by the first frame block.
	NextGISRC *src = nullptr;
	void _capture_src_cards(RenderDataRD *p_render_data);

	// GodotNext: nextgi_src_relight
	NextGIRelight *relight = nullptr;
	void _relight_src(RenderDataRD *p_render_data);

	// GodotNext: nextgi_gather
	NextGIGather *gather = nullptr;
	NextGIReference *reference = nullptr;
	NextGIWRC *wrc = nullptr;
	void _gather(RenderDataRD *p_render_data);
	void _integrate(RenderDataRD *p_render_data);

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
	Quality get_resolved_quality() const { return resolve_quality(quality_setting); }

	static String get_tier_name(Tier p_tier);
	static String get_reason_name(TierReason p_reason);
	static String get_quality_name(Quality p_quality);
	// The one printed line, e.g. "NextGI: tier=FALLBACK reason=D3D12_NO_DXR (SDFGI fallback) quality=HIGH (auto selection pending)".
	static String format_tier_line(const TierResult &p_result, Quality p_quality_setting);
	// Prints that line. Appends " selftest=<ms>ms" when this selection ran the startup self-test.
	void log_tier_selection(const String &p_line);

	// The startup self-test source, built the way ShaderRD would. The only ray-query ShaderRD
	// that is created before the HWRT decision, and it is freed again before the decision returns.
	static String build_hwrt_selftest_source();

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

	// GodotNext: nextgi_user_surface
	// {tier, reason, quality, quality_reason, driver}. reason is only a §6.3 tier reason.
	Dictionary get_status() const;
	// Counters whose producer has not merged are 0. TLAS instances come from the RT scene.
	Dictionary get_counters() const;

	enum DebugViewId {
		DEBUG_VIEW_TIER,
		DEBUG_VIEW_TLAS_HEAT,
		DEBUG_VIEW_BLAS_COST,
		DEBUG_VIEW_HIT_INSTANCE,
		DEBUG_VIEW_RT_PROXY,
		DEBUG_VIEW_SRC_CARDS,
		DEBUG_VIEW_WRC_PROBES,
		DEBUG_VIEW_CARD_COVERAGE,
		DEBUG_VIEW_SCREEN_PROBES,
		DEBUG_VIEW_FALLBACK_REASONS,
		DEBUG_VIEW_DENOISE_RAW,
		DEBUG_VIEW_DENOISE_VARIANCE,
		DEBUG_VIEW_DENOISE_HISTORY,
		DEBUG_VIEW_DENOISE_REJECTED,
		DEBUG_VIEW_DENOISE_DENOISED,
		DEBUG_VIEW_MAX,
	};

	struct DebugView {
		Color tint;
		String label;
		bool has_data = false;
	};

	DebugViewId active_debug_view = DEBUG_VIEW_MAX;

	static Color debug_tint_hwrt() { return Color(0.15, 0.72, 0.28); }
	static Color debug_tint_fallback() { return Color(0.90, 0.55, 0.08); }
	static Color debug_tint_no_data() { return Color(0.25, 0.28, 0.35); }

	// Maps a ViewportDebugDraw value. DEBUG_VIEW_MAX means it is not a NextGI view.
	static DebugViewId debug_view_from_viewport(int p_viewport_debug_draw);
	// #53 denoise views: the renderer publishes the active view each frame so the gather only
	// allocates and writes the debug images while one of them is on screen.
	void set_active_debug_view(DebugViewId p_view) { active_debug_view = p_view; }
	DebugViewId get_active_debug_view() const { return active_debug_view; }
	// Texture for DEBUG_VIEW_DENOISE_RAW/VARIANCE/HISTORY/REJECTED, or an invalid RID.
	RID get_denoise_debug_texture(DebugViewId p_view) const;
	// Tier view uses the green or amber tint. Every other view is a "no data" tint until its producer merges.
	static DebugView resolve_debug_view(DebugViewId p_view, const TierResult &p_result);
	static bool is_tier_reason_name(const String &p_name);
	static Vector<String> get_instance_fallback_reason_names();

	// Whether a view runs the frame block (plan §6.1): the HWRT tier, an Environment with
	// nextgi_enabled, and neither a reflection-probe render nor the unshaded debug draw.
	// Such a view skips GI::process_gi and forces the depth + normal-roughness prepass.
	static bool view_uses_frame_block(Tier p_tier, bool p_reflection_probe, bool p_environment_enabled, bool p_unshaded);

	// RB_TEX_AMBIENT and RB_TEX_REFLECTION have the size GI::process_gi allocates: the internal
	// size, halved per axis under gi/use_half_resolution.
	static Size2i get_gi_buffer_size(const Size2i &p_internal_size, bool p_half_resolution);
	// process_gi's usage bits plus CAN_COPY_TO, which texture_clear needs.
	static uint32_t get_gi_buffer_usage_bits();
	// True when the GI buffers must be dropped: the half-resolution mode changed (as in process_gi),
	// or existing buffers lack the usage bits NextGI needs (process_gi created them).
	static bool gi_buffers_need_reset(bool p_has_buffers, uint32_t p_usage_bits, bool p_using_half_size, bool p_half_resolution);
	// Mirrors the buffer setup of GI::process_gi with get_gi_buffer_usage_bits.
	void ensure_gi_buffers(RenderSceneBuffersRD *p_render_buffers, bool p_half_resolution);

	// _render_shadow_begin and _render_shadow_end bracket a shadow batch, which fills
	// RENDER_LIST_SECONDARY. The frame block never runs inside one.
	void notify_shadow_batch_begin() { shadow_batch_open = true; }
	void notify_shadow_batch_end() { shadow_batch_open = false; }
	bool is_shadow_batch_open() const { return shadow_batch_open; }

	// The frame block, run right after _render_shadow_end in _pre_opaque_render for views where
	// view_uses_frame_block holds. It writes RB_TEX_AMBIENT and RB_TEX_REFLECTION and emits every
	// PASS_SLOTS timestamp, empty slots included. Until the gather lands both buffers are cleared to
	// (0, 0, 0, 0), so the opaque pass keeps its own ambient and specular through the buffer alpha.
	void render_frame(RenderDataRD *p_render_data, const RID *p_normal_roughness_slices, bool p_half_resolution);
	// Writes the reference mean into the HDR color when reference_mode is on.
	void composite_reference(RID p_color);

	// GodotNext: nextgi_rt_scene
	NextGIRTScene *get_rt_scene() const { return rt_scene; }
	NextGIReference *get_reference() const { return reference; }
	NextGIWRC *get_wrc() const { return wrc; }

	// GodotNext: nextgi_src_cards
	// The §8.3 Stage-1 card clustering, nextgi_src_cluster_cards (next_gi_src.h). Pure, so doctests
	// need no device. p_positions holds xyz float triples.
	static void cluster_cards(const float *p_positions, uint32_t p_vertex_count, const uint32_t *p_indices, uint32_t p_index_count, NextGICardClusterResult &r_result);
	NextGISRC *get_src() const { return src; }

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
