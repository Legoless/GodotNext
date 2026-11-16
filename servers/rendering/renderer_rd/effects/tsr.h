/**************************************************************************/
/*  tsr.h                                                                 */
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

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/templates/safe_refcount.h"
#include "servers/rendering/renderer_rd/effects/temporal_upscaler.h"
#include "servers/rendering/renderer_rd/effects/tsr_presets.h"
#include "servers/rendering/renderer_rd/effects/tsr_stability.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_debug_view.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_disocclusion.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_history_update.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_persistent_store.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_rejection_aa.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_shading_rejection.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_stability_analysis.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_velocity_dilate.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

enum TSRTextureDomain : uint8_t {
	TSR_DOMAIN_INTERNAL = 0,
	TSR_DOMAIN_HISTORY = 1,
};

struct TSRTextureFormatEntry {
	const char *name;
	RD::DataFormat format;
	uint32_t bytes_per_pixel;
	TSRTextureDomain domain;
};

// Clean-room TSR RB_SCOPE_TSR format table (docs/godotnext/tsr-sources.md):
// 10 B per history pixel + 8 B per internal pixel (<= 14 B per history pixel on T6).
inline constexpr TSRTextureFormatEntry TSR_FORMAT_TABLE[] = {
	{ "history_color_0", RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32, 4, TSR_DOMAIN_HISTORY },
	{ "history_color_1", RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32, 4, TSR_DOMAIN_HISTORY },
	{ "history_meta_0", RD::DATA_FORMAT_R8_UNORM, 1, TSR_DOMAIN_HISTORY },
	{ "history_meta_1", RD::DATA_FORMAT_R8_UNORM, 1, TSR_DOMAIN_HISTORY },
	{ "dilated_velocity", RD::DATA_FORMAT_R16G16_SFLOAT, 4, TSR_DOMAIN_INTERNAL },
	{ "prev_depth", RD::DATA_FORMAT_R16_SFLOAT, 2, TSR_DOMAIN_INTERNAL },
	{ "disocclusion", RD::DATA_FORMAT_R8_UNORM, 1, TSR_DOMAIN_INTERNAL },
	{ "rejection", RD::DATA_FORMAT_R8_UNORM, 1, TSR_DOMAIN_INTERNAL },
};

// GodotNext: pt_tsr_stability (#198). The textures the stability set adds to
// RB_SCOPE_TSR, beside TSR_FORMAT_TABLE. TSR::stability_layout sizes them so that
// the whole scope stays inside T6; TSR::ensure_stability_textures allocates them.
enum TSRStabilityStore : uint8_t {
	TSR_STABILITY_STORE_PERSISTENT = 0, // Fraction of the output size, N layers per view.
	TSR_STABILITY_STORE_FLICKER = 1, // Internal size.
	TSR_STABILITY_STORE_THIN = 2, // Half the internal size.
};

struct TSRStabilityStoreEntry {
	const char *name;
	RD::DataFormat format;
	uint32_t bytes_per_pixel;
	uint32_t feature;
	TSRStabilityStore store;
	float clear_value;
};

inline constexpr TSRStabilityStoreEntry TSR_STABILITY_STORE_TABLE[] = {
	{ "persistent_frames", RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32, 4, TSR_FEATURE_RESURRECTION, TSR_STABILITY_STORE_PERSISTENT, 0.0f },
	// Cleared to "analysis disabled" (0x07).
	{ "flicker_state_0", RD::DATA_FORMAT_R8_UNORM, 1, TSR_FEATURE_FLICKER, TSR_STABILITY_STORE_FLICKER, 7.0f / 255.0f },
	{ "flicker_state_1", RD::DATA_FORMAT_R8_UNORM, 1, TSR_FEATURE_FLICKER, TSR_STABILITY_STORE_FLICKER, 7.0f / 255.0f },
	{ "thin_coverage_0", RD::DATA_FORMAT_R8_UNORM, 1, TSR_FEATURE_THIN_GEOMETRY, TSR_STABILITY_STORE_THIN, 0.0f },
	{ "thin_coverage_1", RD::DATA_FORMAT_R8_UNORM, 1, TSR_FEATURE_THIN_GEOMETRY, TSR_STABILITY_STORE_THIN, 0.0f },
};

