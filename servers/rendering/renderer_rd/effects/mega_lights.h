/**************************************************************************/
/*  mega_lights.h                                                         */
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

// GodotNext: megalights_sampling (#142, plan §21.3 contract M3, GM-1, GM-2, budget M-3).
// MegaLights sampling core: per-pixel weighted reservoir sampling over the #139 packed froxel
// light grid, full/half downsampled 2x2 rotating sampling, 8x8 tile visible-light guiding with
// unbiased minimum weight floor, falloff early cull, 64x64x32 spatio-temporal blue noise, and
// exhaustive reference mode with trace_shadow() accumulation.
//
// Everything in MegaLightsMath / MegaLightsViewData interfaces is clean and device-free where
// applicable so CPU doctests run without a GPU, and GPU buffers expose clean contracts ready
// for #146 (visibility chain), #135 (denoiser), and #143 (Forward+ color composite).

#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/rendering_device.h"

#include <cstdint>

class RenderDataRD;
class RenderSceneBuffersRD;

namespace RendererRD {

class LightFunctionAtlas;
class NextGI;

// Per-view GPU resources owned by RenderSceneBuffersRD under RB_SCOPE_MEGALIGHTS.
class MegaLightsViewData : public RenderBufferCustomDataRD {
	GDCLASS(MegaLightsViewData, RenderBufferCustomDataRD);

	Size2i internal_size;
	Size2i sample_grid_size;
	Size2i guiding_tile_dims;
	uint32_t samples_per_pixel = 0;
	uint32_t downsample_mode = 1; // 0 = full, 1 = half

	RID sample_buffer;
	RID visibility_buffer;
	RID guiding_buffers[2];
	uint32_t guiding_write_index = 0;
	bool guiding_has_history = false;

	RID reference_accum_texture;
	Size2i reference_accum_size;
	uint32_t reference_frame_count = 0;
	uint64_t reference_camera_hash = 0;

	RID verify_accum_texture;
	RID verify_counts_buffer;
	Size2i verify_size;

	RID params_ubo;
	RID layer_group_buffer;
	uint32_t layer_group_capacity = 0;
	RID light_shadow_policy_buffer;
	uint32_t light_shadow_policy_capacity = 0;

	uint32_t frame_index = 0;

	void _free_sampling_buffers();
	void _free_reference_buffers();
	void _free_verify_buffers();
	void _free_transient_buffers();

public:
	virtual void configure(RenderSceneBuffersRD *p_render_buffers) override;
	virtual void free_data() override;

	bool ensure_sampling_buffers(const Size2i &p_internal_size, uint32_t p_samples_per_pixel, uint32_t p_downsample_mode);
	bool ensure_reference_buffers(const Size2i &p_internal_size);
	bool ensure_verify_buffers(const Size2i &p_internal_size);
	bool ensure_layer_group_buffer(uint32_t p_count);
	bool ensure_light_shadow_policy_buffer(uint32_t p_count);

	void reset_guiding();
	void reset_reference();

	Size2i get_internal_size() const { return internal_size; }
	Size2i get_sample_grid_size() const { return sample_grid_size; }
	Size2i get_guiding_tile_dims() const { return guiding_tile_dims; }
	uint32_t get_samples_per_pixel() const { return samples_per_pixel; }
	uint32_t get_downsample_mode() const { return downsample_mode; }

	// Clean buffer contracts for #146 (visibility chain), #135 (denoiser), and #143 (composite).
	RID get_sample_buffer() const { return sample_buffer; }
	RID get_visibility_buffer() const { return visibility_buffer; }
	RID get_current_guiding_buffer() const { return guiding_buffers[guiding_write_index]; }
	RID get_previous_guiding_buffer() const { return guiding_buffers[1u - guiding_write_index]; }
	bool has_guiding_history() const { return guiding_has_history; }
	void advance_guiding_history();

	RID get_reference_accum_texture() const { return reference_accum_texture; }
	uint32_t get_reference_frame_count() const { return reference_frame_count; }
	uint64_t get_reference_camera_hash() const { return reference_camera_hash; }
	void set_reference_state(uint32_t p_frame_count, uint64_t p_camera_hash) {
		reference_frame_count = p_frame_count;
		reference_camera_hash = p_camera_hash;
	}

	RID get_verify_accum_texture() const { return verify_accum_texture; }
	RID get_verify_counts_buffer() const { return verify_counts_buffer; }

