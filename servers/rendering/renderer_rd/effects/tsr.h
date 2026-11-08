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
#include "core/templates/safe_refcount.h"
#include "servers/rendering/renderer_rd/effects/temporal_upscaler.h"
#include "servers/rendering/renderer_rd/effects/tsr_presets.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_disocclusion.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_history_update.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_rejection_aa.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr/tsr_shading_rejection.glsl.gen.h"
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

class TSRContext {
	static SafeNumeric<uint32_t> live_context_count;

	Size2i internal_size;
	Size2i output_size;
	Size2i history_size;
	RSE::ViewportTSRQuality quality = RSE::VIEWPORT_TSR_QUALITY_EPIC;
	uint32_t parity = 0;
	TemporalHistoryReset reset_latch;

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
	void advance_frame() { parity ^= 1u; }

	static uint32_t get_live_context_count() { return live_context_count.get(); }
};

class TSR {
public:
	enum HistoryMode : uint32_t {
		HISTORY_MODE_R11G11B10F = 0,
		HISTORY_MODE_RGBA16F = 1,
		HISTORY_MODE_MAX = 2,
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
	};

	TSR();
	~TSR();

	TSRContext *create_context(const Size2i &p_internal_size, const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality) const;
	void ensure_textures(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_internal_size, const Size2i &p_history_size) const;
	void process(const Parameters &p_params);

	bool is_history_rgba16f() const { return history_use_rgba16f; }

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

private:
	struct {
		TsrVelocityDilateShaderRD velocity_dilate;
		RID velocity_dilate_version;
		RID velocity_dilate_pipeline;

		TsrDisocclusionShaderRD disocclusion;
		RID disocclusion_version;
		RID disocclusion_pipeline;

		TsrShadingRejectionShaderRD shading_rejection;
		RID shading_rejection_version;
		RID shading_rejection_pipeline;

		TsrHistoryUpdateShaderRD history_update;
		RID history_update_version;
		RID history_update_pipelines[HISTORY_MODE_MAX];

		TsrRejectionAaShaderRD rejection_aa;
		RID rejection_aa_version;
		RID rejection_aa_pipelines[HISTORY_MODE_MAX];

		TsrResolveShaderRD resolve;
		RID resolve_version;
		RID resolve_pipelines[RESOLVE_MODE_MAX];
	} shaders;

	RID nearest_sampler;
	RID linear_sampler;
	bool history_use_rgba16f = false;
};

} // namespace RendererRD