// GodotNext: pt_tsr_d3d12_metal (#195). Every RB_SCOPE_TSR texture is written as a storage
// image and read through a sampler, with this usage on every driver.
inline constexpr uint32_t TSR_TEXTURE_USAGE = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
// History colour (and the persistent frames, which follow it) is the one format with a
// fallback: a device that reports no storage use of R11G11B10 gets RGBA16F, whose 4 extra bytes
// per texel TSR::estimate_memory_bytes and TSR::stability_layout count against T6. TSR only
// writes its storage images (no typed loads). The other formats (R8, R16F, RG16F) have no
// fallback: D3D12 requires typed UAV stores for them from feature level 11, and desktop Vulkan
// and Metal GPUs write them as storage images too. On a device without one, RenderForwardClustered
// leaves TSR off (SCALE_NONE, plain 3D scaling) and warns once when TSR is requested.
inline constexpr RD::DataFormat TSR_HISTORY_COLOR_FORMAT = RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32;
inline constexpr RD::DataFormat TSR_HISTORY_COLOR_FALLBACK_FORMAT = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;

// The formats TSR::select_formats chose for this device.
struct TSRFormatSelection {
	bool history_rgba16f = false; // History colour and the persistent frames use the fallback.
	bool supported = true; // false: a format TSR needs has no usable storage format on this device.
	RD::DataFormat missing_format = RD::DATA_FORMAT_MAX; // The first format with neither itself nor a fallback.
	uint32_t formats_checked = 0; // Distinct formats queried, fallback included.

	// The format a TSR_FORMAT_TABLE or TSR_STABILITY_STORE_TABLE entry is allocated with.
	RD::DataFormat resolve(RD::DataFormat p_table_format) const {
		return (history_rgba16f && p_table_format == TSR_HISTORY_COLOR_FORMAT) ? TSR_HISTORY_COLOR_FALLBACK_FORMAT : p_table_format;
	}
};

// Sizes of the stability stores for one view, from TSR::stability_layout.
struct TSRStabilityLayout {
	uint32_t features = 0;
	uint32_t persistent_count = 0;
	Size2i persistent_size;
	Size2i flicker_size;
	Size2i thin_size;
	uint32_t persistent_fit_q16 = TSR_FIT_FULL_Q16; // 65536 = full resolution.
	uint32_t state_fit_q16 = TSR_FIT_FULL_Q16;
	bool history_rgba16f = false;
	uint64_t core_bytes = 0; // TSR::estimate_memory_bytes at the current internal size, 1 view.
	uint64_t stability_bytes = 0; // The stability stores, 1 view.
	uint64_t budget_bytes = 0;
	bool over_budget = false;

	uint64_t total_bytes() const { return core_bytes + stability_bytes; }
	// Whether a store is allocated, its size, its layers per view and its bytes per pixel.
	bool has_store(const TSRStabilityStoreEntry &p_entry) const;
	Size2i store_size(TSRStabilityStore p_store) const;
	uint32_t store_layers(TSRStabilityStore p_store) const;
	uint32_t store_bytes_per_pixel(const TSRStabilityStoreEntry &p_entry) const;
	// Changes whenever the persistent frames are reallocated or switched off,
	// which invalidates their contents. 0 with no persistent frames.
	uint64_t persistent_signature() const;
};

class TSRContext {
public:
	// GodotNext: pt_tsr_stability (#198). The camera a persistent frame was captured with.
	struct PersistentFrame {
		bool valid = false;
		Projection projection; // Depth-corrected and unjittered, as TSR::Parameters::camera_projection.
		Transform3D camera_transform;
	};

private:
	static SafeNumeric<uint32_t> live_context_count;

	Size2i internal_size;
	Size2i output_size;
	Size2i history_size;
	RSE::ViewportTSRQuality quality = RSE::VIEWPORT_TSR_QUALITY_EPIC;
	uint32_t parity = 0;
	TemporalHistoryReset reset_latch;

	PersistentFrame persistent[TSR_PERSISTENT_MAX_FRAMES];
	uint32_t frames_since_reset = 0;
	RID persistent_texture; // The persistent_frames texture the slots refer to.
	uint64_t persistent_signature = 0;
	RID stability_ubo;
	// Flicker analysis clock: the parts of a step carried to the next frame.
	double flicker_tick_remainder = 0.0;
	double flicker_decay_remainder = 0.0;

public:
	TSRContext(const Size2i &p_internal_size, const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality);
	~TSRContext();

	// Updates context dimensions. Requests a one-shot history reset only when output
	// or history resolution changes; internal-only resolution changes preserve history.
	bool sync_dimensions(const Size2i &p_internal_size, const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality);

	void request_reset() { reset_latch.request(); }
	bool consume_reset() { return reset_latch.consume(); }