	RID get_params_ubo() const { return params_ubo; }
	RID get_layer_group_buffer() const { return layer_group_buffer; }
	RID get_light_shadow_policy_buffer() const { return light_shadow_policy_buffer; }

	uint32_t get_frame_index() const { return frame_index; }
	void set_frame_index(uint32_t p_frame) { frame_index = p_frame; }
	void advance_frame_index() { frame_index++; }

	MegaLightsViewData() = default;
	virtual ~MegaLightsViewData();
};

class MegaLights {
public:
	// Project settings keys (registered in RenderingServer, documented in ProjectSettings.xml).
	static constexpr const char *SETTING_SAMPLES_PER_PIXEL = "rendering/megalights/samples_per_pixel";
	static constexpr const char *SETTING_DOWNSAMPLE = "rendering/megalights/downsample";
	static constexpr const char *SETTING_DIRECTIONAL_LIGHTS = "rendering/megalights/directional_lights";
	static constexpr const char *SETTING_FALLOFF_CULL_THRESHOLD = "rendering/megalights/falloff_cull_threshold";
	static constexpr const char *SETTING_REFERENCE_MODE = "rendering/megalights/reference_mode";

	enum DownsampleMode : uint32_t {
		DOWNSAMPLE_FULL = 0,
		DOWNSAMPLE_HALF = 1,
	};

	enum LightType : uint32_t {
		LIGHT_TYPE_OMNI = 0,
		LIGHT_TYPE_SPOT = 1,
		LIGHT_TYPE_AREA = 2,
		LIGHT_TYPE_DIRECTIONAL = 3,
	};

	static constexpr uint32_t DEFAULT_SAMPLES_PER_PIXEL = 4u;
	static constexpr uint32_t MIN_SAMPLES_PER_PIXEL = 1u;
	static constexpr uint32_t MAX_SAMPLES_PER_PIXEL = 16u;
	static constexpr DownsampleMode DEFAULT_DOWNSAMPLE = DOWNSAMPLE_HALF;
	static constexpr bool DEFAULT_DIRECTIONAL_LIGHTS = false;
	static constexpr float DEFAULT_FALLOFF_CULL_THRESHOLD = 0.001f;
	static constexpr bool DEFAULT_REFERENCE_MODE = false;

	static constexpr uint32_t SAMPLE_INDEX_MASK = 0xFFFFu;
	static constexpr uint32_t SAMPLE_TYPE_SHIFT = 16u;
	static constexpr uint32_t SAMPLE_TYPE_MASK = 0x3u;
	static constexpr uint32_t SAMPLE_INVALID = 0xFFFFFFFFu;

	static constexpr uint32_t GUIDING_TILE_SIZE = 8u;
	static constexpr uint32_t GUIDING_MAX_LIGHTS = 32u;
	static constexpr uint32_t GUIDING_HEADER_WORDS = 4u;
	static constexpr uint32_t GUIDING_TILE_WORDS = 36u;
	static constexpr uint32_t GUIDING_LIGHT_MASK = 0x3FFFFu;
	static constexpr uint32_t GUIDING_COUNT_SHIFT = 18u;
	static constexpr uint32_t GUIDING_COUNT_MAX = 16383u;
	// Strictly positive minimum weight floor for occluded/unlisted lights so WRS remains unbiased.
	static constexpr float DEFAULT_GUIDING_FLOOR = 0.25f;

	static constexpr uint32_t STBN_WIDTH = 64u;
	static constexpr uint32_t STBN_HEIGHT = 64u;
	static constexpr uint32_t STBN_DEPTH = 32u;
	static constexpr uint32_t STBN_CHANNELS = 2u;

	enum ParamFlags : uint32_t {
		PARAM_FLAG_DIRECTIONAL_LIGHTS = 1u << 0u,
		PARAM_FLAG_ORTHOGONAL = 1u << 1u,
		PARAM_FLAG_HAS_GUIDING_HISTORY = 1u << 2u,
		PARAM_FLAG_USE_LIGHT_FUNCTION = 1u << 3u,
		PARAM_FLAG_FORCE_VISIBLE_ONE = 1u << 4u,
		PARAM_FLAG_USE_SOFT_SHADOWS = 1u << 5u,
	};