	Size2i get_internal_size() const { return internal_size; }
	Size2i get_output_size() const { return output_size; }
	Size2i get_history_size() const { return history_size; }
	RSE::ViewportTSRQuality get_quality() const { return quality; }

	uint32_t get_read_index() const { return parity & 1u; }
	uint32_t get_write_index() const { return (parity ^ 1u) & 1u; }
	void advance_frame() {
		parity ^= 1u;
		frames_since_reset++;
	}

	// Persistent frames (history resurrection). Every history reset or resize
	// invalidates them and restarts the capture schedule.
	void invalidate_persistent();
	uint32_t get_frames_since_reset() const { return frames_since_reset; }
	const PersistentFrame &get_persistent_frame(uint32_t p_slot) const;
	void set_persistent_frame(uint32_t p_slot, const Projection &p_projection, const Transform3D &p_camera_transform);
	// Invalidates when the persistent_frames texture or its layout changed since the last call.
	void sync_persistent_storage(RID p_texture, uint64_t p_signature);

	RID get_stability_ubo() const { return stability_ubo; }
	void set_stability_ubo(RID p_buffer) { stability_ubo = p_buffer; }

	// Flicker analysis: this frame's ticks and decay steps, from the frame time with
	// this context's remainders. Call once per frame.
	void step_flicker_clock(double p_delta, uint32_t &r_ticks, uint32_t &r_decay_steps) {
		r_ticks = nextgi_tsr_flicker_ticks(flicker_tick_remainder, p_delta);
		r_decay_steps = nextgi_tsr_flicker_decay_steps(flicker_decay_remainder, p_delta);
	}

	static uint32_t get_live_context_count() { return live_context_count.get(); }
};

class TSR {
public:
	enum HistoryMode : uint32_t {
		HISTORY_MODE_R11G11B10F = 0,
		HISTORY_MODE_RGBA16F = 1,
		HISTORY_MODE_MAX = 2,
	};

	// GodotNext: pt_tsr_stability (#198). Variants of tsr_stability_analysis.glsl.
	enum AnalysisMode : uint32_t {
		ANALYSIS_MODE_FLICKER = 0,
		ANALYSIS_MODE_THIN = 1,
		ANALYSIS_MODE_MAX = 2,
	};

	enum ResolveMode : uint32_t {
		RESOLVE_MODE_R11G11B10F = 0,
		RESOLVE_MODE_RGBA16F = 1,
		RESOLVE_MODE_RGB10A2 = 2,
		RESOLVE_MODE_MAX = 3,
	};

	enum PassFlags : uint32_t {
		TSR_FLAG_RESET = 1u << 0,
		TSR_FLAG_ORTHOGONAL = 1u << 1,
		TSR_FLAG_REJECTION_AA = 1u << 2,
	};

	struct PushConstant {
		float reprojection[16];
		int32_t internal_size[2];
		int32_t history_size[2];
		int32_t output_size[2];
		float jitter_pixels[2];
		float z_near;
		float z_far;
		uint32_t flags;
		float history_scale;
	};
	static_assert(sizeof(PushConstant) == 112);

	// GodotNext: pt_tsr_stability (#198). std140 block of tsr_stability_inc.glsl,
	// bound only by TSR_STABILITY shader variants; the push constant is left as it is.
	struct StabilityUBO {
		float persistent_reprojection[TSR_PERSISTENT_MAX_FRAMES][16]; // Current clip -> persistent frame clip.
		float persistent_valid[TSR_PERSISTENT_MAX_FRAMES]; // 1.0 when the slot holds a frame.
		uint32_t features; // TSR_FEATURE_* in effect.
		uint32_t persistent_count; // N.
		uint32_t persistent_layer_base; // view * N.
		uint32_t debug_view; // TSRDebugView drawn by the debug pass.
		int32_t flicker_size[2]; // 0 when off.
		int32_t thin_size[2]; // 0 when off.
		uint32_t flicker_ticks;
		uint32_t flicker_decay_steps;
		float flicker_max_motion; // Internal pixels per frame.
		float persistent_confidence; // Confidence given to a resurrected sample.
	};
	static_assert(sizeof(StabilityUBO) == 320);

	struct Parameters {
		Ref<RenderSceneBuffersRD> render_buffers;
		TSRContext *context = nullptr;
		uint32_t view = 0;
		RID color;
		RID depth;
		RID velocity;
		RID reactive;
		RID output;
		Size2i internal_size;
		Size2i output_size;
		RSE::ViewportTSRQuality quality = RSE::VIEWPORT_TSR_QUALITY_EPIC;
		Vector2 jitter_pixels;
		Projection reprojection;
		float z_near = 0.05f;
		float z_far = 4000.0f;
		bool is_orthogonal = false;
		bool reset = false;
		// GodotNext: pt_tsr_stability (#198).
		Size2i max_internal_size; // The largest dynamic-resolution bucket; (0, 0) means internal_size.
		Projection camera_projection; // Depth-corrected and unjittered: the projection `reprojection` is built from.
		Transform3D camera_transform;
		// This frame's wall-clock step in seconds, not scaled by Engine.time_scale: the flicker
		// analysis measures real time (RenderingServerGlobals::frame_step_unscaled).
		float delta_time = 0.0f;
		bool thin_geometry = false; // rendering/anti_aliasing/tsr/thin_geometry_detection.
		// The TSRDebugView to draw into RB_SCOPE_TSR_DEBUG after the resolve, or -1 when
		// Viewport.DEBUG_DRAW_TSR is off. It never changes the other passes.
		int32_t debug_view = -1;
	};

	TSR();
	~TSR();

	TSRContext *create_context(const Size2i &p_internal_size, const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality) const;
	// Returns true when it (re)created textures.
	bool ensure_textures(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_internal_size, const Size2i &p_history_size) const;
	// GodotNext: pt_tsr_stability (#198). Frees and creates the stability stores to
	// match the layout; with no feature it leaves RB_SCOPE_TSR as #194 made it.
	// Returns true when it freed or created textures.
	bool ensure_stability_textures(Ref<RenderSceneBuffersRD> p_render_buffers, const TSRStabilityLayout &p_layout) const;
	void process(const Parameters &p_params);

	bool is_history_rgba16f() const { return format_selection.history_rgba16f; }

	static uint32_t jitter_phase_count(const Size2i &p_target_size, const Size2i &p_render_size) {
		return nextgi_tsr_jitter_phase_count(p_target_size, p_render_size);
	}
	static constexpr float mip_bias_offset() {
		return nextgi_tsr_mip_bias_offset();
	}
	static Size2i compute_history_size(const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality);
	static uint32_t history_bytes_per_pixel();
	static uint32_t internal_bytes_per_pixel();
	static uint64_t estimate_memory_bytes(const Size2i &p_internal_size, const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality, uint32_t p_view_count = 1, bool p_history_rgba16f = false);
	// GodotNext: pt_tsr_stability (#198). T6 accounting: sizes the stability stores
	// of p_features so that every RB_SCOPE_TSR texture together stays within
	// p_budget_x100 / 100 bytes per history pixel wherever the #194 textures leave
	// room, shrinking the stores (never below TSR_FIT_MIN_Q16 per axis) where they
	// leave less than the full layout needs. Pure arithmetic, for one view.
	static TSRStabilityLayout stability_layout(const Size2i &p_internal_size, const Size2i &p_max_internal_size, const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality, uint32_t p_features, bool p_history_rgba16f = false, uint32_t p_budget_x100 = TSR_STABILITY_BUDGET_X100);