	enum GuidingFlags : uint32_t {
		GUIDING_FLAG_PHASE_MASK = 3u,
		GUIDING_FLAG_DOWNSAMPLE_HALF = 1u << 2u,
		GUIDING_FLAG_FORCE_VISIBLE_ONE = 1u << 3u,
	};

	struct Settings {
		uint32_t samples_per_pixel = DEFAULT_SAMPLES_PER_PIXEL;
		DownsampleMode downsample = DEFAULT_DOWNSAMPLE;
		bool directional_lights = DEFAULT_DIRECTIONAL_LIGHTS;
		float falloff_cull_threshold = DEFAULT_FALLOFF_CULL_THRESHOLD;
		bool reference_mode = DEFAULT_REFERENCE_MODE;
	};

	// Packed 16-byte sample matching MLSample (uvec4) in ml_generate_samples.glsl.
	struct SampleGPU {
		uint32_t light_packed = SAMPLE_INVALID;
		float inv_pdf = 0.0f;
		uint32_t ray_dir_oct = 0u;
		float max_distance = 0.0f;
	};
	static_assert(sizeof(SampleGPU) == 16, "MegaLights::SampleGPU must be 16 bytes (uvec4).");
	static_assert(sizeof(SampleGPU) % 16 == 0, "MegaLights::SampleGPU must stay 16-byte aligned.");

	// std140, 256 bytes. Must match MLSceneParams in ml_light_eval_inc.glsl.
	struct ParamsUBO {
		float inv_projection[16] = {};
		float inv_view_rows[12] = {};
		uint32_t screen_sample_size[4] = {};
		uint32_t grid_dims_flags[4] = {};
		float grid_z_params[4] = {};
		uint32_t light_counts[4] = {};
		uint32_t sample_config[4] = {};
		uint32_t guiding_tile_info[4] = {};
		float thresholds[4] = {};
		uint32_t pad_extra[4] = {};
		uint32_t pad_extra2[4] = {};
	};
	static_assert(sizeof(ParamsUBO) == 256, "MegaLights::ParamsUBO must match MLSceneParams (256 bytes).");
	static_assert(sizeof(ParamsUBO) % 16 == 0, "MegaLights::ParamsUBO must stay 16-byte aligned.");

	// Push constant for ml_guiding.glsl (32 bytes).
	struct GuidingPushConstant {
		uint32_t full_size[2] = {};
		uint32_t sample_size[2] = {};
		uint32_t tile_dims[2] = {};
		uint32_t samples_per_pixel = 0;
		uint32_t flags = 0;
	};
	static_assert(sizeof(GuidingPushConstant) == 32, "MegaLights::GuidingPushConstant must match ml_guiding.glsl.");

	// Inputs supplied per view by RenderForwardClustered (or #143/#146 orchestrator).
	struct FrameInputs {
		RenderDataRD *render_data = nullptr;
		RID depth_texture;
		RID normal_roughness_texture;
		RID ml_albedo_texture;
		RID ml_material_texture;
		RID dfg_lut_texture;
		RID ltc_lut1_texture;
		RID ltc_lut2_texture;
		LightFunctionAtlas *light_function_atlas = nullptr;
		NextGI *next_gi = nullptr;
		bool flip_y = true;
		bool use_soft_shadows = true;
	};

	// --- Pure, device-free helpers (tested by CPU doctests) ---

	static Settings read_settings();
	static Settings validate_settings(const Settings &p_settings);
	// Provisional sample count per quality preset (Low=2, Medium=2, High=4, Epic=8; tuned in PQ).
	static uint32_t samples_for_quality_preset(int p_quality_level);
	static DownsampleMode parse_downsample_variant(const Variant &p_value);

	static Vector2i downsample_offset(uint32_t p_frame_index);
	static Size2i compute_sample_grid_size(const Size2i &p_internal_size, DownsampleMode p_downsample);
	static Vector2i sample_to_full_pixel(const Vector2i &p_sample_px, const Size2i &p_full_size, DownsampleMode p_downsample, uint32_t p_frame_index);
	static Size2i compute_guiding_tile_dims(const Size2i &p_full_size);

	static uint32_t pack_sample_light(uint32_t p_type, uint32_t p_index);
	static uint32_t unpack_sample_light_type(uint32_t p_packed);
	static uint32_t unpack_sample_light_index(uint32_t p_packed);

	static uint32_t pack_guiding_entry(uint32_t p_light_packed, uint32_t p_count);
	static uint32_t unpack_guiding_light(uint32_t p_entry);
	static uint32_t unpack_guiding_count(uint32_t p_entry);

	static uint32_t pack_oct_snorm16(const Vector3 &p_dir);
	static Vector3 unpack_oct_snorm16(uint32_t p_word);

	static float omni_attenuation(float p_distance, float p_inv_range, float p_decay);
	static bool passes_falloff_cull(float p_attenuation, float p_threshold);
	static float guiding_weight(uint32_t p_visible_count, uint32_t p_max_tile_count, bool p_has_tile_history, float p_floor = DEFAULT_GUIDING_FLOOR);

	// CPU reference guiding aggregator for one 8x8 tile (matching ml_guiding.glsl top-32 sort).
	struct GuidingEntry {
		uint32_t light_packed = SAMPLE_INVALID;
		uint32_t count = 0;
	};
	static void aggregate_guiding_tile_cpu(const uint32_t *p_visible_lights, uint32_t p_count, LocalVector<GuidingEntry> &r_top32);

	// CPU reference 1-pass K-reservoir weighted sampler (matching ml_generate_samples.glsl).
	struct ReservoirSample {
		uint32_t light_index = SAMPLE_INVALID;
		float inv_pdf = 0.0f;
	};
	static void sample_reservoirs_cpu(const float *p_weights, uint32_t p_light_count, uint32_t p_k, uint64_t &r_rng_state, LocalVector<ReservoirSample> &r_out);

	// Welford relative standard error helper matching ml_reference.glsl.
	static float welford_relative_std_error(float p_mean_luma, float p_m2, uint32_t p_sample_count);

	// Spatio-temporal blue-noise table accessors (64x64x32 RG8 committed data).
	static const uint8_t *get_stbn_data();
	static uint32_t get_stbn_byte_size();

	// Per-view state accessors (owned by RenderSceneBuffersRD under RB_SCOPE_MEGALIGHTS).
	static Ref<MegaLightsViewData> get_view_data(RenderSceneBuffersRD *p_rb);
	static Ref<MegaLightsViewData> ensure_view_data(RenderSceneBuffersRD *p_rb);

	// GPU verification check hook (matching LightGrid / VSM check pattern).
	static void request_check(const Dictionary &p_options = Dictionary());
	static bool has_pending_check();
	static Dictionary get_check_result();

private:
	struct GPUState;
	GPUState *gpu = nullptr;

	bool _ensure_common_shaders();
	bool _ensure_rt_reference_shader(NextGI *p_next_gi, bool p_use_alpha_textures);
	void _ensure_stbn_texture();
	void _fill_params_ubo(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, uint32_t p_ref_frame_index, bool p_has_guiding, bool p_force_visible_one, float p_falloff_override, ParamsUBO &r_ubo) const;
	void _upload_layer_group_masks(const Ref<MegaLightsViewData> &p_view) const;
	void _upload_light_shadow_policies(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, bool p_include_directional) const;
	void _build_common_uniforms(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, Vector<RD::Uniform> &r_uniforms) const;
	void _dispatch_generate(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, bool p_has_guiding, float p_falloff_override);
	void _dispatch_guiding(const Ref<MegaLightsViewData> &p_view, RID p_visibility_buffer, uint32_t p_frame_index, bool p_force_visible_one);
	void _dispatch_reference_unshadowed(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, uint32_t p_ref_n, float p_falloff_override);
	void _dispatch_reference_rt(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, uint32_t p_ref_n, bool p_force_visible_one, float p_falloff_override);
	void _dispatch_verify_samples(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, uint32_t p_step_n, float p_falloff_override);
	Dictionary _run_check(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Dictionary &p_options);

public:
	// Stage 1 (M3): generate K weighted light samples per sampling pixel into view->get_sample_buffer().
	void generate_samples(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb);

	// Stage 2 (M3, called after #146 visibility): aggregate visible lights per 8x8 tile for next frame.
	void update_guiding(Ref<RenderSceneBuffersRD> p_rb, RID p_visibility_buffer = RID(), bool p_force_visible_one = false);

	// Reference mode (M3, GM-1): exhaustive per-light shading with trace_shadow() accumulated across frames.
	void render_reference(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb, bool p_force_visible_one = false);

	// Per-frame entry point invoked by RenderForwardClustered after prepass & packed light grid build.
	void render_sampling(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb);

	void free_resources();

	MegaLights();
	~MegaLights();
};

} // namespace RendererRD