	// GodotNext: pt_tsr_d3d12_metal (#195). Picks TSR's RB_SCOPE_TSR storage formats from a
	// capability query, bool p_supported(RD::DataFormat, uint32_t usage), asked once per distinct
	// format of TSR_FORMAT_TABLE and TSR_STABILITY_STORE_TABLE with TSR_TEXTURE_USAGE, and for the
	// history fallback when R11G11B10 fails. Pure: the doctests pass a mock capability table.
	template <typename Query>
	static TSRFormatSelection select_formats(const Query &p_supported) {
		TSRFormatSelection selection;
		constexpr uint32_t max_asked = uint32_t(std_size(TSR_FORMAT_TABLE) + std_size(TSR_STABILITY_STORE_TABLE)) + 1;
		RD::DataFormat asked[max_asked] = {};
		bool answers[max_asked] = {};
		auto ask = [&](RD::DataFormat p_format) -> bool {
			for (uint32_t i = 0; i < selection.formats_checked; i++) {
				if (asked[i] == p_format) {
					return answers[i];
				}
			}
			const bool answer = p_supported(p_format, TSR_TEXTURE_USAGE);
			asked[selection.formats_checked] = p_format;
			answers[selection.formats_checked] = answer;
			selection.formats_checked++;
			return answer;
		};
		auto take = [&](RD::DataFormat p_format) {
			bool usable = ask(p_format);
			if (!usable && p_format == TSR_HISTORY_COLOR_FORMAT && ask(TSR_HISTORY_COLOR_FALLBACK_FORMAT)) {
				selection.history_rgba16f = true;
				usable = true;
			}
			if (!usable && selection.supported) {
				selection.supported = false;
				selection.missing_format = p_format;
			}
		};
		for (const TSRTextureFormatEntry &entry : TSR_FORMAT_TABLE) {
			take(entry.format);
		}
		for (const TSRStabilityStoreEntry &entry : TSR_STABILITY_STORE_TABLE) {
			take(entry.format);
		}
		return selection;
	}
	// select_formats with the formats a GODOTNEXT_TSR_DENY_STORAGE_FORMATS list names treated
	// as unusable.
	template <typename Query>
	static TSRFormatSelection select_formats_for(const String &p_deny_list, const Query &p_supported) {
		const Vector<RD::DataFormat> denied = parse_denied_formats(p_deny_list);
		return select_formats([&](RD::DataFormat p_format, uint32_t p_usage) -> bool {
			return !denied.has(p_format) && p_supported(p_format, p_usage);
		});
	}
	// select_formats_for reading GODOTNEXT_TSR_DENY_STORAGE_FORMATS from the environment, shared
	// between select_device_formats (TSR::TSR) and the doctests.
	template <typename Query>
	static TSRFormatSelection select_env_formats(const Query &p_supported) {
		return select_formats_for(get_deny_list(), p_supported);
	}
	// The selection on the current RenderingDevice, with the environment's deny list, stored by
	// TSR's constructor (get_format_selection()). Without a RenderingDevice it reports TSR as
	// unsupported, with no missing format.
	static TSRFormatSelection select_device_formats();
	// GODOTNEXT_TSR_DENY_STORAGE_FORMATS: a comma-separated list of RB_SCOPE_TSR formats, by their
	// RenderingDevice.DataFormat names with or without the DATA_FORMAT_ prefix, in any case
	// (for example "B10G11R11_UFLOAT_PACK32"), that TSR treats as unusable for storage, so the
	// fallback can run on any GPU. Names of other formats, and unknown names, are ignored.
	static Vector<RD::DataFormat> parse_denied_formats(const String &p_list);
	// The environment variable's value ("" without an OS singleton).
	static String get_deny_list();
	// The DataFormat name of one of RB_SCOPE_TSR's formats ("DATA_FORMAT_R8_UNORM"), else "".
	static const char *format_name(RD::DataFormat p_format);
	const TSRFormatSelection &get_format_selection() const { return format_selection; }
	void warn_unsupported_once() const;

private:
	struct {
		TsrVelocityDilateShaderRD velocity_dilate;
		RID velocity_dilate_version;
		RID velocity_dilate_pipeline;

		TsrDisocclusionShaderRD disocclusion;
		RID disocclusion_version;
		RID disocclusion_pipeline;

		// GodotNext: pt_tsr_stability (#198). Variant 0 is the #194 shader; variant 1
		// (TSR_STABILITY) adds the stability relaxation.
		TsrShadingRejectionShaderRD shading_rejection;
		RID shading_rejection_version;
		RID shading_rejection_pipelines[2];

		TsrStabilityAnalysisShaderRD stability_analysis;
		RID stability_analysis_version;
		RID stability_analysis_pipelines[ANALYSIS_MODE_MAX];

		// GodotNext: pt_tsr_stability (#198). Variants [0, HISTORY_MODE_MAX) are
		// the #194 shader; the TSR_STABILITY variants follow in the same order.
		TsrHistoryUpdateShaderRD history_update;
		RID history_update_version;
		RID history_update_pipelines[2 * HISTORY_MODE_MAX];

		TsrPersistentStoreShaderRD persistent_store;
		RID persistent_store_version;
		RID persistent_store_pipelines[HISTORY_MODE_MAX];

		TsrRejectionAaShaderRD rejection_aa;
		RID rejection_aa_version;
		RID rejection_aa_pipelines[HISTORY_MODE_MAX];

		TsrResolveShaderRD resolve;
		RID resolve_version;
		RID resolve_pipelines[RESOLVE_MODE_MAX];

		// GodotNext: pt_tsr_stability (#198). The pass visualizer.
		TsrDebugViewShaderRD debug_view;
		RID debug_view_version;
		RID debug_view_pipeline;
	} shaders;

	RID nearest_sampler;
	RID linear_sampler;
	// GodotNext: pt_tsr_d3d12_metal (#195). From select_device_formats, once; it replaces
	// #194's history_use_rgba16f (format_selection.history_rgba16f).
	TSRFormatSelection format_selection;
};

} // namespace RendererRD
