/**************************************************************************/
/*  render_forward_clustered.cpp                                          */
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

#include "render_forward_clustered.h"

#include "core/config/project_settings.h"
#include "core/templates/hash_set.h"
#include "servers/rendering/renderer_rd/environment/fog.h"
#include "servers/rendering/renderer_rd/environment/next_gi_wrc.h"
#include "servers/rendering/renderer_rd/environment/nextgeo_cull.h"
// GodotNext: megalights_inputs (#136)
#include "servers/rendering/renderer_rd/forward_clustered/megalights_inputs.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_visibility.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/light_source_parity.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/particles_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
// GodotNext: vsm_page_marking (#219) / rt_shadows (#141).
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_shadows.h"
#include "servers/rendering/renderer_rd/environment/next_gi_swrt_trace.h"
#include "servers/rendering/renderer_rd/environment/next_gi_view.h"
// GodotNext: vsm_non_nanite (#218).
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/storage_rd/virtual_shadow_map_storage.h"
// GodotNext: vsm_projection (#222). #54's blue-noise tile seeds the SMRT rays.
#include "servers/rendering/renderer_rd/environment/nextgi_blue_noise.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/renderer_viewport.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_default.h"
#include "servers/rendering/storage/ltc_lut.gen.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_TEXTURE_STREAMING_ENABLED
#include "modules/texture_streaming/texture_streaming.h"
#endif

using namespace RendererSceneRenderImplementation;

#define PRELOAD_PIPELINES_ON_SURFACE_CACHE_CONSTRUCTION 1

#define FADE_ALPHA_PASS_THRESHOLD 0.999

namespace {

struct NextGeoBoolScope {
	bool &flag;
	bool prev;
	NextGeoBoolScope(bool &p_flag, bool p_value) :
			flag(p_flag), prev(p_flag) {
		flag = p_value;
	}
	~NextGeoBoolScope() {
		flag = prev;
	}
};

} // namespace

void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_specular() {
	ERR_FAIL_NULL(render_buffers);

	if (!render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_SPECULAR)) {
		bool msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
		render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_SPECULAR, get_specular_format(), get_specular_usage_bits(msaa, false, render_buffers->get_can_be_storage()));
		if (msaa) {
			render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_SPECULAR_MSAA, get_specular_format(), get_specular_usage_bits(false, msaa, render_buffers->get_can_be_storage()), render_buffers->get_texture_samples());
		}
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_normal_roughness_texture() {
	ERR_FAIL_NULL(render_buffers);

	if (!render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_NORMAL_ROUGHNESS)) {
		bool msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
		render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_NORMAL_ROUGHNESS, get_normal_roughness_format(), get_normal_roughness_usage_bits(msaa, false, render_buffers->get_can_be_storage()));
		if (msaa) {
			render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_NORMAL_ROUGHNESS_MSAA, get_normal_roughness_format(), get_normal_roughness_usage_bits(false, msaa, render_buffers->get_can_be_storage()), render_buffers->get_texture_samples());
		}
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_voxelgi() {
	ERR_FAIL_NULL(render_buffers);

	if (!render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_VOXEL_GI)) {
		bool msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
		render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_VOXEL_GI, get_voxelgi_format(), get_voxelgi_usage_bits(msaa, false, render_buffers->get_can_be_storage()));
		if (msaa) {
			render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_VOXEL_GI_MSAA, get_voxelgi_format(), get_voxelgi_usage_bits(false, msaa, render_buffers->get_can_be_storage()), render_buffers->get_texture_samples());
		}
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_fsr2(RendererRD::FSR2Effect *p_effect) {
	const Size2i max_render_size = render_buffers->get_max_internal_size();
	const Size2i display_size = render_buffers->get_target_size();
	if (fsr2_context != nullptr &&
			(int(fsr2_context->fsr_desc.maxRenderSize.width) != max_render_size.x ||
					int(fsr2_context->fsr_desc.maxRenderSize.height) != max_render_size.y ||
					int(fsr2_context->fsr_desc.displaySize.width) != display_size.x ||
					int(fsr2_context->fsr_desc.displaySize.height) != display_size.y)) {
		memdelete(fsr2_context);
		fsr2_context = nullptr;
	}
	if (fsr2_context == nullptr) {
		fsr2_context = p_effect->create_context(max_render_size, display_size);
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_tsr(RendererRD::TSR *p_effect, uint32_t p_view) {
	ERR_FAIL_NULL(render_buffers);
	ERR_FAIL_NULL(p_effect);
	ERR_FAIL_UNSIGNED_INDEX(p_view, RendererSceneRender::MAX_RENDER_VIEWS);
	if (tsr_contexts[p_view] == nullptr) {
		tsr_contexts[p_view] = p_effect->create_context(render_buffers->get_internal_size(), render_buffers->get_target_size(), render_buffers->get_tsr_quality());
	}
}

RendererRD::TSRContext *RenderForwardClustered::RenderBufferDataForwardClustered::ensure_tsr_context(uint32_t p_view, const Size2i &p_internal_size, const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality) {
	ERR_FAIL_UNSIGNED_INDEX_V(p_view, RendererSceneRender::MAX_RENDER_VIEWS, nullptr);
	if (tsr_contexts[p_view] == nullptr) {
		tsr_contexts[p_view] = memnew(RendererRD::TSRContext(p_internal_size, p_output_size, p_quality));
	} else {
		tsr_contexts[p_view]->sync_dimensions(p_internal_size, p_output_size, p_quality);
	}
	return tsr_contexts[p_view];
}

void RenderForwardClustered::RenderBufferDataForwardClustered::free_tsr_contexts() {
	for (uint32_t v = 0; v < RendererSceneRender::MAX_RENDER_VIEWS; v++) {
		if (tsr_contexts[v] != nullptr) {
			memdelete(tsr_contexts[v]);
			tsr_contexts[v] = nullptr;
		}
	}
}

#ifdef METAL_MFXTEMPORAL_ENABLED
bool RenderForwardClustered::RenderBufferDataForwardClustered::ensure_mfx_temporal(RendererRD::MFXTemporalEffect *p_effect) {
	const Size2i max_in = render_buffers->get_max_internal_size();
	const Size2i cur_in = render_buffers->get_internal_size();
	const bool dynres = render_buffers->is_dynamic_resolution_configured() || (cur_in != max_in);
	const Size2i min_in = dynres ? render_buffers->get_min_internal_size() : max_in;
	const Size2i out_size = render_buffers->get_target_size();

	if (mfx_temporal_context != nullptr &&
			(mfx_temporal_context->max_input_size != max_in ||
					mfx_temporal_context->output_size != out_size ||
					(!mfx_temporal_context->dynamic_resolution && dynres) ||
					(dynres && (cur_in.x < mfx_temporal_context->min_input_size.x || cur_in.y < mfx_temporal_context->min_input_size.y)))) {
		memdelete(mfx_temporal_context);
		mfx_temporal_context = nullptr;
	}
	if (mfx_temporal_context == nullptr) {
		RendererRD::MFXTemporalEffect::CreateParams params;
		params.input_size = max_in;
		params.min_input_size = min_in;
		params.output_size = out_size;
		params.input_format = render_buffers->get_base_data_format();
		params.depth_format = render_buffers->get_depth_format(false, false, render_buffers->get_can_be_storage());
		params.motion_format = render_buffers->get_velocity_format();
		params.reactive_format = render_buffers->get_base_data_format(); // Reactive is derived from input.
		params.output_format = render_buffers->get_base_data_format();
		params.motion_vector_scale = cur_in;
		params.dynamic_resolution = dynres;
		mfx_temporal_context = p_effect->create_context(params);
		return true;
	}
	return false;
}
#endif

void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_df_shadow_targets(const Size2i &p_full_size, uint32_t p_layers) {
	// GodotNext: df_shadows (#214, plan §21.3 contract S7).
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);
	const Size2i half_size(MAX((p_full_size.x + 1) / 2, 1), MAX((p_full_size.y + 1) / 2, 1));
	const uint32_t layers = CLAMP(p_layers, 1u, RendererVirtualShadowMapStorage::DF_MAX_LIGHTS_PER_VIEW);
	if (df_shadow_mask.is_valid() && df_shadow_guide.is_valid() && df_shadow_half_size == half_size && df_shadow_layers == layers) {
		return;
	}
	free_df_shadow_targets();
	{
		RD::TextureFormat fmt;
		fmt.format = RD::DATA_FORMAT_R8_UNORM;
		fmt.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		fmt.width = uint32_t(half_size.x);
		fmt.height = uint32_t(half_size.y);
		fmt.array_layers = layers;
		fmt.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		df_shadow_mask = rd->texture_create(fmt, RD::TextureView());
		rd->set_resource_name(df_shadow_mask, "DF Shadow Mask Array");
	}
	{
		RD::TextureFormat fmt;
		fmt.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		fmt.texture_type = RD::TEXTURE_TYPE_2D;
		fmt.width = uint32_t(half_size.x);
		fmt.height = uint32_t(half_size.y);
		fmt.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		df_shadow_guide = rd->texture_create(fmt, RD::TextureView());
		rd->set_resource_name(df_shadow_guide, "DF Shadow Guide");
	}
	df_shadow_half_size = half_size;
	df_shadow_layers = layers;
}

void RenderForwardClustered::RenderBufferDataForwardClustered::free_df_shadow_targets() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr) {
		if (df_shadow_mask.is_valid()) {
			rd->free_rid(df_shadow_mask);
		}
		if (df_shadow_guide.is_valid()) {
			rd->free_rid(df_shadow_guide);
		}
	}
	df_shadow_mask = RID();
	df_shadow_guide = RID();
	df_shadow_half_size = Size2i();
	df_shadow_layers = 0;
}

void RenderForwardClustered::RenderBufferDataForwardClustered::free_data() {
	free_df_shadow_targets();
	// JIC, should already have been cleared
	if (render_buffers) {
		render_buffers->clear_context(RB_SCOPE_FORWARD_CLUSTERED);
		render_buffers->clear_context(RB_SCOPE_SSDS);
		render_buffers->clear_context(RB_SCOPE_SSIL);
		render_buffers->clear_context(RB_SCOPE_SSAO);
		render_buffers->clear_context(RB_SCOPE_SSR);
		render_buffers->clear_context(RB_SCOPE_SSCS);
		render_buffers->clear_context(RB_SCOPE_TSR);
		// GodotNext: pt_tsr_stability (#198). The TSR pass visualizer's texture.
		render_buffers->clear_context(RB_SCOPE_TSR_DEBUG);
	}

	if (cluster_builder) {
		memdelete(cluster_builder);
		cluster_builder = nullptr;
	}

	if (render_buffers != nullptr) {
		TemporalUpscalerRegistry::free_contexts(render_buffers);
	}

	if (fsr2_context) {
		memdelete(fsr2_context);
		fsr2_context = nullptr;
	}

	free_tsr_contexts();

#ifdef METAL_MFXTEMPORAL_ENABLED
	if (mfx_temporal_context) {
		memdelete(mfx_temporal_context);
		mfx_temporal_context = nullptr;
	}
#endif

	if (!render_sdfgi_uniform_set.is_null() && RD::get_singleton() != nullptr && RD::get_singleton()->uniform_set_is_valid(render_sdfgi_uniform_set)) {
		RD::get_singleton()->free_rid(render_sdfgi_uniform_set);
	}
	render_sdfgi_uniform_set = RID();
}

void RenderForwardClustered::RenderBufferDataForwardClustered::configure(RenderSceneBuffersRD *p_render_buffers) {
	if (render_buffers) {
		// JIC
		free_data();
	}

	render_buffers = p_render_buffers;
	ERR_FAIL_NULL(render_buffers);

	if (RenderForwardClustered::get_singleton() != nullptr && RD::get_singleton() != nullptr) {
		if (cluster_builder == nullptr) {
			cluster_builder = memnew(ClusterBuilderRD);
		}
		cluster_builder->set_shared(RenderForwardClustered::get_singleton()->get_cluster_builder_shared());

		RID sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		// Note: Color and depth buffer is only used for our debug mode, we don't support stereo rendering for this.
		cluster_builder->setup(p_render_buffers->get_internal_size(), p_render_buffers->get_max_cluster_elements(), p_render_buffers->get_depth_texture(0), sampler, p_render_buffers->get_internal_texture(0));
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::configure_internal_size(RenderSceneBuffersRD *p_render_buffers) {
	render_buffers = p_render_buffers;
	ERR_FAIL_NULL(render_buffers);

	free_df_shadow_targets();
	ss_effects_data.ssil = {};
	ss_effects_data.ssao = {};
	ss_effects_data.ssr = {};
	ss_effects_data.sscs = {};

	if (!render_sdfgi_uniform_set.is_null() && RD::get_singleton() != nullptr && RD::get_singleton()->uniform_set_is_valid(render_sdfgi_uniform_set)) {
		RD::get_singleton()->free_rid(render_sdfgi_uniform_set);
	}
	render_sdfgi_uniform_set = RID();

	if (RenderForwardClustered::get_singleton() != nullptr && RD::get_singleton() != nullptr) {
		if (cluster_builder == nullptr) {
			cluster_builder = memnew(ClusterBuilderRD);
			cluster_builder->set_shared(RenderForwardClustered::get_singleton()->get_cluster_builder_shared());
		}

		RID sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		cluster_builder->setup(p_render_buffers->get_internal_size(), p_render_buffers->get_max_cluster_elements(), p_render_buffers->get_depth_texture(0), sampler, p_render_buffers->get_internal_texture(0));
	}
}

RID RenderForwardClustered::RenderBufferDataForwardClustered::get_color_only_fb() {
	ERR_FAIL_NULL_V(render_buffers, RID());

	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;

	RID color = use_msaa ? render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_COLOR_MSAA) : render_buffers->get_internal_texture();
	RID depth = use_msaa ? render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_DEPTH_MSAA) : render_buffers->get_depth_texture();

	if (render_buffers->has_texture(RB_SCOPE_VRS, RB_TEXTURE)) {
		RID vrs_texture = render_buffers->get_texture(RB_SCOPE_VRS, RB_TEXTURE);
		return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), color, depth, vrs_texture);
	} else {
		return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), color, depth);
	}
}

RID RenderForwardClustered::RenderBufferDataForwardClustered::get_color_pass_fb(uint32_t p_color_pass_flags) {
	ERR_FAIL_NULL_V(render_buffers, RID());
	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;

	int v_count = (p_color_pass_flags & COLOR_PASS_FLAG_MULTIVIEW) ? render_buffers->get_view_count() : 1;
	RID color = use_msaa ? render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_COLOR_MSAA) : render_buffers->get_internal_texture();

	RID specular;
	if (p_color_pass_flags & COLOR_PASS_FLAG_SEPARATE_SPECULAR) {
		ensure_specular();
		specular = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, use_msaa ? RB_TEX_SPECULAR_MSAA : RB_TEX_SPECULAR);
	}

	RID velocity_buffer;
	if (p_color_pass_flags & COLOR_PASS_FLAG_MOTION_VECTORS) {
		render_buffers->ensure_velocity();
		velocity_buffer = render_buffers->get_velocity_buffer(use_msaa);
	}

	RID depth = use_msaa ? render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_DEPTH_MSAA) : render_buffers->get_depth_texture();

	// GodotNext: rr_guides (#205). The two albedo guides follow velocity, at locations 3 and 4.
	if (p_color_pass_flags & COLOR_PASS_FLAG_RR_GUIDES) {
		ERR_FAIL_COND_V_MSG(use_msaa, RID(), "The DLSS-RR guides are not allocated when MSAA is enabled.");
		render_buffers->ensure_rr_guides();
		RID diffuse_albedo = render_buffers->get_texture(RB_SCOPE_RR_GUIDES, RB_TEX_RR_DIFFUSE_ALBEDO);
		RID specular_albedo = render_buffers->get_texture(RB_SCOPE_RR_GUIDES, RB_TEX_RR_SPECULAR_ALBEDO);
		if (render_buffers->has_texture(RB_SCOPE_VRS, RB_TEXTURE)) {
			RID vrs_texture = render_buffers->get_texture(RB_SCOPE_VRS, RB_TEXTURE);
			return FramebufferCacheRD::get_singleton()->get_cache_multiview(v_count, color, specular, velocity_buffer, diffuse_albedo, specular_albedo, depth, vrs_texture);
		} else {
			return FramebufferCacheRD::get_singleton()->get_cache_multiview(v_count, color, specular, velocity_buffer, diffuse_albedo, specular_albedo, depth);
		}
	}

	if (render_buffers->has_texture(RB_SCOPE_VRS, RB_TEXTURE)) {
		RID vrs_texture = render_buffers->get_texture(RB_SCOPE_VRS, RB_TEXTURE);
		return FramebufferCacheRD::get_singleton()->get_cache_multiview(v_count, color, specular, velocity_buffer, depth, vrs_texture);
	} else {
		return FramebufferCacheRD::get_singleton()->get_cache_multiview(v_count, color, specular, velocity_buffer, depth);
	}
}

RID RenderForwardClustered::RenderBufferDataForwardClustered::get_depth_fb(DepthFrameBufferType p_type) {
	ERR_FAIL_NULL_V(render_buffers, RID());
	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;

	RID depth = use_msaa ? render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_DEPTH_MSAA) : render_buffers->get_depth_texture();

	switch (p_type) {
		case DEPTH_FB: {
			return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), depth);
		} break;
		case DEPTH_FB_ROUGHNESS: {
			ensure_normal_roughness_texture();

			RID normal_roughness_buffer = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, use_msaa ? RB_TEX_NORMAL_ROUGHNESS_MSAA : RB_TEX_NORMAL_ROUGHNESS);

			return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), depth, normal_roughness_buffer);
		} break;
		case DEPTH_FB_ROUGHNESS_VOXELGI: {
			ensure_normal_roughness_texture();
			ensure_voxelgi();

			RID normal_roughness_buffer = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, use_msaa ? RB_TEX_NORMAL_ROUGHNESS_MSAA : RB_TEX_NORMAL_ROUGHNESS);
			RID voxelgi_buffer = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, use_msaa ? RB_TEX_VOXEL_GI_MSAA : RB_TEX_VOXEL_GI);

			return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), depth, normal_roughness_buffer, voxelgi_buffer);
		} break;
		case DEPTH_FB_ROUGHNESS_VELOCITY: {
			ensure_normal_roughness_texture();
			render_buffers->ensure_velocity();
			RID normal_roughness_buffer = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, use_msaa ? RB_TEX_NORMAL_ROUGHNESS_MSAA : RB_TEX_NORMAL_ROUGHNESS);
			RID velocity_buffer = render_buffers->get_velocity_buffer(use_msaa);
			return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), depth, normal_roughness_buffer, velocity_buffer);
		} break;
		case DEPTH_FB_ROUGHNESS_VOXELGI_VELOCITY: {
			ensure_normal_roughness_texture();
			ensure_voxelgi();
			render_buffers->ensure_velocity();
			RID normal_roughness_buffer = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, use_msaa ? RB_TEX_NORMAL_ROUGHNESS_MSAA : RB_TEX_NORMAL_ROUGHNESS);
			RID voxelgi_buffer = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, use_msaa ? RB_TEX_VOXEL_GI_MSAA : RB_TEX_VOXEL_GI);
			RID velocity_buffer = render_buffers->get_velocity_buffer(use_msaa);
			return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), depth, normal_roughness_buffer, voxelgi_buffer, velocity_buffer);
		} break;
		// GodotNext: megalights_inputs (#136). Single-sample only. Order is normal, albedo, material, voxel, velocity.
		case DEPTH_FB_ROUGHNESS_ML:
		case DEPTH_FB_ROUGHNESS_VOXELGI_ML:
		case DEPTH_FB_ROUGHNESS_VELOCITY_ML:
		case DEPTH_FB_ROUGHNESS_VOXELGI_VELOCITY_ML: {
			ERR_FAIL_COND_V_MSG(use_msaa, RID(), "MegaLights input targets are not allocated when MSAA is enabled.");
			ensure_normal_roughness_texture();
			render_buffers->ensure_megalights_inputs();
			RID normal_roughness_buffer = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_NORMAL_ROUGHNESS);
			RID ml_albedo = render_buffers->get_ml_albedo();
			RID ml_material = render_buffers->get_ml_material();
			const bool voxel = p_type == DEPTH_FB_ROUGHNESS_VOXELGI_ML || p_type == DEPTH_FB_ROUGHNESS_VOXELGI_VELOCITY_ML;
			const bool velocity = p_type == DEPTH_FB_ROUGHNESS_VELOCITY_ML || p_type == DEPTH_FB_ROUGHNESS_VOXELGI_VELOCITY_ML;
			if (voxel) {
				ensure_voxelgi();
			}
			if (velocity) {
				render_buffers->ensure_velocity();
			}
			RID voxelgi_buffer = voxel ? render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_VOXEL_GI) : RID();
			RID velocity_buffer = velocity ? render_buffers->get_velocity_buffer(false) : RID();
			FramebufferCacheRD *cache = FramebufferCacheRD::get_singleton();
			const uint32_t views = render_buffers->get_view_count();
			if (voxel && velocity) {
				return cache->get_cache_multiview(views, depth, normal_roughness_buffer, ml_albedo, ml_material, voxelgi_buffer, velocity_buffer);
			} else if (voxel) {
				return cache->get_cache_multiview(views, depth, normal_roughness_buffer, ml_albedo, ml_material, voxelgi_buffer);
			} else if (velocity) {
				return cache->get_cache_multiview(views, depth, normal_roughness_buffer, ml_albedo, ml_material, velocity_buffer);
			}
			return cache->get_cache_multiview(views, depth, normal_roughness_buffer, ml_albedo, ml_material);
		} break;
		default: {
			ERR_FAIL_V(RID());
		} break;
	}
}

// GodotNext: megalights_inputs (#136), nextgeo_vertex_offset (#164)
RID RenderForwardClustered::RenderBufferDataForwardClustered::get_nextgeo_stage_a_fb(bool p_voxel_gi, bool p_megalights_inputs, bool p_with_velocity) {
	ERR_FAIL_NULL_V(render_buffers, RID());
	ensure_normal_roughness_texture();
	render_buffers->ensure_nextgeo_targets();

	RID normal_roughness_buffer = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_NORMAL_ROUGHNESS);
	RID instance_id_buffer = render_buffers->get_nextgeo_instance_id();
	RID velocity_buffer = p_with_velocity ? render_buffers->get_velocity_buffer(false) : RID();
	const bool with_vel = p_with_velocity && velocity_buffer.is_valid();
	// GodotNext: megalights_inputs (#136). Stage A writes the same targets as the depth prepass.
	if (p_megalights_inputs) {
		ERR_FAIL_COND_V_MSG(render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED, RID(), "MegaLights input targets are not allocated when MSAA is enabled.");
		render_buffers->ensure_megalights_inputs();
		RID ml_albedo = render_buffers->get_ml_albedo();
		RID ml_material = render_buffers->get_ml_material();
		if (p_voxel_gi) {
			ensure_voxelgi();
			RID voxelgi_buffer = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_VOXEL_GI);
			if (with_vel) {
				return FramebufferCacheRD::get_singleton()->get_cache_multiview(1, normal_roughness_buffer, ml_albedo, ml_material, voxelgi_buffer, instance_id_buffer, velocity_buffer);
			}
			return FramebufferCacheRD::get_singleton()->get_cache_multiview(1, normal_roughness_buffer, ml_albedo, ml_material, voxelgi_buffer, instance_id_buffer);
		}
		if (with_vel) {
			return FramebufferCacheRD::get_singleton()->get_cache_multiview(1, normal_roughness_buffer, ml_albedo, ml_material, instance_id_buffer, velocity_buffer);
		}
		return FramebufferCacheRD::get_singleton()->get_cache_multiview(1, normal_roughness_buffer, ml_albedo, ml_material, instance_id_buffer);
	}
	if (p_voxel_gi) {
		ensure_voxelgi();
		RID voxelgi_buffer = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_VOXEL_GI);
		if (with_vel) {
			return FramebufferCacheRD::get_singleton()->get_cache_multiview(1, normal_roughness_buffer, voxelgi_buffer, instance_id_buffer, velocity_buffer);
		}
		return FramebufferCacheRD::get_singleton()->get_cache_multiview(1, normal_roughness_buffer, voxelgi_buffer, instance_id_buffer);
	}
	if (with_vel) {
		return FramebufferCacheRD::get_singleton()->get_cache_multiview(1, normal_roughness_buffer, instance_id_buffer, velocity_buffer);
	}
	return FramebufferCacheRD::get_singleton()->get_cache_multiview(1, normal_roughness_buffer, instance_id_buffer);
}

RID RenderForwardClustered::RenderBufferDataForwardClustered::get_specular_only_fb() {
	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;

	RID specular = render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, use_msaa ? RB_TEX_SPECULAR_MSAA : RB_TEX_SPECULAR);

	return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), specular);
}

RID RenderForwardClustered::RenderBufferDataForwardClustered::get_velocity_only_fb() {
	bool use_msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;

	RID velocity = render_buffers->get_texture(RB_SCOPE_BUFFERS, use_msaa ? RB_TEX_VELOCITY_MSAA : RB_TEX_VELOCITY);

	return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), velocity);
}

RD::DataFormat RenderForwardClustered::RenderBufferDataForwardClustered::get_specular_format() {
	return RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
}

uint32_t RenderForwardClustered::RenderBufferDataForwardClustered::get_specular_usage_bits(bool p_resolve, bool p_msaa, bool p_storage) {
	return RenderSceneBuffersRD::get_color_usage_bits(p_resolve, p_msaa, p_storage);
}

RD::DataFormat RenderForwardClustered::RenderBufferDataForwardClustered::get_normal_roughness_format() {
	return RD::DATA_FORMAT_R8G8B8A8_UNORM;
}

uint32_t RenderForwardClustered::RenderBufferDataForwardClustered::get_normal_roughness_usage_bits(bool p_resolve, bool p_msaa, bool p_storage) {
	return RenderSceneBuffersRD::get_color_usage_bits(p_resolve, p_msaa, p_storage);
}

RD::DataFormat RenderForwardClustered::RenderBufferDataForwardClustered::get_voxelgi_format() {
	return RD::DATA_FORMAT_R8G8_UINT;
}

uint32_t RenderForwardClustered::RenderBufferDataForwardClustered::get_voxelgi_usage_bits(bool p_resolve, bool p_msaa, bool p_storage) {
	return RenderSceneBuffersRD::get_color_usage_bits(p_resolve, p_msaa, p_storage);
}

void RenderForwardClustered::setup_render_buffer_data(Ref<RenderSceneBuffersRD> p_render_buffers) {
	Ref<RenderBufferDataForwardClustered> data;
	data.instantiate();
	p_render_buffers->set_custom_data(RB_SCOPE_FORWARD_CLUSTERED, data);

	Ref<RendererRD::GI::RenderBuffersGI> rbgi;
	rbgi.instantiate();
	p_render_buffers->set_custom_data(RB_SCOPE_GI, rbgi);
}

bool RenderForwardClustered::free(RID p_rid) {
	if (RendererSceneRenderRD::free(p_rid)) {
		return true;
	}
	return false;
}

void RenderForwardClustered::update() {
	RendererSceneRenderRD::update();
	_update_global_pipeline_data_requirements_from_project();
	_update_global_pipeline_data_requirements_from_light_storage();
}

/// RENDERING ///

template <RenderForwardClustered::PassMode p_pass_mode, uint32_t p_color_pass_flags>
void RenderForwardClustered::_render_list_template(RenderingDevice::DrawListID p_draw_list, RenderingDevice::FramebufferFormatID p_framebuffer_Format, RenderListParameters *p_params, uint32_t p_from_element, uint32_t p_to_element) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::ParticlesStorage *particles_storage = RendererRD::ParticlesStorage::get_singleton();
	RD::DrawListID draw_list = p_draw_list;
	RD::FramebufferFormatID framebuffer_format = p_framebuffer_Format;

	//global scope bindings
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, render_base_uniform_set, SCENE_UNIFORM_SET);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, p_params->render_pass_uniform_set, RENDER_PASS_UNIFORM_SET);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, scene_shader.default_vec4_xform_uniform_set, TRANSFORMS_UNIFORM_SET);

	RID prev_material_uniform_set;

	RID prev_vertex_array_rd;
	RID prev_index_array_rd;
	RID prev_xforms_uniform_set;

	SceneShaderForwardClustered::ShaderData *shader = nullptr;
	SceneShaderForwardClustered::ShaderData *prev_shader = nullptr;
	SceneShaderForwardClustered::ShaderData::PipelineKey pipeline_key;
	// GodotNext: avboit (#295)
	pipeline_key.avboit = p_params->avboit_pass == RendererRD::AVBOIT::PASS_COMPOSITE;
	uint32_t pipeline_hash = 0;
	uint32_t prev_pipeline_hash = 0;

	bool shadow_pass = (p_pass_mode == PASS_MODE_SHADOW) || (p_pass_mode == PASS_MODE_SHADOW_DP);

	SceneState::PushConstant push_constant;

	if constexpr (p_pass_mode == PASS_MODE_DEPTH_MATERIAL) {
		if (p_params->nextgeo_fallback_mark) {
			push_constant.uv_offset = 0;
		} else {
			push_constant.uv_offset = Math::make_half_float(p_params->uv_offset.y) << 16;
			push_constant.uv_offset |= Math::make_half_float(p_params->uv_offset.x);
		}
	} else {
		push_constant.uv_offset = 0;
	}

	bool should_request_redraw = false;

	for (uint32_t i = p_from_element; i < p_to_element; i++) {
		const GeometryInstanceSurfaceDataCache *surf = p_params->elements[i];
		const RenderElementInfo &element_info = p_params->element_info[i];

		if (p_pass_mode == PASS_MODE_COLOR && surf->color_pass_inclusion_mask && (p_color_pass_flags & surf->color_pass_inclusion_mask) == 0) {
			// Some surfaces can be repeated in multiple render lists. We exclude them from being rendered on the color pass based on the
			// features supported by the pass compared to the exclusion mask.
			continue;
		}

		// GodotNext: avboit (#295). Each AVBOIT pass draws only its share of the alpha list.
		if (p_params->avboit_pass != RendererRD::AVBOIT::PASS_NONE && !RendererRD::AVBOIT::pass_draws(p_params->avboit_pass, surf->shader->blend_mode, surf->shader->uses_shadow_to_opacity)) {
			continue;
		}

		if (p_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS_FRONT_LAYER && (surf->shader->unshaded || surf->shader->depth_draw == SceneShaderForwardClustered::ShaderData::DEPTH_DRAW_DISABLED || surf->shader->depth_test == SceneShaderForwardClustered::ShaderData::DEPTH_TEST_DISABLED)) {
			continue;
		}

		if (surf->owner->instance_count == 0) {
			continue;
		}

		if (p_params->nextgeo_fallback_codes != nullptr) {
			push_constant.uv_offset = p_params->nextgeo_fallback_codes[i];
			if (push_constant.uv_offset == 0) {
				continue;
			}
		}

		push_constant.base_index = i + p_params->element_offset;

		const bool use_proxy = p_params->use_nextgeo_proxy && surf->surface_proxy != nullptr;
		const bool use_rt_proxy = p_params->use_nextgeo_rt_proxy && surf->surface_rt_proxy != nullptr;
		if (p_params->use_nextgeo_rt_proxy) {
			if (surf->owner->data->base_type != RSE::INSTANCE_MESH) {
				continue;
			}
			if (surf->mesh_has_virtual_geometry) {
				if (surf->surface_rt_proxy == nullptr) {
					continue;
				}
			} else if (surf->shader == nullptr || surf->shader->get_nextgi_rt_class() != RendererRD::MaterialStorage::ShaderData::NEXTGI_RT_OPAQUE) {
				continue;
			}
		}

		RID material_uniform_set;
		void *mesh_surface;

		if (shadow_pass || p_pass_mode == PASS_MODE_DEPTH) { //regular depth pass can use these too
			material_uniform_set = surf->material_uniform_set_shadow;
			shader = surf->shader_shadow;
			mesh_surface = use_rt_proxy ? surf->surface_rt_proxy : (use_proxy ? surf->surface_proxy : surf->surface_shadow);

		} else {
#ifdef DEBUG_ENABLED
			if (unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_LIGHTING)) {
				material_uniform_set = scene_shader.default_material_uniform_set;
				shader = scene_shader.default_material_shader_ptr;
			} else if (unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW)) {
				material_uniform_set = scene_shader.overdraw_material_uniform_set;
				shader = scene_shader.overdraw_material_shader_ptr;
			} else if (unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_PSSM_SPLITS)) {
				material_uniform_set = scene_shader.debug_shadow_splits_material_uniform_set;
				shader = scene_shader.debug_shadow_splits_material_shader_ptr;
			} else {
#endif
				material_uniform_set = surf->material_uniform_set;
				shader = surf->shader;
				surf->material->set_as_used();
#ifdef DEBUG_ENABLED
			}
#endif
			mesh_surface = use_rt_proxy ? surf->surface_rt_proxy : (use_proxy ? surf->surface_proxy : surf->surface);
		}

		if (!mesh_surface) {
			continue;
		}

		if (p_params->nextgeo_cracks_backface && shader->cull_mode == RSE::CULL_MODE_DISABLED) {
			continue;
		}

		if (RSG::virtual_mesh_storage != nullptr && surf->mesh_has_virtual_geometry && (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_VIRTUAL) != 0) {
			if (p_params->is_uv2_bake) {
				if (use_proxy) {
					RSG::virtual_mesh_storage->nextgeo_count_uv2_proxy_draw();
				} else {
					RSG::virtual_mesh_storage->nextgeo_count_uv2_authored_draw();
				}
			} else if (p_params->use_nextgeo_proxy) {
				if (use_proxy) {
					RSG::virtual_mesh_storage->nextgeo_count_auxiliary_proxy_draw();
				} else {
					RSG::virtual_mesh_storage->nextgeo_count_auxiliary_authored_virtual_draw();
				}
			}
		}

		//request a redraw if one of the shaders uses TIME
		if (shader->uses_time) {
			should_request_redraw = true;
		}

		// Determine the cull variant.
		SceneShaderForwardClustered::ShaderData::CullVariant cull_variant = SceneShaderForwardClustered::ShaderData::CULL_VARIANT_MAX;
		// GodotNext: avboit (#295). The AVBOIT pre-passes cull as the composite does.
		if ((p_pass_mode == PASS_MODE_DEPTH_MATERIAL && !p_params->nextgeo_fallback_mark && p_params->avboit_pass == RendererRD::AVBOIT::PASS_NONE) || p_pass_mode == PASS_MODE_SDF) {
			cull_variant = SceneShaderForwardClustered::ShaderData::CULL_VARIANT_DOUBLE_SIDED;
		} else {
			if constexpr (p_pass_mode == PASS_MODE_SHADOW || p_pass_mode == PASS_MODE_SHADOW_DP) {
				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_DOUBLE_SIDED_SHADOWS) {
					cull_variant = SceneShaderForwardClustered::ShaderData::CULL_VARIANT_DOUBLE_SIDED;
				}
			}

			if (cull_variant == SceneShaderForwardClustered::ShaderData::CULL_VARIANT_MAX) {
				bool mirror = surf->owner->mirror;
				if (p_params->reverse_cull) {
					mirror = !mirror;
				}
				if (p_params->nextgeo_cracks_backface) {
					mirror = !mirror;
				}

				cull_variant = mirror ? SceneShaderForwardClustered::ShaderData::CULL_VARIANT_REVERSED : SceneShaderForwardClustered::ShaderData::CULL_VARIANT_NORMAL;
			}
		}

		pipeline_key.primitive_type = surf->primitive;

		RID xforms_uniform_set = surf->owner->transforms_uniform_set;

		SceneShaderForwardClustered::ShaderSpecialization pipeline_specialization = p_params->base_specialization;
		pipeline_specialization.multimesh = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH);
		pipeline_specialization.multimesh_format_2d = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D);
		pipeline_specialization.multimesh_has_color = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR);
		pipeline_specialization.multimesh_has_custom_data = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA);
		pipeline_specialization.material_feedback = p_params->use_material_feedback && surf->material->material_feedback_rid.is_valid();

		if constexpr (p_pass_mode == PASS_MODE_COLOR) {
			pipeline_specialization.use_light_soft_shadows = element_info.uses_softshadow;
			pipeline_specialization.use_light_projector = element_info.uses_projector;
			pipeline_specialization.use_directional_soft_shadows = p_params->use_directional_soft_shadow;
			pipeline_specialization.use_lightmap_specular = element_info.uses_lightmap_specular;
		}

		pipeline_key.color_pass_flags = 0;

		switch (p_pass_mode) {
			case PASS_MODE_COLOR: {
				if (element_info.uses_lightmap) {
					pipeline_key.color_pass_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_LIGHTMAP;
				} else {
					pipeline_specialization.use_forward_gi = element_info.uses_forward_gi;
				}

				if constexpr ((p_color_pass_flags & COLOR_PASS_FLAG_SEPARATE_SPECULAR) != 0) {
					pipeline_key.color_pass_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_SEPARATE_SPECULAR;
				}

				if constexpr ((p_color_pass_flags & COLOR_PASS_FLAG_MOTION_VECTORS) != 0) {
					pipeline_key.color_pass_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS;
				}

				if constexpr ((p_color_pass_flags & COLOR_PASS_FLAG_TRANSPARENT) != 0) {
					pipeline_key.color_pass_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_TRANSPARENT;
				}

				if constexpr ((p_color_pass_flags & COLOR_PASS_FLAG_MULTIVIEW) != 0) {
					pipeline_key.color_pass_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_MULTIVIEW;
				}

				// GodotNext: rr_guides (#205)
				if constexpr ((p_color_pass_flags & COLOR_PASS_FLAG_RR_GUIDES) != 0) {
					pipeline_key.color_pass_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_RR_GUIDES;
				}

				// GodotNext: avboit (#295)
				if (p_params->avboit_pass == RendererRD::AVBOIT::PASS_COMPOSITE) {
					pipeline_specialization.avboit_composite = RendererRD::AVBOIT::composite_mode(shader->blend_mode, shader->uses_shadow_to_opacity);
				}

				pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_COLOR_PASS;
			} break;
			case PASS_MODE_SHADOW:
			case PASS_MODE_DEPTH: {
				pipeline_key.version = p_params->view_count > 1 ? SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_MULTIVIEW : SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS;
			} break;
			case PASS_MODE_SHADOW_DP: {
				ERR_FAIL_COND_MSG(p_params->view_count > 1, "Multiview not supported for shadow DP pass");
				pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_DP;
			} break;
			case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_FRONT_LAYER:
				pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS;
				break;
			case PASS_MODE_DEPTH_NORMAL_ROUGHNESS: {
				// GodotNext: megalights_inputs (#136)
				if (p_params->megalights_inputs) {
					if (p_params->prepass_motion) {
						pipeline_key.version = p_params->view_count > 1 ? SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_MOTION_MULTIVIEW : SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_MOTION;
					} else {
						pipeline_key.version = p_params->view_count > 1 ? SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_MULTIVIEW : SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS;
					}
				} else if (p_params->prepass_motion) {
					pipeline_key.version = p_params->view_count > 1 ? SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_MOTION_MULTIVIEW : SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_MOTION;
				} else {
					pipeline_key.version = p_params->view_count > 1 ? SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW : SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS;
				}
			} break;
			case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI: {
				// GodotNext: megalights_inputs (#136)
				if (p_params->megalights_inputs) {
					if (p_params->prepass_motion) {
						pipeline_key.version = p_params->view_count > 1 ? SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_AND_MOTION_MULTIVIEW : SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_AND_MOTION;
					} else {
						pipeline_key.version = p_params->view_count > 1 ? SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_MULTIVIEW : SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI;
					}
				} else if (p_params->prepass_motion) {
					pipeline_key.version = p_params->view_count > 1 ? SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_AND_MOTION_MULTIVIEW : SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_AND_MOTION;
				} else {
					pipeline_key.version = p_params->view_count > 1 ? SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW : SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI;
				}
			} break;
			case PASS_MODE_DEPTH_MATERIAL: {
				ERR_FAIL_COND_MSG(p_params->view_count > 1, "Multiview not supported for material pass");
				if (p_params->nextgeo_fallback_mark) {
					pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_NEXTGEO_FALLBACK;
				} else if (p_params->nextgi_translucency_mark) {
					pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_NEXTGI_TRANSLUCENCY_MARK;
				} else if (p_params->avboit_pass == RendererRD::AVBOIT::PASS_BOUNDS) { // GodotNext: avboit (#295)
					pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_AVBOIT_BOUNDS;
				} else if (p_params->avboit_pass == RendererRD::AVBOIT::PASS_OCCLUSION) {
					pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_AVBOIT_OCCLUSION;
				} else {
					pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_MATERIAL;
				}
			} break;
			case PASS_MODE_SDF: {
				// Note, SDF is prepared in world space, this shouldn't be a multiview buffer even when stereoscopic rendering is used.
				ERR_FAIL_COND_MSG(p_params->view_count > 1, "Multiview not supported for SDF pass");
				pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_SDF;
			} break;
		}

		pipeline_key.framebuffer_format_id = framebuffer_format;
		pipeline_key.wireframe = p_params->force_wireframe;
		pipeline_key.ubershader = 0;
		pipeline_key.zero_velocity_write = 0;
		if (p_params->prepass_motion) {
			const bool mover = (surf->owner->flags_cache & INSTANCE_DATA_FLAGS_DYNAMIC) || (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_MOTION_VECTOR);
			pipeline_key.zero_velocity_write = mover ? 0 : 1;
		} else if (surf->uses_prepass_velocity && (pipeline_key.color_pass_flags & SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS)) {
			pipeline_key.zero_velocity_write = 1;
		}

		bool emulate_point_size = shader->uses_point_size && scene_shader.emulate_point_size;

		const RD::PolygonCullMode cull_mode = shader->get_cull_mode_from_cull_variant(cull_variant);
		RID vertex_array_rd;
		RID index_array_rd;
		RID pipeline_rd;
		const uint32_t ubershader_iterations = 2;
		bool pipeline_valid = false;
		while (pipeline_key.ubershader < ubershader_iterations) {
			// Skeleton and blend shape.
			RD::VertexFormatID vertex_format = -1;
			bool pipeline_motion_vectors = (pipeline_key.color_pass_flags & SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS) || p_params->prepass_motion;
			uint64_t input_mask = shader->get_vertex_input_mask(pipeline_key.version, pipeline_key.color_pass_flags, pipeline_key.ubershader);
			if (surf->owner->mesh_instance.is_valid()) {
				mesh_storage->mesh_instance_surface_get_vertex_arrays_and_format(surf->owner->mesh_instance, surf->surface_index, input_mask, pipeline_motion_vectors, emulate_point_size, vertex_array_rd, vertex_format);
			} else {
				mesh_storage->mesh_surface_get_vertex_arrays_and_format(mesh_surface, input_mask, pipeline_motion_vectors, emulate_point_size, vertex_array_rd, vertex_format);
			}

			pipeline_key.vertex_format_id = vertex_format;

			if (pipeline_key.ubershader) {
				pipeline_key.shader_specialization = {};
				pipeline_key.cull_mode = RD::POLYGON_CULL_DISABLED;
			} else {
				pipeline_key.shader_specialization = pipeline_specialization;
				pipeline_key.cull_mode = cull_mode;
			}

			pipeline_hash = pipeline_key.hash();

			if (shader != prev_shader || pipeline_hash != prev_pipeline_hash) {
				RSE::PipelineSource pipeline_source = pipeline_key.ubershader ? RSE::PIPELINE_SOURCE_DRAW : RSE::PIPELINE_SOURCE_SPECIALIZATION;
				pipeline_rd = shader->pipeline_hash_map.get_pipeline(pipeline_key, pipeline_hash, pipeline_key.ubershader, pipeline_source);

				if (pipeline_rd.is_valid()) {
					pipeline_valid = true;
					prev_shader = shader;
					prev_pipeline_hash = pipeline_hash;
					break;
				} else {
					pipeline_key.ubershader++;
				}
			} else {
				// The same pipeline is bound already.
				pipeline_valid = true;
				break;
			}
		}

		if (pipeline_valid) {
			if (!emulate_point_size) {
				const uint32_t lod_index = (use_proxy || use_rt_proxy) ? 0u : element_info.lod_index;
				index_array_rd = mesh_storage->mesh_surface_get_index_array(mesh_surface, lod_index);
			} else {
				index_array_rd = RID();
			}

			if (prev_vertex_array_rd != vertex_array_rd) {
				RD::get_singleton()->draw_list_bind_vertex_array(draw_list, vertex_array_rd);
				prev_vertex_array_rd = vertex_array_rd;
			}

			if (prev_index_array_rd != index_array_rd) {
				if (index_array_rd.is_valid()) {
					RD::get_singleton()->draw_list_bind_index_array(draw_list, index_array_rd);
				}
				prev_index_array_rd = index_array_rd;
			}

			if (!pipeline_rd.is_null()) {
				RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, pipeline_rd);
			}

			if (xforms_uniform_set.is_valid() && prev_xforms_uniform_set != xforms_uniform_set) {
				RD::get_singleton()->draw_list_bind_uniform_set(draw_list, xforms_uniform_set, TRANSFORMS_UNIFORM_SET);
				prev_xforms_uniform_set = xforms_uniform_set;
			}

			if (material_uniform_set != prev_material_uniform_set) {
				// Update uniform set.
				if (material_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(material_uniform_set)) { // Material may not have a uniform set.
					RD::get_singleton()->draw_list_bind_uniform_set(draw_list, material_uniform_set, MATERIAL_UNIFORM_SET);
				}

				prev_material_uniform_set = material_uniform_set;
			}

			if (surf->owner->base_flags & INSTANCE_DATA_FLAG_PARTICLES) {
				particles_storage->particles_get_instance_buffer_motion_vectors_offsets(surf->owner->data->base, push_constant.multimesh_motion_vectors_current_offset, push_constant.multimesh_motion_vectors_previous_offset);
			} else if (surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH) {
				mesh_storage->_multimesh_get_motion_vectors_offsets(surf->owner->data->base, push_constant.multimesh_motion_vectors_current_offset, push_constant.multimesh_motion_vectors_previous_offset);
			} else {
				push_constant.multimesh_motion_vectors_current_offset = 0;
				push_constant.multimesh_motion_vectors_previous_offset = 0;
			}

			size_t push_constant_size = 0;
			if (pipeline_key.ubershader) {
				push_constant_size = sizeof(SceneState::PushConstant);
				push_constant.ubershader.specialization = pipeline_specialization;
				push_constant.ubershader.constants = {};
				push_constant.ubershader.constants.cull_mode = cull_mode;
			} else {
				push_constant_size = sizeof(SceneState::PushConstant) - sizeof(SceneState::PushConstantUbershader);
			}

			RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, push_constant_size);

			const uint32_t repeat_count = p_params->nextgeo_fallback_codes != nullptr ? 1u : element_info.repeat;
			uint32_t instance_count = surf->owner->instance_count > 1 ? surf->owner->instance_count : repeat_count;
			if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_PARTICLE_TRAILS) {
				instance_count /= surf->owner->trail_steps;
			}

			bool indirect = bool(surf->owner->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_INDIRECT);

			if (emulate_point_size) {
				if (indirect) {
					WARN_PRINT("Indirect draws are not supported when emulating point size.");
				}
				RD::get_singleton()->draw_list_draw(draw_list, false, mesh_storage->mesh_surface_get_vertex_count(mesh_surface), instance_count * 6);
			} else if (indirect) {
				RD::get_singleton()->draw_list_draw_indirect(draw_list, index_array_rd.is_valid(), mesh_storage->_multimesh_get_command_buffer_rd_rid(surf->owner->data->base), surf->surface_index * sizeof(uint32_t) * mesh_storage->INDIRECT_MULTIMESH_COMMAND_STRIDE, 1, 0);
			} else {
				RD::get_singleton()->draw_list_draw(draw_list, index_array_rd.is_valid(), instance_count);
			}
		}

		if (p_params->nextgeo_fallback_codes == nullptr) {
			i += element_info.repeat - 1; //skip equal elements
		}
	}

	// Make the actual redraw request
	if (should_request_redraw) {
		RenderingServerDefault::redraw_request();
	}
}

void RenderForwardClustered::_render_list(RenderingDevice::DrawListID p_draw_list, RenderingDevice::FramebufferFormatID p_framebuffer_Format, RenderListParameters *p_params, uint32_t p_from_element, uint32_t p_to_element) {
	//use template for faster performance (pass mode comparisons are inlined)

	switch (p_params->pass_mode) {
#define VALID_FLAG_COMBINATION(f) \
	case f: { \
		_render_list_template<PASS_MODE_COLOR, f>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element); \
	} break;

		case PASS_MODE_COLOR: {
			switch (p_params->color_pass_flags) {
				VALID_FLAG_COMBINATION(0);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_TRANSPARENT);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_TRANSPARENT | COLOR_PASS_FLAG_MULTIVIEW);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_TRANSPARENT | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_SEPARATE_SPECULAR);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_SEPARATE_SPECULAR | COLOR_PASS_FLAG_MULTIVIEW);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_SEPARATE_SPECULAR | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_MULTIVIEW);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_MULTIVIEW | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_SEPARATE_SPECULAR | COLOR_PASS_FLAG_MULTIVIEW | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_TRANSPARENT | COLOR_PASS_FLAG_MULTIVIEW | COLOR_PASS_FLAG_MOTION_VECTORS);
				// GodotNext: rr_guides (#205). Opaque, single-view passes only.
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_RR_GUIDES);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_RR_GUIDES | COLOR_PASS_FLAG_SEPARATE_SPECULAR);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_RR_GUIDES | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_RR_GUIDES | COLOR_PASS_FLAG_SEPARATE_SPECULAR | COLOR_PASS_FLAG_MOTION_VECTORS);
				default: {
					ERR_FAIL_MSG("Invalid color pass flag combination " + itos(p_params->color_pass_flags));
				}
			}

		} break;
		case PASS_MODE_SHADOW: {
			_render_list_template<PASS_MODE_SHADOW>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_SHADOW_DP: {
			_render_list_template<PASS_MODE_SHADOW_DP>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH: {
			_render_list_template<PASS_MODE_DEPTH>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_FRONT_LAYER: {
			_render_list_template<PASS_MODE_DEPTH_NORMAL_ROUGHNESS_FRONT_LAYER>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_NORMAL_ROUGHNESS: {
			_render_list_template<PASS_MODE_DEPTH_NORMAL_ROUGHNESS>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI: {
			_render_list_template<PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_MATERIAL: {
			_render_list_template<PASS_MODE_DEPTH_MATERIAL>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_SDF: {
			_render_list_template<PASS_MODE_SDF>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		default: {
			// Unknown pass mode.
		} break;
	}
}

void RenderForwardClustered::_render_list_with_draw_list(RenderListParameters *p_params, RID p_framebuffer, BitField<RD::DrawFlags> p_draw_flags, const Vector<Color> &p_clear_color_values, float p_clear_depth_value, uint32_t p_clear_stencil_value, const Rect2 &p_region) {
	RD::FramebufferFormatID fb_format = RD::get_singleton()->framebuffer_get_format(p_framebuffer);
	p_params->framebuffer_format = fb_format;

	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, p_draw_flags, p_clear_color_values, p_clear_depth_value, p_clear_stencil_value, p_region);
	_render_list(draw_list, fb_format, p_params, 0, p_params->element_count);
	RD::get_singleton()->draw_list_end();
}

uint32_t RenderForwardClustered::_setup_environment(const RenderDataRD *p_render_data, bool p_no_fog, const Size2i &p_screen_size, const Size2 &p_viewport_size, const Color &p_default_bg_color, bool p_opaque_render_buffers, bool p_apply_alpha_multiplier, bool p_pancake_shadows) {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	Ref<RenderSceneBuffersRD> rd = p_render_data->render_buffers;
	RID env = is_environment(p_render_data->environment) ? p_render_data->environment : RID();
	RID reflection_probe_instance = p_render_data->reflection_probe.is_valid() ? light_storage->reflection_probe_instance_get_probe(p_render_data->reflection_probe) : RID();

	// May do this earlier in RenderSceneRenderRD::render_scene
	uint32_t uniform_buffer_index = scene_state.used_uniform_buffer_count;
	++scene_state.used_uniform_buffer_count;

	if (uniform_buffer_index >= scene_state.uniform_buffers.size()) {
		uint32_t from = scene_state.uniform_buffers.size();
		scene_state.uniform_buffers.resize(uniform_buffer_index + 1);
		for (uint32_t i = from; i < scene_state.uniform_buffers.size(); i++) {
			scene_state.uniform_buffers[i] = p_render_data->scene_data->create_uniform_buffer();
		}
	}

	float luminance_multiplier = rd.is_valid() ? rd->get_luminance_multiplier() : 1.0;

	p_render_data->scene_data->update_ubo(scene_state.uniform_buffers[uniform_buffer_index], get_debug_draw_mode(), env, reflection_probe_instance, p_render_data->camera_attributes, p_pancake_shadows, p_screen_size, p_viewport_size, p_default_bg_color, luminance_multiplier, p_opaque_render_buffers, p_apply_alpha_multiplier);

	// now do implementation UBO

	scene_state.ubo.cluster_shift = Math::get_shift_from_power_of_2(p_render_data->cluster_size);
	scene_state.ubo.max_cluster_element_count_div_32 = p_render_data->cluster_max_elements / 32;
	{
		uint32_t cluster_screen_width = Math::division_round_up((uint32_t)p_screen_size.width, p_render_data->cluster_size);
		uint32_t cluster_screen_height = Math::division_round_up((uint32_t)p_screen_size.height, p_render_data->cluster_size);
		scene_state.ubo.cluster_type_size = cluster_screen_width * cluster_screen_height * (scene_state.ubo.max_cluster_element_count_div_32 + 32);
		scene_state.ubo.cluster_width = cluster_screen_width;
	}

	// GodotNext: light_grid (#139, plan §21.3 contract M2).
	scene_state.ubo.light_grid_dims[0] = uint32_t(MAX(p_render_data->packed_light_grid_dims.x, 1));
	scene_state.ubo.light_grid_dims[1] = uint32_t(MAX(p_render_data->packed_light_grid_dims.y, 1));
	scene_state.ubo.light_grid_dims[2] = uint32_t(MAX(p_render_data->packed_light_grid_dims.z, 1));
	scene_state.ubo.light_grid_dims[3] = p_render_data->packed_light_grid_flags;
	scene_state.ubo.light_grid_z_params[0] = p_render_data->packed_light_grid_z_near;
	scene_state.ubo.light_grid_z_params[1] = p_render_data->packed_light_grid_z_far;
	scene_state.ubo.light_grid_z_params[2] = p_render_data->packed_light_grid_z_scale;
	scene_state.ubo.light_grid_z_params[3] = p_render_data->packed_light_grid_z_bias;

	scene_state.ubo.gi_upscale_for_msaa = false;
	scene_state.ubo.volumetric_fog_enabled = false;

	if (rd.is_valid()) {
		if (rd->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED) {
			scene_state.ubo.gi_upscale_for_msaa = true;
		}

		if (rd->has_custom_data(RB_SCOPE_FOG)) {
			Ref<RendererRD::Fog::VolumetricFog> fog = rd->get_custom_data(RB_SCOPE_FOG);

			scene_state.ubo.volumetric_fog_enabled = true;
			float fog_end = fog->length;
			if (fog_end > 0.0) {
				scene_state.ubo.volumetric_fog_inv_length = 1.0 / fog_end;
			} else {
				scene_state.ubo.volumetric_fog_inv_length = 1.0;
			}

			float fog_detail_spread = fog->spread; //reverse lookup
			if (fog_detail_spread > 0.0) {
				scene_state.ubo.volumetric_fog_detail_spread = 1.0 / fog_detail_spread;
			} else {
				scene_state.ubo.volumetric_fog_detail_spread = 1.0;
			}
		}
	}

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_UNSHADED) {
		scene_state.ubo.ss_effects_flags = 0;
	} else if (p_render_data->reflection_probe.is_null() && is_environment(p_render_data->environment)) {
		scene_state.ubo.ssao_ao_affect = environment_get_ssao_ao_channel_affect(p_render_data->environment);
		scene_state.ubo.ssao_light_affect = environment_get_ssao_direct_light_affect(p_render_data->environment);
		uint32_t ss_flags = 0;
		if (p_opaque_render_buffers) {
			// GodotNext: nextgi_frame_block. A NextGI view reads the GI buffers instead of SSIL.
			// GodotNext: ps_swrt_trace (#186). Both active tiers run the frame block (SWRT one view at a time).
			const bool nextgi_view = p_render_data->environment.is_valid() && RendererRD::NextGI::view_uses_frame_block(next_gi.get_tier_result().tier, p_render_data->reflection_probe.is_valid(), environment_get_nextgi_enabled(p_render_data->environment), false, p_render_data->scene_data->view_count);
			ss_flags |= environment_get_ssao_enabled(p_render_data->environment) ? SCREEN_SPACE_EFFECTS_FLAGS_USE_SSAO : 0;
			// GodotNext: nextgi_frame_block, ps_lumen_lite (#183). SSIL only runs without NextGI.
			ss_flags |= (!nextgi_view && environment_get_ssil_enabled(p_render_data->environment)) ? SCREEN_SPACE_EFFECTS_FLAGS_USE_SSIL : 0;
			ss_flags |= nextgi_view ? SCREEN_SPACE_EFFECTS_FLAGS_USE_NEXTGI : 0;
			// GodotNext: nextgi_reflection_behaviours (#132)
			ss_flags |= nextgi_view && !RendererRD::NextGI::is_reflections_enabled() && !environment_get_ssr_enabled(p_render_data->environment) ? SCREEN_SPACE_EFFECTS_FLAGS_NEXTGI_ROUGH_REFLECTION : 0;
			// GodotNext: ps_lumen_lite (#183). Lite traces no reflections: below its SSR roughness
			// the forward specular (probes, sky, SSR) stays instead of the ambient fallback.
			ss_flags |= (nextgi_view && RendererRD::NextGI::is_lumen_lite()) ? SCREEN_SPACE_EFFECTS_FLAGS_NEXTGI_LITE : 0;
			ss_flags |= environment_get_ssr_enabled(p_render_data->environment) ? SCREEN_SPACE_EFFECTS_FLAGS_USE_SSR : 0;
			// GodotNext: light_source_parity (#140). Only enable SSCS when at least one visible light has active contact shadows.
			ss_flags |= !p_render_data->contact_shadow_lights.is_empty() ? SCREEN_SPACE_EFFECTS_FLAGS_USE_SSCS : 0;

			if (rd.is_valid()) {
				Ref<RenderBufferDataForwardClustered> rb_data;
				if (rd->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
					rb_data = rd->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
					ss_flags |= (rb_data.is_valid() && !rb_data->ss_effects_data.ssr.half_size) ? SCREEN_SPACE_EFFECTS_FLAGS_RESOLVE_SSR : 0;
				}
			}
		}
		scene_state.ubo.ss_effects_flags = ss_flags;
	} else {
		// GodotNext: light_source_parity (#140). Per-light contact shadows also work without a WorldEnvironment.
		uint32_t ss_flags = 0;
		if (p_render_data->reflection_probe.is_null() && p_opaque_render_buffers && !p_render_data->contact_shadow_lights.is_empty()) {
			ss_flags |= SCREEN_SPACE_EFFECTS_FLAGS_USE_SSCS;
		}
		scene_state.ubo.ss_effects_flags = ss_flags;
	}

	if (uniform_buffer_index >= scene_state.implementation_uniform_buffers.size()) {
		uint32_t from = scene_state.implementation_uniform_buffers.size();
		scene_state.implementation_uniform_buffers.resize(uniform_buffer_index + 1);
		for (uint32_t i = from; i < scene_state.implementation_uniform_buffers.size(); i++) {
			scene_state.implementation_uniform_buffers[i] = RD::get_singleton()->uniform_buffer_create(sizeof(SceneState::UBO));
		}
	}

	RD::get_singleton()->buffer_update(scene_state.implementation_uniform_buffers[uniform_buffer_index], 0, sizeof(SceneState::UBO), &scene_state.ubo);

	return uniform_buffer_index;
}

void RenderForwardClustered::SceneState::grow_instance_buffer(RenderListType p_render_list, uint32_t p_req_element_count, bool p_append) {
	if (p_req_element_count > 0) {
		if (instance_buffer[p_render_list].get_size(0u) < p_req_element_count * sizeof(SceneState::InstanceData)) {
			instance_buffer[p_render_list].uninit();
			uint32_t new_size = Math::nearest_power_of_2_templated(MAX(uint64_t(INSTANCE_DATA_BUFFER_MIN_SIZE), p_req_element_count));
			instance_buffer[p_render_list].set_storage_size(0u, new_size * sizeof(SceneState::InstanceData));
			curr_gpu_ptr[p_render_list] = nullptr;
		}

		const bool must_remap = instance_buffer[p_render_list].prepare_for_map(p_append);
		if (must_remap) {
			curr_gpu_ptr[p_render_list] = nullptr;
		}
	}
}

RID RenderForwardClustered::SceneState::get_spline_deform_buffer(RenderListType p_render_list) {
	if (spline_deform_entries[p_render_list].is_empty()) {
		return default_spline_deform_buffer;
	}
	RenderingDevice *rd = RD::get_singleton();
	const uint32_t count = spline_deform_entries[p_render_list].size();
	if (!spline_deform_buffer[p_render_list].is_valid() || spline_deform_capacity[p_render_list] < count) {
		if (spline_deform_buffer[p_render_list].is_valid()) {
			rd->free_rid(spline_deform_buffer[p_render_list]);
		}
		spline_deform_capacity[p_render_list] = MAX(Math::next_power_of_2(count), 16u);
		spline_deform_buffer[p_render_list] = rd->storage_buffer_create(spline_deform_capacity[p_render_list] * sizeof(SplineDeformEntry));
	}
	if (spline_deform_dirty[p_render_list] && spline_deform_buffer[p_render_list].is_valid()) {
		rd->buffer_update(spline_deform_buffer[p_render_list], 0, count * sizeof(SplineDeformEntry), spline_deform_entries[p_render_list].ptr());
		spline_deform_dirty[p_render_list] = false;
	}
	return spline_deform_buffer[p_render_list].is_valid() ? spline_deform_buffer[p_render_list] : default_spline_deform_buffer;
}

void RenderForwardClustered::_fill_instance_data(RenderListType p_render_list, int *p_render_info, uint32_t p_offset, int32_t p_max_elements, bool p_update_buffer) {
	RenderList *rl = &render_list[p_render_list];
	uint32_t element_total = p_max_elements >= 0 ? uint32_t(p_max_elements) : rl->elements.size();

	rl->element_info.resize(p_offset + element_total);

	// If p_offset == 0, grow_instance_buffer resets and increment the buffer.
	// If this behavior ever changes, _render_shadow_begin may need to change.
	scene_state.grow_instance_buffer(p_render_list, p_offset + element_total, p_offset != 0u);
	if (!scene_state.curr_gpu_ptr[p_render_list] && element_total > 0u) {
		// The old buffer was replaced for another larger one. We must start copying from scratch.
		element_total += p_offset;
		p_offset = 0u;
		scene_state.curr_gpu_ptr[p_render_list] = reinterpret_cast<SceneState::InstanceData *>(scene_state.instance_buffer[p_render_list].map_raw_for_upload(0u));
	}
	if (p_offset == 0u) {
		scene_state.spline_deform_entries[p_render_list].clear();
		scene_state.spline_deform_dirty[p_render_list] = false;
	}

	if (p_render_info) {
		p_render_info[RSE::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME] += element_total;
	}

	uint32_t repeats = 0;
	GeometryInstanceSurfaceDataCache *prev_surface = nullptr;
	for (uint32_t i = 0; i < element_total; i++) {
		GeometryInstanceSurfaceDataCache *surface = rl->elements[i + p_offset];
		GeometryInstanceForwardClustered *inst = surface->owner;

		SceneState::InstanceData instance_data;

		if (likely(inst->store_transform_cache)) {
			RendererRD::MaterialStorage::store_transform_transposed_3x4(inst->transform, instance_data.transform);
			RendererRD::MaterialStorage::store_transform_transposed_3x4(inst->prev_transform, instance_data.prev_transform);

#ifdef REAL_T_IS_DOUBLE
			// Split the origin into two components, the float approximation and the missing precision.
			// In the shader we will combine these back together to restore the lost precision.
			RendererRD::MaterialStorage::split_double(inst->transform.origin.x, &instance_data.transform[3], &instance_data.model_precision[0]);
			RendererRD::MaterialStorage::split_double(inst->transform.origin.y, &instance_data.transform[7], &instance_data.model_precision[1]);
			RendererRD::MaterialStorage::split_double(inst->transform.origin.z, &instance_data.transform[11], &instance_data.model_precision[2]);
			RendererRD::MaterialStorage::split_double(inst->prev_transform.origin.x, &instance_data.prev_transform[3], &instance_data.prev_model_precision[0]);
			RendererRD::MaterialStorage::split_double(inst->prev_transform.origin.y, &instance_data.prev_transform[7], &instance_data.prev_model_precision[1]);
			RendererRD::MaterialStorage::split_double(inst->prev_transform.origin.z, &instance_data.prev_transform[11], &instance_data.prev_model_precision[2]);
#endif
		} else {
			RendererRD::MaterialStorage::store_transform_transposed_3x4(Transform3D(), instance_data.transform);
			RendererRD::MaterialStorage::store_transform_transposed_3x4(Transform3D(), instance_data.prev_transform);
#ifdef REAL_T_IS_DOUBLE
			memset(instance_data.model_precision, 0, sizeof(instance_data.model_precision));
			memset(instance_data.prev_model_precision, 0, sizeof(instance_data.prev_model_precision));
#endif
		}

		instance_data.flags = inst->flags_cache;
		// GodotNext: nextgi_alpha_policy (#61). Transparent instances take their ambient from the
		// translucency path instead of the opaque GI buffers.
		if (p_render_list == RENDER_LIST_ALPHA && nextgi_active && !(instance_data.flags & INSTANCE_DATA_FLAG_USE_LIGHTMAP)) {
			instance_data.flags |= INSTANCE_DATA_FLAG_USE_NEXTGI_TRANSLUCENCY;
		}
		instance_data.gi_offset = inst->gi_offset_cache;
		instance_data.layer_mask = inst->layer_mask;
		instance_data.instance_uniforms_ofs = uint32_t(inst->shader_uniforms_offset);
		instance_data.set_lightmap_uv_scale(inst->lightmap_uv_scale);

		void *active_surface = surface->surface;
		if (nextgeo_rt_proxy_active && surface->surface_rt_proxy != nullptr) {
			active_surface = surface->surface_rt_proxy;
		} else if (nextgeo_proxy_active && surface->surface_proxy != nullptr) {
			active_surface = surface->surface_proxy;
		}

		AABB surface_aabb = AABB(Vector3(0.0, 0.0, 0.0), Vector3(1.0, 1.0, 1.0));
		uint64_t format = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_format(active_surface);
		Vector4 uv_scale = Vector4(0.0, 0.0, 0.0, 0.0);

		if (format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) {
			surface_aabb = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_aabb(active_surface);
			uv_scale = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_uv_scale(active_surface);
		}

		uint32_t material_feedback_index = UINT32_MAX;
#ifdef MODULE_TEXTURE_STREAMING_ENABLED
		if (surface->material->material_feedback_rid.is_valid()) {
			material_feedback_index = TextureStreaming::get_singleton()->feedback_buffer_material_index(surface->material->material_feedback_rid);
		}
#endif

		instance_data.set_compressed_aabb(surface_aabb);
		// GodotNext: megalights_inputs (#136). Only the opaque prepass writes ml_material.
		const bool ml_assign = megalights_inputs_active && p_render_list == RENDER_LIST_OPAQUE;
		instance_data.set_megalights_layer_group(ml_assign ? RendererRD::MegaLightsInputs::assign_layer_group(inst->layer_mask) : 0);
		instance_data.material_feedback_index = material_feedback_index;
		instance_data.set_uv_scale(uv_scale);
		// GodotNext: spline_meshes (#161). Pack current/previous spline poses only for active spline instances.
		if (inst->spline_deform_active) {
			SplineDeformEntry spline_entry;
			spline_entry.current = inst->spline_deform;
			spline_entry.previous = inst->prev_spline_deform;
			const uint32_t spline_idx = scene_state.spline_deform_entries[p_render_list].size();
			scene_state.spline_deform_entries[p_render_list].push_back(spline_entry);
			scene_state.spline_deform_dirty[p_render_list] = true;
			instance_data.spline_deform_data[0] = spline_idx;
			instance_data.spline_deform_data[1] = 1u;
			instance_data.spline_deform_data[2] = 0u;
			instance_data.spline_deform_data[3] = 0u;
		} else {
			instance_data.spline_deform_data[0] = 0u;
			instance_data.spline_deform_data[1] = 0u;
			instance_data.spline_deform_data[2] = 0u;
			instance_data.spline_deform_data[3] = 0u;
		}

		scene_state.curr_gpu_ptr[p_render_list][i + p_offset] = instance_data;

		const bool cant_repeat = instance_data.flags & INSTANCE_DATA_FLAG_MULTIMESH || inst->mesh_instance.is_valid();

		if (prev_surface != nullptr && !cant_repeat && prev_surface->sort.sort_key1 == surface->sort.sort_key1 && prev_surface->sort.sort_key2 == surface->sort.sort_key2 && prev_surface->uses_lightmap_specular == surface->uses_lightmap_specular && inst->mirror == prev_surface->owner->mirror && repeats < RenderElementInfo::MAX_REPEATS) {
			//this element is the same as the previous one, count repeats to draw it using instancing
			repeats++;
		} else {
			if (repeats > 0) {
				for (uint32_t j = 1; j <= repeats; j++) {
					rl->element_info[p_offset + i - j].repeat = j;
				}
			}
			repeats = 1;
			if (p_render_info) {
				p_render_info[RSE::VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME]++;
			}
		}

		RenderElementInfo &element_info = rl->element_info[p_offset + i];

		// Manually bit shift and OR the "uses_lightmap_specular" value to avoid redundant memory stores generated by the compiler.
		element_info.value = uint32_t(surface->sort.sort_key1 & 0xFFF) | (uint32_t(surface->uses_lightmap_specular) << 12);

		if (cant_repeat) {
			prev_surface = nullptr;
		} else {
			prev_surface = surface;
		}
	}

	if (repeats > 0) {
		for (uint32_t j = 1; j <= repeats; j++) {
			rl->element_info[p_offset + element_total - j].repeat = j;
		}
	}

	if (p_update_buffer && element_total > 0u) {
		RenderingDevice::get_singleton()->buffer_flush(scene_state.instance_buffer[p_render_list]._get(0u));
	}
}

_FORCE_INLINE_ static uint32_t _indices_to_primitives(RSE::PrimitiveType p_primitive, uint32_t p_indices) {
	static const uint32_t divisor[RSE::PRIMITIVE_MAX] = { 1, 2, 1, 3, 1 };
	static const uint32_t subtractor[RSE::PRIMITIVE_MAX] = { 0, 0, 1, 0, 2 };
	return (p_indices - subtractor[p_primitive]) / divisor[p_primitive];
}
void RenderForwardClustered::_fill_render_list(RenderListType p_render_list, const RenderDataRD *p_render_data, PassMode p_pass_mode, bool p_using_sdfgi, bool p_using_opaque_gi, bool p_using_motion_pass, bool p_append, bool p_nextgi_velocity, bool p_using_nextgi) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	uint64_t frame = RSG::rasterizer->get_frame_number();

	if (p_render_list == RENDER_LIST_OPAQUE) {
		scene_state.used_sss = false;
		scene_state.used_screen_texture = false;
		scene_state.used_normal_texture = false;
		scene_state.used_depth_texture = false;
		scene_state.used_lightmap = false;
		scene_state.used_opaque_stencil = false;
	}
	uint32_t lightmap_captures_used = 0;

	Plane near_plane = Plane(-p_render_data->scene_data->cam_transform.basis.get_column(Vector3::AXIS_Z), p_render_data->scene_data->cam_transform.origin);
	near_plane.d += p_render_data->scene_data->cam_projection.get_z_near();
	float z_max = p_render_data->scene_data->cam_projection.get_z_far() - p_render_data->scene_data->cam_projection.get_z_near();

	RenderList *rl = &render_list[p_render_list];
	_update_dirty_geometry_instances();

	// GodotNext: nextgeo_exclusive_dispatch (#79) / nextgeo_resolve (#80).
	// Routing is per main view: MSAA, multiview, reflection probes, auxiliary
	// passes and unsupported debug views keep virtual surfaces legacy.
	bool nextgeo_virtual_view = false;
	if (nextgeo_virtual_active &&
			p_render_list == RENDER_LIST_OPAQUE &&
			p_pass_mode == PASS_MODE_COLOR &&
			!p_render_data->reflection_probe.is_valid() &&
			p_render_data->render_buffers.is_valid() &&
			RSG::virtual_mesh_storage != nullptr) {
		nextgeo_virtual_view = true;
		if (!p_append) {
			if (!nextgeo_visibility_only_active || nextgeo_programmable_view_active) {
				nextgeo_resolve.begin_frame();
				const uint32_t bucket_count = RSG::virtual_mesh_storage->get_bucket_record_count();
				nextgeo_bucket_table.resize(MAX(bucket_count * 32u, 1u));
				for (uint32_t k = 0; k < nextgeo_bucket_table.size(); k++) {
					nextgeo_bucket_table[k] = NextGeoResolve::BUCKET_INVALID;
				}
				nextgeo_instance_data.resize(MAX(bucket_count, 1u));
				memset(nextgeo_instance_data.ptr(), 0, nextgeo_instance_data.size() * sizeof(SceneState::InstanceData));
			} else {
				nextgeo_bucket_table.clear();
				nextgeo_instance_data.clear();
			}
		}
		// GodotNext: nextgeo_raster_bins (#155). The raster-bin table exists only while a programmable bin does.
		if (!p_append) {
			nextgeo_raster_bins_used = 0;
			// GodotNext: vsm_nanite (#217). Clear per-view bin owners so recycled bins never retain stale pointers.
			for (uint32_t bin = 0; bin < RendererVirtualMeshStorage::RASTER_BIN_LIMIT; bin++) {
				nextgeo_raster_bin_owners[bin] = {};
			}
			if (nextgeo_programmable_view_active) {
				const uint32_t bucket_count = RSG::virtual_mesh_storage->get_bucket_record_count();
				nextgeo_raster_bin_table.resize(MAX(bucket_count * RendererRD::NEXTGEO_RASTER_BIN_TABLE_WORDS_PER_RECORD, 1u));
				memset(nextgeo_raster_bin_table.ptr(), 0, nextgeo_raster_bin_table.size() * sizeof(uint32_t));
			} else {
				nextgeo_raster_bin_table.clear();
			}
		}
	}

	bool nextgeo_request_redraw = false;

	if (!p_append) {
		rl->clear();
		if (p_render_list == RENDER_LIST_OPAQUE) {
			// Opaque fills motion and alpha lists.
			render_list[RENDER_LIST_MOTION].clear();
			render_list[RENDER_LIST_ALPHA].clear();
		}
	}

	//fill list

	for (int i = 0; i < (int)p_render_data->instances->size(); i++) {
		GeometryInstanceForwardClustered *inst = static_cast<GeometryInstanceForwardClustered *>((*p_render_data->instances)[i]);

		Vector3 center = inst->transform.origin;
		if (p_render_data->scene_data->cam_orthogonal) {
			if (inst->use_aabb_center) {
				center = inst->transformed_aabb.get_support(-near_plane.normal);
			}
			inst->depth = near_plane.distance_to(center) - inst->sorting_offset;
		} else {
			if (inst->use_aabb_center) {
				center = inst->transformed_aabb.position + (inst->transformed_aabb.size * 0.5);
			}
			inst->depth = p_render_data->scene_data->cam_transform.origin.distance_to(center) - inst->sorting_offset;
		}
		uint32_t depth_layer = CLAMP(int(inst->depth * 16 / z_max), 0, 15);

		uint32_t flags = inst->base_flags; //fill flags if appropriate

		if (inst->non_uniform_scale) {
			flags |= INSTANCE_DATA_FLAGS_NON_UNIFORM_SCALE;
		}
		bool uses_lightmap = false;
		bool uses_lightmap_specular = false;
		bool uses_gi = false;
		bool uses_motion = false;
		float fade_alpha = 1.0;

		if (inst->fade_near || inst->fade_far) {
			float fade_dist = inst->transformed_aabb.get_center().distance_to(p_render_data->scene_data->cam_transform.origin);
			// Use `smoothstep()` to make opacity changes more gradual and less noticeable to the player.
			if (inst->fade_far && fade_dist > inst->fade_far_begin) {
				fade_alpha = Math::smoothstep(0.0f, 1.0f, 1.0f - (fade_dist - inst->fade_far_begin) / (inst->fade_far_end - inst->fade_far_begin));
			} else if (inst->fade_near && fade_dist < inst->fade_near_end) {
				fade_alpha = Math::smoothstep(0.0f, 1.0f, (fade_dist - inst->fade_near_begin) / (inst->fade_near_end - inst->fade_near_begin));
			}
		}

		fade_alpha *= inst->force_alpha * inst->parent_fade_alpha;

		flags = (flags & ~INSTANCE_DATA_FLAGS_FADE_MASK) | (uint32_t(fade_alpha * 255.0) << INSTANCE_DATA_FLAGS_FADE_SHIFT);

		if (p_render_list == RENDER_LIST_OPAQUE) {
			// Setup GI
			if (inst->lightmap_instance.is_valid()) {
				// find index of the lightmap_instance of the instance being rendered
				int32_t lightmap_cull_index = -1;
				for (uint32_t j = 0; j < scene_state.lightmaps_used; j++) {
					if (scene_state.lightmap_ids[j] == inst->lightmap_instance) {
						lightmap_cull_index = j;
						break;
					}
				}
				if (lightmap_cull_index >= 0) {
					inst->gi_offset_cache = inst->lightmap_slice_index << 16;
					inst->gi_offset_cache |= lightmap_cull_index;
					flags |= INSTANCE_DATA_FLAG_USE_LIGHTMAP;
					if (scene_state.lightmap_has_sh[lightmap_cull_index]) {
						flags |= INSTANCE_DATA_FLAG_USE_SH_LIGHTMAP;
						uses_lightmap_specular = scene_state.lightmap_has_specular[lightmap_cull_index];
					}
					uses_lightmap = true;
				} else {
					inst->gi_offset_cache = 0xFFFFFFFF;
				}

			} else if (inst->lightmap_sh && !p_using_nextgi) {
				if (lightmap_captures_used < scene_state.max_lightmap_captures) {
					const Color *src_capture = inst->lightmap_sh->sh;
					LightmapCaptureData &lcd = scene_state.lightmap_captures[lightmap_captures_used];
					for (int j = 0; j < 9; j++) {
						lcd.sh[j * 4 + 0] = src_capture[j].r;
						lcd.sh[j * 4 + 1] = src_capture[j].g;
						lcd.sh[j * 4 + 2] = src_capture[j].b;
						lcd.sh[j * 4 + 3] = src_capture[j].a;
					}
					flags |= INSTANCE_DATA_FLAG_USE_LIGHTMAP_CAPTURE;
					inst->gi_offset_cache = lightmap_captures_used;
					lightmap_captures_used++;
					uses_lightmap = true;
				}

			} else {
				if (p_using_opaque_gi || (p_using_nextgi && inst->lightmap_sh)) {
					flags |= INSTANCE_DATA_FLAG_USE_GI_BUFFERS;
				}

				if (p_using_nextgi) {
					inst->gi_offset_cache = 0xFFFFFFFF;
				} else if (inst->voxel_gi_instances[0].is_valid()) {
					uint32_t probe0_index = 0xFFFF;
					uint32_t probe1_index = 0xFFFF;

					for (uint32_t j = 0; j < scene_state.voxelgis_used; j++) {
						if (scene_state.voxelgi_ids[j] == inst->voxel_gi_instances[0]) {
							probe0_index = j;
						} else if (scene_state.voxelgi_ids[j] == inst->voxel_gi_instances[1]) {
							probe1_index = j;
						}
					}

					if (probe0_index == 0xFFFF && probe1_index != 0xFFFF) {
						//0 must always exist if a probe exists
						SWAP(probe0_index, probe1_index);
					}

					inst->gi_offset_cache = probe0_index | (probe1_index << 16);
					flags |= INSTANCE_DATA_FLAG_USE_VOXEL_GI;
					uses_gi = true;
				} else {
					if (p_using_sdfgi && inst->can_sdfgi) {
						flags |= INSTANCE_DATA_FLAG_USE_SDFGI;
						uses_gi = true;
					}
					inst->gi_offset_cache = 0xFFFFFFFF;
				}
			}
			if (p_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS || p_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS_FRONT_LAYER || p_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI || p_pass_mode == PASS_MODE_COLOR) {
				bool transform_changed = inst->transform_status == GeometryInstanceForwardClustered::TransformStatus::MOVED;
				bool spline_changed = inst->spline_deform_active && inst->spline_changed_this_frame;
				bool has_mesh_instance = inst->mesh_instance.is_valid();
				bool uses_particles = inst->base_flags & INSTANCE_DATA_FLAG_PARTICLES;
				bool is_multimesh_with_motion = !uses_particles && (inst->base_flags & INSTANCE_DATA_FLAG_MULTIMESH) && mesh_storage->_multimesh_uses_motion_vectors_offsets(inst->data->base);
				bool is_dynamic = transform_changed || spline_changed || has_mesh_instance || uses_particles || is_multimesh_with_motion;
				if (p_nextgi_velocity && is_dynamic) {
					flags |= INSTANCE_DATA_FLAGS_DYNAMIC;
				}
				if (p_pass_mode == PASS_MODE_COLOR && p_using_motion_pass) {
					uses_motion = is_dynamic;
				} else if (is_dynamic && !p_nextgi_velocity) {
					flags |= INSTANCE_DATA_FLAGS_DYNAMIC;
				}
			}
		}
		inst->flags_cache = flags;

		GeometryInstanceSurfaceDataCache *surf = inst->surface_caches;

		float lod_distance = 0.0;

		if (p_render_data->scene_data->cam_orthogonal) {
			lod_distance = 1.0;
		} else {
			Vector3 aabb_min = inst->transformed_aabb.position;
			Vector3 aabb_max = inst->transformed_aabb.position + inst->transformed_aabb.size;
			Vector3 camera_position = p_render_data->scene_data->main_cam_transform.origin;
			Vector3 surface_distance = Vector3(0.0, 0.0, 0.0).max(aabb_min - camera_position).max(camera_position - aabb_max);

			lod_distance = surface_distance.length();
		}

		if (unlikely(inst->transform_status != GeometryInstanceForwardClustered::TransformStatus::NONE && frame > inst->prev_transform_change_frame && inst->prev_transform_change_frame)) {
			inst->prev_transform = inst->transform;
			inst->transform_status = GeometryInstanceForwardClustered::TransformStatus::NONE;
		}
		if (unlikely(inst->spline_changed_this_frame && frame > inst->prev_spline_change_frame && inst->prev_spline_change_frame)) {
			inst->prev_spline_deform = inst->spline_deform;
			inst->spline_changed_this_frame = false;
		}

		while (surf) {
			// A virtual surface is drawn by the NextGeo resolve (or NextGeo shadow
			// raster in active shadow views), not by the legacy opaque/motion/shadow lists.
			const bool surface_virtual = (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_VIRTUAL) != 0;
			const bool shadow_virtual_view = nextgeo_shadow_pass_active && p_render_list == RENDER_LIST_SECONDARY && (p_pass_mode == PASS_MODE_SHADOW || p_pass_mode == PASS_MODE_SHADOW_DP);
			// GodotNext: nextgeo_raster_bins (#155). A surface with a programmable raster bin draws on legacy
			// raster in a view without the hybrid raster and in atlas shadow views (until #217's VSM programmable
			// bins); its bucket entry and fixed-function shadow mask bit stay clear, so NextGeo skips it.
			const bool programmable_on_legacy = surface_virtual && surf->nextgeo_raster_bin != 0u && (!nextgeo_programmable_view_active || shadow_virtual_view);
			const bool skip_virtual = ((nextgeo_virtual_view && (p_render_list == RENDER_LIST_OPAQUE || p_render_list == RENDER_LIST_MOTION)) || shadow_virtual_view) && surface_virtual && !programmable_on_legacy;
#ifdef DEBUG_ENABLED
			if ((nextgeo_virtual_view && p_pass_mode == PASS_MODE_COLOR && p_render_list == RENDER_LIST_OPAQUE) || shadow_virtual_view) {
				if (surface_virtual && !skip_virtual && !programmable_on_legacy) {
					// The skip below must have caught it; an add here is a double dispatch.
					RSG::virtual_mesh_storage->nextgeo_count_double_dispatch();
				} else if (surface_virtual && skip_virtual && inst->nextgeo_record_index == UINT32_MAX) {
					RSG::virtual_mesh_storage->nextgeo_count_missing();
				} else if (!surface_virtual &&
						surf->nextgeo_fallback_reason == RendererRD::NEXTGEO_FALLBACK_NONE &&
						inst->data->base_type == RSE::INSTANCE_MESH &&
						inst->data->base.is_valid() &&
						RSG::virtual_mesh_storage->mesh_has_virtual_geometry(inst->data->base) &&
						RSG::virtual_mesh_storage->mesh_surface_is_virtual(inst->data->base, surf->surface_index)) {
					RSG::virtual_mesh_storage->nextgeo_count_missing();
				}
			}
#endif
			if (skip_virtual) {
				const uint32_t bslot = (inst->nextgeo_bucket_slot != UINT32_MAX) ? (inst->nextgeo_bucket_slot + surf->nextgeo_bucket_offset) : UINT32_MAX;
				// GodotNext: nextgeo_raster_bins (#155). The surface's programmable bin, and the material it binds.
				if (surf->nextgeo_raster_bin != 0u && nextgeo_virtual_view && p_render_list == RENDER_LIST_OPAQUE && surf->surface_index < 32 && surf->nextgeo_raster_bin < RendererVirtualMeshStorage::RASTER_BIN_LIMIT &&
						bslot != UINT32_MAX && RendererRD::nextgeo_raster_bin_table_word(bslot, surf->surface_index) < nextgeo_raster_bin_table.size()) {
					RendererRD::nextgeo_raster_bin_table_set(nextgeo_raster_bin_table.ptr(), bslot, surf->surface_index, surf->nextgeo_raster_bin);
					nextgeo_raster_bin_owners[surf->nextgeo_raster_bin].shader = surf->shader;
					nextgeo_raster_bin_owners[surf->nextgeo_raster_bin].material = surf->material;
					nextgeo_raster_bins_used |= uint64_t(1) << (surf->nextgeo_raster_bin - 1u);
				}
				if ((!nextgeo_visibility_only_active || nextgeo_programmable_view_active) && p_render_list == RENDER_LIST_OPAQUE && bslot < nextgeo_instance_data.size() && surf->surface_index < 32) {
					const uint32_t rec = bslot;
					SceneState::InstanceData &idata = nextgeo_instance_data[rec];
					if (inst->store_transform_cache) {
						RendererRD::MaterialStorage::store_transform_transposed_3x4(inst->transform, idata.transform);
						RendererRD::MaterialStorage::store_transform_transposed_3x4(inst->prev_transform, idata.prev_transform);
#ifdef REAL_T_IS_DOUBLE
						RendererRD::MaterialStorage::split_double(inst->transform.origin.x, &idata.transform[3], &idata.model_precision[0]);
						RendererRD::MaterialStorage::split_double(inst->transform.origin.y, &idata.transform[7], &idata.model_precision[1]);
						RendererRD::MaterialStorage::split_double(inst->transform.origin.z, &idata.transform[11], &idata.model_precision[2]);
						RendererRD::MaterialStorage::split_double(inst->prev_transform.origin.x, &idata.prev_transform[3], &idata.prev_model_precision[0]);
						RendererRD::MaterialStorage::split_double(inst->prev_transform.origin.y, &idata.prev_transform[7], &idata.prev_model_precision[1]);
						RendererRD::MaterialStorage::split_double(inst->prev_transform.origin.z, &idata.prev_transform[11], &idata.prev_model_precision[2]);
#endif
					} else {
						RendererRD::MaterialStorage::store_transform_transposed_3x4(Transform3D(), idata.transform);
						RendererRD::MaterialStorage::store_transform_transposed_3x4(Transform3D(), idata.prev_transform);
					}
					idata.flags = inst->flags_cache;
					idata.gi_offset = inst->gi_offset_cache;
					idata.layer_mask = inst->layer_mask;
					idata.instance_uniforms_ofs = uint32_t(inst->shader_uniforms_offset);
					idata.set_lightmap_uv_scale(inst->lightmap_uv_scale);
					idata.set_compressed_aabb(AABB(Vector3(0.0, 0.0, 0.0), Vector3(1.0, 1.0, 1.0)));
					// GodotNext: megalights_inputs (#136)
					idata.set_megalights_layer_group(megalights_inputs_active ? RendererRD::MegaLightsInputs::assign_layer_group(inst->layer_mask) : 0);
					idata.material_feedback_index = UINT32_MAX;
					idata.set_uv_scale(Vector4(0.0, 0.0, 0.0, 0.0));
					idata.spline_deform_data[0] = 0u;
					idata.spline_deform_data[1] = 0u;
					idata.spline_deform_data[2] = 0u;
					idata.spline_deform_data[3] = 0u;

					uint32_t cull_bits = 0u;
					if (surf->shader->cull_mode == RSE::CULL_MODE_DISABLED) {
						cull_bits |= NextGeoResolve::BUCKET_CULL_DISABLED;
					} else if (surf->shader->cull_mode == RSE::CULL_MODE_FRONT) {
						cull_bits |= NextGeoResolve::BUCKET_CULL_FRONT;
					}
					if (!nextgeo_visibility_only_active) {
						uint32_t material_feedback_index = UINT32_MAX;
#ifdef MODULE_TEXTURE_STREAMING_ENABLED
						if (surf->material->material_feedback_rid.is_valid() && TextureStreaming::get_singleton() != nullptr) {
							material_feedback_index = TextureStreaming::get_singleton()->feedback_buffer_material_index(surf->material->material_feedback_rid);
						}
#endif
						SceneShaderForwardClustered::ShaderSpecialization bucket_spec = {};
						bucket_spec.use_forward_gi = uses_gi;
						bucket_spec.use_light_soft_shadows = inst->using_softshadows;
						bucket_spec.use_light_projector = inst->using_projectors;
						bucket_spec.use_lightmap_specular = uses_lightmap_specular;
						bucket_spec.material_feedback = material_feedback_index != UINT32_MAX ? 1 : 0;

						const uint32_t bucket_id = nextgeo_resolve.register_bucket(surf->material, surf->shader, surf->material_uniform_set, material_feedback_index, bucket_spec);
						if (bucket_id != NextGeoResolve::BUCKET_INVALID && (rec * 32u + surf->surface_index) < nextgeo_bucket_table.size()) {
							nextgeo_bucket_table[rec * 32u + surf->surface_index] = (bucket_id & NextGeoResolve::BUCKET_ID_MASK) | cull_bits;
						}
					} else if ((rec * 32u + surf->surface_index) < nextgeo_bucket_table.size()) {
						nextgeo_bucket_table[rec * 32u + surf->surface_index] = cull_bits;
					}
					if (surf->shader->uses_time) {
						nextgeo_request_redraw = true;
					}
					if (uses_lightmap) {
						scene_state.used_lightmap = true;
					}
					if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_SUBSURFACE_SCATTERING) {
						scene_state.used_sss = true;
					}
					if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_NORMAL_TEXTURE) {
						scene_state.used_normal_texture = true;
					}
					if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_DEPTH_TEXTURE) {
						scene_state.used_depth_texture = true;
					}
				}
				surf = surf->next;
				continue;
			}
			// GodotNext: nextgeo_foliage_instancing (#151). Assembly part surfaces carry part-local meshes
			// only for NextGeo bucket/bin registration and must not draw stacked at the container origin.
			if (surf->nextgeo_assembly_part) {
				surf = surf->next;
				continue;
			}
			surf->sort.uses_forward_gi = 0;
			surf->sort.uses_lightmap = 0;
			surf->uses_lightmap_specular = false;
			surf->uses_prepass_velocity = false;

			// LOD
			void *active_surface = surf->surface;
			const bool using_proxy_surface = (nextgeo_rt_proxy_active && surf->surface_rt_proxy != nullptr) || (nextgeo_proxy_active && surf->surface_proxy != nullptr);
			if (nextgeo_rt_proxy_active && surf->surface_rt_proxy != nullptr) {
				active_surface = surf->surface_rt_proxy;
			} else if (nextgeo_proxy_active && surf->surface_proxy != nullptr) {
				active_surface = surf->surface_proxy;
			}
			if (!using_proxy_surface && p_render_data->scene_data->screen_mesh_lod_threshold > 0.0 && mesh_storage->mesh_surface_has_lod(active_surface)) {
				uint32_t indices = 0;
				surf->sort.lod_index = mesh_storage->mesh_surface_get_lod(active_surface, inst->lod_model_scale * inst->lod_bias, lod_distance * p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, indices);
				if (p_render_data->render_info) {
					indices = _indices_to_primitives(surf->primitive, indices);
					if (p_render_list == RENDER_LIST_OPAQUE) { //opaque
						p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += indices;
					} else if (p_render_list == RENDER_LIST_SECONDARY) { //shadow
						p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_SHADOW][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += indices;
					}
				}
			} else {
				surf->sort.lod_index = 0;
				if (p_render_data->render_info) {
					// This does not include primitives rendered via indirect draw calls.
					uint32_t to_draw = mesh_storage->mesh_surface_get_vertices_drawn_count(active_surface);
					to_draw = _indices_to_primitives(surf->primitive, to_draw);
					to_draw *= inst->instance_count;
					if (p_render_list == RENDER_LIST_OPAQUE) { //opaque
						p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += to_draw;
					} else if (p_render_list == RENDER_LIST_SECONDARY) { //shadow
						p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_SHADOW][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += to_draw;
					}
				}
			}

			if (p_nextgi_velocity && (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH) && ((inst->flags_cache & INSTANCE_DATA_FLAGS_DYNAMIC) || (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_MOTION_VECTOR))) {
				surf->uses_prepass_velocity = true;
			}

			// ADD Element
			if (p_pass_mode == PASS_MODE_COLOR) {
#ifdef DEBUG_ENABLED
				bool force_alpha = unlikely(get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW);
#else
				bool force_alpha = false;
#endif

				if (fade_alpha < FADE_ALPHA_PASS_THRESHOLD) {
					force_alpha = true;
				}

				if (!force_alpha && (surf->flags & (GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH | GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE))) {
					rl->add_element(surf);
				}

				if (force_alpha || (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA)) {
					surf->color_pass_inclusion_mask = COLOR_PASS_FLAG_TRANSPARENT;
					render_list[RENDER_LIST_ALPHA].add_element(surf);
					if (uses_gi) {
						surf->sort.uses_forward_gi = 1;
					}
				} else if (p_using_motion_pass && (uses_motion || (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_MOTION_VECTOR)) && !surf->uses_prepass_velocity) {
					surf->color_pass_inclusion_mask = COLOR_PASS_FLAG_MOTION_VECTORS;
					render_list[RENDER_LIST_MOTION].add_element(surf);
				} else {
					surf->color_pass_inclusion_mask = 0;
				}

				if (uses_lightmap) {
					surf->sort.uses_lightmap = 1;
					surf->uses_lightmap_specular = uses_lightmap_specular;
					scene_state.used_lightmap = true;
				}

				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_SUBSURFACE_SCATTERING) {
					scene_state.used_sss = true;
				}
				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_SCREEN_TEXTURE) {
					scene_state.used_screen_texture = true;
				}
				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_NORMAL_TEXTURE) {
					scene_state.used_normal_texture = true;
				}
				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_DEPTH_TEXTURE) {
					scene_state.used_depth_texture = true;
				}
				if ((surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_STENCIL) && !force_alpha && (surf->flags & (GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH | GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE))) {
					scene_state.used_opaque_stencil = true;
				}
			} else if (p_pass_mode == PASS_MODE_SHADOW || p_pass_mode == PASS_MODE_SHADOW_DP) {
				if (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW) {
					rl->add_element(surf);
				}
			} else if (p_pass_mode == PASS_MODE_DEPTH_MATERIAL) {
				if (surf->flags & (GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH | GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE | GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA)) {
					rl->add_element(surf);
				}
			} else {
				if (surf->flags & (GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH | GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE)) {
					rl->add_element(surf);
				}
			}

			surf->sort.depth_layer = depth_layer;

			surf = surf->next;
		}
	}

	if (p_render_list == RENDER_LIST_OPAQUE && lightmap_captures_used) {
		RD::get_singleton()->buffer_update(scene_state.lightmap_capture_buffer, 0, sizeof(LightmapCaptureData) * lightmap_captures_used, scene_state.lightmap_captures);
	}
	if (nextgeo_request_redraw) {
		RenderingServerDefault::redraw_request();
	}
}

void RenderForwardClustered::_setup_voxelgis(const PagedArray<RID> &p_voxelgis) {
	scene_state.voxelgis_used = MIN(p_voxelgis.size(), uint32_t(MAX_VOXEL_GI_INSTANCESS));
	for (uint32_t i = 0; i < scene_state.voxelgis_used; i++) {
		scene_state.voxelgi_ids[i] = p_voxelgis[i];
	}
}

void RenderForwardClustered::_setup_lightmaps(const RenderDataRD *p_render_data, const PagedArray<RID> &p_lightmaps, const Transform3D &p_cam_transform) {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	scene_state.lightmaps_used = 0;
	for (int i = 0; i < (int)p_lightmaps.size(); i++) {
		if (i >= (int)scene_state.max_lightmaps) {
			break;
		}

		RID lightmap = light_storage->lightmap_instance_get_lightmap(p_lightmaps[i]);

		// Transform (for directional lightmaps).
		Basis to_lm = light_storage->lightmap_instance_get_transform(p_lightmaps[i]).basis.inverse() * p_cam_transform.basis;
		to_lm = to_lm.inverse().transposed(); //will transform normals
		RendererRD::MaterialStorage::store_transform_3x3(to_lm, scene_state.lightmaps[i].normal_xform_and_specular_intensity);

		// Light texture size.
		Vector2i lightmap_size = light_storage->lightmap_get_light_texture_size(lightmap);
		scene_state.lightmaps[i].texture_size[0] = lightmap_size[0];
		scene_state.lightmaps[i].texture_size[1] = lightmap_size[1];

		// Exposure.
		scene_state.lightmaps[i].exposure_normalization = 1.0;
		scene_state.lightmaps[i].flags = light_storage->lightmap_get_shadowmask_mode(lightmap);
		if (p_render_data->camera_attributes.is_valid()) {
			float baked_exposure = light_storage->lightmap_get_baked_exposure_normalization(lightmap);
			float enf = RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
			scene_state.lightmaps[i].exposure_normalization = enf / baked_exposure;
		}

		scene_state.lightmap_ids[i] = p_lightmaps[i];
		scene_state.lightmap_has_sh[i] = light_storage->lightmap_uses_spherical_harmonics(lightmap);

		float &specular_intensity = scene_state.lightmaps[i].normal_xform_and_specular_intensity[3];
		specular_intensity = light_storage->lightmap_get_specular_intensity(lightmap);
		scene_state.lightmap_has_specular[i] = scene_state.lightmap_has_sh[i] && (specular_intensity > 0.0f);

		scene_state.lightmaps_used++;
	}
	if (scene_state.lightmaps_used > 0) {
		RD::get_singleton()->buffer_update(scene_state.lightmap_buffer, 0, sizeof(LightmapData) * scene_state.lightmaps_used, scene_state.lightmaps);
	}
}

/* SDFGI */

void RenderForwardClustered::_update_sdfgi(RenderDataRD *p_render_data) {
	if (p_render_data->sdfgi_update_data == nullptr) {
		return;
	}

	Ref<RenderSceneBuffersRD> rb;
	if (p_render_data && p_render_data->render_buffers.is_valid()) {
		rb = p_render_data->render_buffers;
	}

	if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_SDFGI)) {
		RENDER_TIMESTAMP("Render SDFGI");
		Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
		float exposure_normalization = 1.0;

		if (p_render_data->camera_attributes.is_valid()) {
			exposure_normalization = RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
		}
		for (int i = 0; i < p_render_data->render_sdfgi_region_count; i++) {
			sdfgi->render_region(rb, p_render_data->render_sdfgi_regions[i].region, p_render_data->render_sdfgi_regions[i].instances, exposure_normalization);
		}
		if (p_render_data->sdfgi_update_data->update_static) {
			sdfgi->render_static_lights(p_render_data, rb, p_render_data->sdfgi_update_data->static_cascade_count, p_render_data->sdfgi_update_data->static_cascade_indices, p_render_data->sdfgi_update_data->static_positional_lights);
		}
	}
}

/* Debug */

void RenderForwardClustered::_debug_draw_cluster(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (p_render_buffers.is_valid() && current_cluster_builder != nullptr) {
		RSE::ViewportDebugDraw dd = get_debug_draw_mode();

		if (dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_OMNI_LIGHTS || dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_SPOT_LIGHTS || dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_AREA_LIGHTS || dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_DECALS || dd == RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_REFLECTION_PROBES) {
			ClusterBuilderRD::ElementType elem_type = ClusterBuilderRD::ELEMENT_TYPE_MAX;
			switch (dd) {
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_OMNI_LIGHTS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_OMNI_LIGHT;
					break;
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_SPOT_LIGHTS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_SPOT_LIGHT;
					break;
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_AREA_LIGHTS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_AREA_LIGHT;
					break;
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_DECALS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_DECAL;
					break;
				case RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_REFLECTION_PROBES:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_REFLECTION_PROBE;
					break;
				default: {
				}
			}
			current_cluster_builder->debug(elem_type);
		}
	}
}

////////////////////////////////////////////////////////////////////////////////
// FOG SHADER

void RenderForwardClustered::_update_volumetric_fog(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const Projection &p_cam_projection, const Transform3D &p_cam_transform, const Transform3D &p_prev_cam_inv_transform, RID p_shadow_atlas, int p_directional_light_count, bool p_use_directional_shadows, int p_positional_light_count, int p_voxel_gi_count, const PagedArray<RID> &p_fog_volumes) {
	ERR_FAIL_COND(p_render_buffers.is_null());

	Ref<RenderBufferDataForwardClustered> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	ERR_FAIL_COND(!p_render_buffers->has_custom_data(RB_SCOPE_GI));
	Ref<RendererRD::GI::RenderBuffersGI> rbgi = p_render_buffers->get_custom_data(RB_SCOPE_GI);

	Ref<RendererRD::GI::SDFGI> sdfgi;
	if (p_render_buffers->has_custom_data(RB_SCOPE_SDFGI)) {
		sdfgi = p_render_buffers->get_custom_data(RB_SCOPE_SDFGI);
	}

	Size2i size = p_render_buffers->get_internal_size();
	float ratio = float(size.x) / float((size.x + size.y) / 2);
	uint32_t target_width = uint32_t(float(get_volumetric_fog_size()) * ratio);
	uint32_t target_height = uint32_t(float(get_volumetric_fog_size()) / ratio);

	if (p_render_buffers->has_custom_data(RB_SCOPE_FOG)) {
		Ref<RendererRD::Fog::VolumetricFog> fog = p_render_buffers->get_custom_data(RB_SCOPE_FOG);
		//validate
		if (p_environment.is_null() || !environment_get_volumetric_fog_enabled(p_environment) || fog->width != target_width || fog->height != target_height || fog->depth != get_volumetric_fog_depth()) {
			p_render_buffers->set_custom_data(RB_SCOPE_FOG, Ref<RenderBufferCustomDataRD>());
		}
	}

	if (p_environment.is_null() || !environment_get_volumetric_fog_enabled(p_environment)) {
		//no reason to enable or update, bye
		return;
	}

	if (p_environment.is_valid() && environment_get_volumetric_fog_enabled(p_environment) && !p_render_buffers->has_custom_data(RB_SCOPE_FOG)) {
		//required volumetric fog but not existing, create
		Ref<RendererRD::Fog::VolumetricFog> fog;

		fog.instantiate();
		fog->init(Vector3i(target_width, target_height, get_volumetric_fog_depth()), sky.sky_shader.default_shader_rd);

		p_render_buffers->set_custom_data(RB_SCOPE_FOG, fog);
	}

	if (p_render_buffers->has_custom_data(RB_SCOPE_FOG)) {
		Ref<RendererRD::Fog::VolumetricFog> fog = p_render_buffers->get_custom_data(RB_SCOPE_FOG);

		RendererRD::Fog::VolumetricFogSettings settings;
		settings.rb_size = size;
		settings.time = time;
		settings.is_using_radiance_octmap_array = is_using_radiance_octmap_array();
		settings.max_cluster_elements = RendererRD::LightStorage::get_singleton()->get_max_cluster_elements();
		settings.volumetric_fog_filter_active = get_volumetric_fog_filter_active();

		settings.shadow_sampler = shadow_sampler;
		settings.shadow_atlas_depth = RendererRD::LightStorage::get_singleton()->owns_shadow_atlas(p_shadow_atlas) ? RendererRD::LightStorage::get_singleton()->shadow_atlas_get_texture(p_shadow_atlas) : RID();
		settings.voxel_gi_buffer = rbgi->get_voxel_gi_buffer();
		settings.omni_light_buffer = RendererRD::LightStorage::get_singleton()->get_omni_light_buffer();
		settings.spot_light_buffer = RendererRD::LightStorage::get_singleton()->get_spot_light_buffer();
		settings.area_light_buffer = RendererRD::LightStorage::get_singleton()->get_area_light_buffer();
		settings.area_light_atlas = RendererRD::TextureStorage::get_singleton()->area_light_atlas_get_texture();
		settings.directional_shadow_depth = RendererRD::LightStorage::get_singleton()->directional_shadow_get_texture();
		settings.directional_light_buffer = RendererRD::LightStorage::get_singleton()->get_directional_light_buffer();

		settings.vfog = fog;
		settings.cluster_builder = rb_data->cluster_builder;
		settings.rbgi = rbgi;
		settings.sdfgi = sdfgi;
		RendererRD::NextGIWRC *wrc = next_gi.get_wrc(p_render_buffers.ptr());
		// GodotNext: ps_swrt_trace (#186, plan §21.2 L9b). Fog reads the WRC volume on both active tiers.
		const RendererRD::NextGI::Tier fog_tier = next_gi.get_tier_result().tier;
		settings.use_nextgi = environment_get_nextgi_enabled(p_environment) && (fog_tier == RendererRD::NextGI::TIER_HWRT || fog_tier == RendererRD::NextGI::TIER_SOFTWARE) && wrc != nullptr && wrc->is_ready();
		if (settings.use_nextgi) {
			settings.nextgi_sh_r = wrc->get_translucency_r();
			settings.nextgi_sh_g = wrc->get_translucency_g();
			settings.nextgi_sh_b = wrc->get_translucency_b();
			settings.nextgi_params = wrc->get_params_buffer();
			settings.nextgi_counters = wrc->get_counter_buffer();
		}
		settings.env = p_environment;
		settings.sky = &sky;
		settings.gi = &gi;

		// GodotNext: vsm_fog_translucency (#215). Pass VSM coarse-page buffers when this view samples VSM,
		// or safe zeroed fallback resources when VSM is off so legacy fog stays byte-identical.
		_ensure_vsm_projection_resources();
		RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
		const bool vsm_ready = vsm_sample_view && vsm != nullptr && vsm->is_active() &&
				vsm->get_table_capacity() > 0 && vsm->get_map_buffers_capacity() == vsm->get_table_capacity() &&
				vsm->get_pool_texture().is_valid() && vsm->get_page_table_buffer().is_valid() &&
				vsm->get_map_data_buffer().is_valid() && vsm->get_coarse_miss_buffer().is_valid();
		settings.vsm_pool_tex = vsm_ready ? vsm->get_pool_texture() : vsm_projection_default_pool;
		settings.vsm_page_table = vsm_ready ? vsm->get_page_table_buffer() : vsm_projection_default_buffer;
		settings.vsm_map_data = vsm_ready ? vsm->get_map_data_buffer() : vsm_projection_default_buffer;
		settings.vsm_coarse_miss = vsm_ready ? vsm->get_coarse_miss_buffer() : vsm_projection_default_miss_buffer;
		settings.vsm_table_maps = vsm_ready ? vsm->get_table_capacity() : 0u;
		// GodotNext: light_source_parity (#140). IES profile atlas for volumetric fog light evaluation.
		settings.ies_profile_atlas = RendererRD::LightStorage::get_singleton()->get_ies_atlas_texture();

		// GodotNext: light_function_atlas (#138, plan §21.3 contract M8).
		settings.light_function_atlas = light_function_atlas.get_atlas_texture_or_default();
		settings.light_function_buffer = light_function_atlas.get_view_entry_buffer_or_default();
		settings.light_function_count = light_function_atlas.has_baked_atlas() ? light_function_atlas.get_view_entry_count() : 0u;

		RendererRD::Fog::get_singleton()->volumetric_fog_update(settings, p_cam_projection, p_cam_transform, p_prev_cam_inv_transform, p_shadow_atlas, p_directional_light_count, p_use_directional_shadows, p_positional_light_count, p_voxel_gi_count, p_fog_volumes);
	}
}

/* Lighting */

void RenderForwardClustered::setup_added_reflection_probe(const Transform3D &p_transform, const Vector3 &p_half_size) {
	if (current_cluster_builder != nullptr) {
		current_cluster_builder->add_box(ClusterBuilderRD::BOX_TYPE_REFLECTION_PROBE, p_transform, p_half_size);
	}
}

void RenderForwardClustered::setup_added_light(const RSE::LightType p_type, const Transform3D &p_transform, float p_radius, float p_spot_aperture, const Vector2 &p_area_size, uint32_t p_per_type_index, float p_barn_door_angle_deg, float p_barn_door_length, float p_intensity) {
	if (current_cluster_builder != nullptr) {
		ClusterBuilderRD::LightType type;
		if (p_type == RSE::LIGHT_SPOT) {
			type = ClusterBuilderRD::LIGHT_TYPE_SPOT;
		} else if (p_type == RSE::LIGHT_OMNI) {
			type = ClusterBuilderRD::LIGHT_TYPE_OMNI;
		} else {
			type = ClusterBuilderRD::LIGHT_TYPE_AREA;
		}

		// GodotNext: light_grid (#139). Route lights into the packed grid when active; legacy bitmask stays untouched otherwise.
		if (current_cluster_builder->is_packed_grid_requested()) {
			current_cluster_builder->add_packed_light(type, p_per_type_index, p_transform, p_radius, p_spot_aperture, p_area_size, p_barn_door_angle_deg, p_barn_door_length, p_intensity, false);
		} else {
			current_cluster_builder->add_light(type, p_transform, p_radius, p_spot_aperture, p_area_size);
		}
	}
}

void RenderForwardClustered::setup_added_decal(const Transform3D &p_transform, const Vector3 &p_half_size) {
	if (current_cluster_builder != nullptr) {
		current_cluster_builder->add_box(ClusterBuilderRD::BOX_TYPE_DECAL, p_transform, p_half_size);
	}
}

/* Render scene */

void RenderForwardClustered::_process_ssao(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const RID *p_normal_buffers, const Projection *p_projections) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_environment.is_null());

	Ref<RenderBufferDataForwardClustered> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	RENDER_TIMESTAMP("Process SSAO");

	RendererRD::SSEffects::SSAOSettings settings;
	settings.radius = environment_get_ssao_radius(p_environment);
	settings.intensity = environment_get_ssao_intensity(p_environment);
	settings.power = environment_get_ssao_power(p_environment);
	settings.detail = environment_get_ssao_detail(p_environment);
	settings.horizon = environment_get_ssao_horizon(p_environment);
	settings.sharpness = environment_get_ssao_sharpness(p_environment);
	settings.full_screen_size = p_render_buffers->get_internal_size();

	ss_effects->ssao_allocate_buffers(p_render_buffers, rb_data->ss_effects_data.ssao, settings);

	for (uint32_t v = 0; v < p_render_buffers->get_view_count(); v++) {
		ss_effects->generate_ssao(p_render_buffers, rb_data->ss_effects_data.ssao, v, p_normal_buffers[v], p_projections[v], settings);
	}
}

void RenderForwardClustered::_process_ssil(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const RID *p_normal_buffers, const Projection *p_projections, const Transform3D &p_transform) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_environment.is_null());

	Ref<RenderBufferDataForwardClustered> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	RENDER_TIMESTAMP("Process SSIL");

	RendererRD::SSEffects::SSILSettings settings;
	settings.radius = environment_get_ssil_radius(p_environment);
	settings.intensity = environment_get_ssil_intensity(p_environment);
	settings.sharpness = environment_get_ssil_sharpness(p_environment);
	settings.normal_rejection = environment_get_ssil_normal_rejection(p_environment);
	settings.full_screen_size = p_render_buffers->get_internal_size();

	ss_effects->ssil_allocate_buffers(p_render_buffers, rb_data->ss_effects_data.ssil, settings);

	Transform3D transform = p_transform;
	transform.set_origin(Vector3(0.0, 0.0, 0.0));

	for (uint32_t v = 0; v < p_render_buffers->get_view_count(); v++) {
		Projection correction;
		correction.set_depth_correction(true);
		Projection projection = correction * p_projections[v];
		Projection last_frame_projection = rb_data->ss_effects_data.ssil_last_frame_projections[v] * Projection(rb_data->ss_effects_data.ssil_last_frame_transform.affine_inverse()) * Projection(transform) * projection.inverse();

		ss_effects->screen_space_indirect_lighting(p_render_buffers, rb_data->ss_effects_data.ssil, v, p_normal_buffers[v], p_projections[v], last_frame_projection, settings);

		rb_data->ss_effects_data.ssil_last_frame_projections[v] = projection;
	}
	rb_data->ss_effects_data.ssil_last_frame_transform = transform;
}

void RenderForwardClustered::_process_ssr(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_environment, const RID *p_normal_slices, const Projection *p_projections, const Vector3 *p_eye_offsets, const Transform3D &p_transform) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_render_buffers.is_null());

	Ref<RenderBufferDataForwardClustered> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	RENDER_TIMESTAMP("Process SSR");

	ss_effects->ssr_allocate_buffers(p_render_buffers, rb_data->ss_effects_data.ssr, p_render_buffers->get_base_data_format());

	Projection reprojections[RendererSceneRender::MAX_RENDER_VIEWS];

	for (uint32_t v = 0; v < p_render_buffers->get_view_count(); v++) {
		Projection correction;
		correction.set_depth_correction(true);

		Projection projection = correction * p_projections[v];

		reprojections[v] = rb_data->ss_effects_data.ssr_last_frame_projections[v] * Projection(rb_data->ss_effects_data.ssr_last_frame_transform.affine_inverse()) * Projection(p_transform) * projection.inverse();

		rb_data->ss_effects_data.ssr_last_frame_projections[v] = projection;
	}
	rb_data->ss_effects_data.ssr_last_frame_transform = p_transform;

	ss_effects->screen_space_reflection(p_render_buffers, rb_data->ss_effects_data.ssr, p_normal_slices, environment_get_ssr_max_steps(p_environment), environment_get_ssr_fade_in(p_environment), environment_get_ssr_fade_out(p_environment), environment_get_ssr_depth_tolerance(p_environment), p_projections, reprojections, p_eye_offsets, *copy_effects);
}

void RenderForwardClustered::_process_sscs(Ref<RenderSceneBuffersRD> p_render_buffers, const Projection *p_projections, const Transform3D &p_transform, const LocalVector<RenderDataRD::ContactShadowLight> &p_contact_shadow_lights, const float p_taa_frame_count) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_render_buffers.is_null());

	Ref<RenderBufferDataForwardClustered> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	RENDER_TIMESTAMP("Process SSCS");

	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	const int project_length_enum = GLOBAL_GET_CACHED(int, "rendering/lights_and_shadows/contact_shadow/shadow_length");
	const float surface_thickness = GLOBAL_GET_CACHED(float, "rendering/lights_and_shadows/contact_shadow/surface_thickness");
	const int screen_height = p_render_buffers->get_internal_size().height;

	Transform3D inverse_transform = p_transform.affine_inverse();

	ss_effects->sscs_allocate_buffers(p_render_buffers, rb_data->ss_effects_data.sscs, p_contact_shadow_lights.size());

	for (uint32_t i = 0; i < p_contact_shadow_lights.size(); i++) {
		const RenderDataRD::ContactShadowLight &entry = p_contact_shadow_lights[i];
		Transform3D light_transform = light_storage->light_instance_get_base_transform(entry.light_instance);
		const bool is_directional = entry.type == RSE::LIGHT_DIRECTIONAL;
		Vector3 light_coord = is_directional
				? inverse_transform.basis.xform(light_transform.basis.xform(Vector3(0, 0, 1))).normalized()
				: inverse_transform.xform(light_transform.origin);

		RendererRD::SSEffects::SSCSSettings settings;
		settings.surface_thickness = surface_thickness;
		settings.contact_shadow_length = entry.length;
		settings.world_space = entry.world_space;
		settings.quality = RendererRD::LightSourceParity::select_contact_shadow_quality(entry.length, entry.world_space, screen_height, project_length_enum);

		ss_effects->screen_space_contact_shadows(p_render_buffers, rb_data->ss_effects_data.sscs, settings, p_projections, light_coord, is_directional, i, entry.blur, p_taa_frame_count);
	}
}

void RenderForwardClustered::_copy_framebuffer_to_ss_effects(Ref<RenderSceneBuffersRD> p_render_buffers, bool p_use_ssil, bool p_use_ssr) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_render_buffers.is_null());

	ss_effects->copy_internal_texture_to_last_frame(p_render_buffers, *copy_effects);
}

// GodotNext: nextgi_frame_block
void RenderForwardClustered::_pre_opaque_render(RenderDataRD *p_render_data, bool p_use_ssao, bool p_use_ssil, bool p_use_ssr, bool p_use_sscs, bool p_use_gi, const RID *p_normal_roughness_slices, const RID *p_voxel_gi_buffer_slices, bool p_use_nextgi) {
	// Render shadows while GI is rendering, due to how barriers are handled, this should happen at the same time
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	Ref<RenderBufferDataForwardClustered> rb_data;
	if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		// Our forward clustered custom data buffer will only be available when we're rendering our normal view.
		// This will not be available when rendering reflection probes.
		rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	}

	RENDER_TIMESTAMP("Setup Shadows");

	if (rb.is_valid() && p_use_gi && rb->has_custom_data(RB_SCOPE_SDFGI)) {
		Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
		sdfgi->store_probes();
	}

	Size2i viewport_size = Size2i(1, 1);
	if (rb.is_valid()) {
		viewport_size = rb->get_internal_size();
	}

	p_render_data->cube_shadows.clear();
	p_render_data->shadows.clear();
	p_render_data->directional_shadows.clear();
	p_render_data->contact_shadows.clear();

	float lod_distance_multiplier = p_render_data->scene_data->cam_projection.get_lod_multiplier();
	LocalVector<int32_t> render_shadow_to_view;
	render_shadow_to_view.resize(p_render_data->render_shadow_count);
	for (int i = 0; i < p_render_data->render_shadow_count; i++) {
		render_shadow_to_view[i] = -1;
	}
	// GodotNext: vsm_projection (#222). The view's shadow-atlas passes, kept and skipped (debug info).
	uint32_t vsm_atlas_passes = 0;
	uint32_t vsm_atlas_passes_skipped = 0;
	{
		for (int i = 0; i < p_render_data->render_shadow_count; i++) {
			RID li = p_render_data->render_shadows[i].light;
			RID base = light_storage->light_instance_get_base_light(li);
			// GodotNext: vsm_projection (#222, issue step 7) / df_shadows (#214) / rt_shadows (#141). A light this view samples
			// through virtual shadow maps, distance-field shadows, or standalone HWRT ray-traced shadows gets no atlas pass.
			// Contact shadows keep their entry.
			const bool atlas_pass = !(vsm_sample_view && vsm_light_skips_atlas(li, p_render_data->environment)) &&
					!(df_shadows_view && df_light_skips_atlas(li, p_render_data->environment)) &&
					!(rt_shadows_view && rt_light_skips_atlas(li, p_render_data->environment, p_render_data->render_buffers));
			if (atlas_pass) {
				vsm_atlas_passes++;
			} else {
				vsm_atlas_passes_skipped++;
			}

			if (light_storage->light_get_type(base) == RSE::LIGHT_DIRECTIONAL) {
				if (atlas_pass) {
					p_render_data->directional_shadows.push_back(i);
				}

				if (rb_data.is_valid() && ss_effects) {
					// Add contact shadows to be processed
					if (p_render_data->render_shadows[i].pass == 0 && light_storage->light_get_allow_contact_shadows(base)) {
						// Contact shadows only need one pass
						p_render_data->contact_shadows.push_back(i);
					}
				}

			} else if (light_storage->light_get_type(base) == RSE::LIGHT_OMNI && light_storage->light_omni_get_shadow_mode(base) == RSE::LIGHT_OMNI_SHADOW_CUBE) {
				if (atlas_pass) {
					p_render_data->cube_shadows.push_back(i);
				}
			} else if (atlas_pass) {
				p_render_data->shadows.push_back(i);
			}
		}

		// GodotNext: nextgeo_shadow_cuts (#82). Dispatch multi-view shadow BVH cuts
		// before cube_shadows run (cube finalization overwrites shadow_transform).
		if (nextgeo_virtual_active &&
				!p_render_data->reflection_probe.is_valid() &&
				RSG::virtual_mesh_storage != nullptr &&
				RSG::virtual_mesh_storage->get_live_record_count() > 0 &&
				RSG::virtual_mesh_storage->get_record_count() > 0 &&
				RSG::virtual_mesh_storage->get_record_buffer().is_valid() &&
				RSG::virtual_mesh_storage->get_max_bvh_levels() > 0 &&
				(p_render_data->cube_shadows.size() > 0 || p_render_data->directional_shadows.size() > 0 || p_render_data->shadows.size() > 0)) {
			if (p_render_data->directional_shadows.size() > 0) {
				light_storage->update_directional_shadow_atlas();
			}
			if (p_render_data->shadow_atlas.is_valid() && light_storage->owns_shadow_atlas(p_render_data->shadow_atlas)) {
				light_storage->shadow_atlas_update(p_render_data->shadow_atlas);
			}

			const uint32_t camera_layers = p_render_data->scene_data != nullptr ? p_render_data->scene_data->camera_visible_layers : 0xFFFFFFFFu;
			LocalVector<NextGeoShadowViewDesc> shadow_views;
			shadow_views.reserve(MIN(uint32_t(p_render_data->render_shadow_count), NextGeoCut::MAX_SHADOW_VIEWS));

			auto append_shadow_view = [&](int p_shadow_index) {
				if (shadow_views.size() >= NextGeoCut::MAX_SHADOW_VIEWS) {
					return;
				}
				const RID li = p_render_data->render_shadows[p_shadow_index].light;
				const int pass = p_render_data->render_shadows[p_shadow_index].pass;
				if (!light_storage->owns_light_instance(li)) {
					return;
				}
				const RID base = light_storage->light_instance_get_base_light(li);
				const RSE::LightType ltype = light_storage->light_get_type(base);
				const bool rev_cull = light_storage->light_get_reverse_cull_face_mode(base);

				NextGeoShadowViewDesc vd;
				vd.max_texel_error = 1.0f;
				vd.lod_bias = 1.0f;
				vd.cull_mask = camera_layers & light_storage->light_get_shadow_caster_mask(base);
				vd.light_type = int32_t(ltype);
				vd.pass = pass;

				if (ltype == RSE::LIGHT_DIRECTIONAL) {
					const uint64_t last_scene_shadow_pass = light_storage->light_instance_get_shadow_pass(li);
					if (last_scene_shadow_pass != get_scene_pass()) {
						light_storage->light_instance_set_directional_rect(li, light_storage->get_directional_shadow_rect());
						light_storage->directional_shadow_increase_current_light();
						light_storage->light_instance_set_shadow_pass(li, get_scene_pass());
					}

					Rect2i atlas_rect = light_storage->light_instance_get_directional_rect(li);
					const RSE::LightDirectionalShadowMode dir_mode = light_storage->light_directional_get_shadow_mode(base);
					if (dir_mode == RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_4_SPLITS) {
						atlas_rect.size.width /= 2;
						atlas_rect.size.height /= 2;
					} else if (dir_mode == RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS) {
						atlas_rect.size.height /= 2;
					}
					if (atlas_rect.size.width <= 0 || atlas_rect.size.height <= 0) {
						return;
					}

					vd.use_pancake = light_storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_PANCAKE_SIZE) > 0;
					vd.projection = light_storage->light_instance_get_shadow_camera(li, pass);
					vd.transform = light_storage->light_instance_get_shadow_transform(li, pass);
					vd.light_position = vd.transform.origin;
					vd.tile_size = atlas_rect.size;
					vd.z_near = 0.001f;
					vd.z_far = light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
					vd.cut_flags = RendererRD::NEXTGEO_CUT_FLAG_SHADOW_VIEW | RendererRD::NEXTGEO_CUT_FLAG_ORTHOGRAPHIC;
					vd.dual_paraboloid = false;
					vd.dual_paraboloid_flip = false;
					vd.flip_y = true;
					vd.flip_cull = (vd.dual_paraboloid_flip != vd.flip_y) != rev_cull;

					Vector<Plane> planes = vd.projection.get_projection_planes(vd.transform);
					if (planes.size() < 6) {
						return;
					}
					if (vd.use_pancake) {
						const Vector3 z_vec = vd.transform.basis.get_column(Vector3::AXIS_Z).normalized();
						planes.write[Projection::PLANE_NEAR] = Plane(z_vec, vd.transform.origin.dot(z_vec) + 1e6f);
					}
					for (int p = 0; p < 6; p++) {
						vd.frustum[p] = RendererRD::nextgeo_plane_to_frustum(planes[p]);
					}
				} else {
					if (!light_storage->owns_shadow_atlas(p_render_data->shadow_atlas) ||
							!light_storage->shadow_atlas_owns_light_instance(p_render_data->shadow_atlas, li)) {
						return;
					}
					const uint32_t key = light_storage->shadow_atlas_get_light_instance_key(p_render_data->shadow_atlas, li);
					const uint32_t quadrant = (key >> RendererRD::LightStorage::QUADRANT_SHIFT) & 0x3;
					const uint32_t shadow = key & RendererRD::LightStorage::SHADOW_INDEX_MASK;
					const uint32_t subdivision = light_storage->shadow_atlas_get_quadrant_subdivision(p_render_data->shadow_atlas, quadrant);
					if (subdivision == 0 || int(shadow) >= light_storage->shadow_atlas_get_quadrant_shadow_size(p_render_data->shadow_atlas, quadrant)) {
						return;
					}
					const uint32_t shadow_atlas_size = light_storage->shadow_atlas_get_size(p_render_data->shadow_atlas);
					const uint32_t quadrant_size = shadow_atlas_size >> 1;
					const uint32_t shadow_size = quadrant_size / subdivision;
					if (shadow_size == 0) {
						return;
					}
					const float range = light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE);

					if (ltype == RSE::LIGHT_OMNI && light_storage->light_omni_get_shadow_mode(base) == RSE::LIGHT_OMNI_SHADOW_CUBE) {
						const int face_size = MAX(int(shadow_size / 2), 1);
						vd.projection = light_storage->light_instance_get_shadow_camera(li, pass);
						vd.transform = light_storage->light_instance_get_shadow_transform(li, pass);
						vd.light_position = vd.transform.origin;
						vd.tile_size = Size2i(face_size, face_size);
						vd.z_near = MAX(0.001f, float(vd.projection.get_z_near()));
						vd.z_far = range;
						vd.cut_flags = RendererRD::NEXTGEO_CUT_FLAG_SHADOW_VIEW;
						vd.dual_paraboloid = false;
						vd.dual_paraboloid_flip = false;
						vd.flip_y = false;
						vd.flip_cull = (vd.dual_paraboloid_flip != vd.flip_y) != rev_cull;

						const Vector<Plane> planes = vd.projection.get_projection_planes(vd.transform);
						if (planes.size() < 6) {
							return;
						}
						for (int p = 0; p < 6; p++) {
							vd.frustum[p] = RendererRD::nextgeo_plane_to_frustum(planes[p]);
						}
					} else if (ltype == RSE::LIGHT_OMNI) {
						const int dp_tile = int(shadow_size) - 2;
						if (dp_tile <= 0) {
							return;
						}
						vd.projection = light_storage->light_instance_get_shadow_camera(li, 0);
						vd.transform = light_storage->light_instance_get_shadow_transform(li, 0);
						vd.light_position = vd.transform.origin;
						vd.tile_size = Size2i(dp_tile, dp_tile);
						vd.z_near = MAX(0.001f, MIN(0.025f, range));
						vd.z_far = range;
						vd.cut_flags = RendererRD::NEXTGEO_CUT_FLAG_SHADOW_VIEW | RendererRD::NEXTGEO_CUT_FLAG_DUAL_PARABOLOID;
						vd.dual_paraboloid = true;
						vd.dual_paraboloid_flip = (pass == 1);
						vd.flip_y = true;
						vd.flip_cull = (vd.dual_paraboloid_flip != vd.flip_y) != rev_cull;

						const real_t z_dir = vd.dual_paraboloid_flip ? 1.0 : -1.0;
						const Plane planes[6] = {
							vd.transform.xform(Plane(Vector3(0, 0, z_dir), range)),
							vd.transform.xform(Plane(Vector3(1, 0, z_dir).normalized(), range)),
							vd.transform.xform(Plane(Vector3(-1, 0, z_dir).normalized(), range)),
							vd.transform.xform(Plane(Vector3(0, 1, z_dir).normalized(), range)),
							vd.transform.xform(Plane(Vector3(0, -1, z_dir).normalized(), range)),
							vd.transform.xform(Plane(Vector3(0, 0, -z_dir), 0.02f * range)),
						};
						for (int p = 0; p < 6; p++) {
							vd.frustum[p] = RendererRD::nextgeo_plane_to_frustum(planes[p]);
						}
					} else if (ltype == RSE::LIGHT_SPOT) {
						vd.projection = light_storage->light_instance_get_shadow_camera(li, 0);
						vd.transform = light_storage->light_instance_get_shadow_transform(li, 0);
						vd.light_position = vd.transform.origin;
						vd.tile_size = Size2i(int(shadow_size), int(shadow_size));
						vd.z_near = MAX(0.001f, float(vd.projection.get_z_near()));
						vd.z_far = range;
						vd.cut_flags = RendererRD::NEXTGEO_CUT_FLAG_SHADOW_VIEW;
						vd.dual_paraboloid = false;
						vd.dual_paraboloid_flip = false;
						vd.flip_y = true;
						vd.flip_cull = (vd.dual_paraboloid_flip != vd.flip_y) != rev_cull;

						const Vector<Plane> planes = vd.projection.get_projection_planes(vd.transform);
						if (planes.size() < 6) {
							return;
						}
						for (int p = 0; p < 6; p++) {
							vd.frustum[p] = RendererRD::nextgeo_plane_to_frustum(planes[p]);
						}
					} else if (ltype == RSE::LIGHT_AREA) {
						const Vector2 area_size = light_storage->light_area_get_size(base);
						const Vector2 half_size = area_size * 0.5f;
						vd.projection = light_storage->light_instance_get_shadow_camera(li, 0);
						vd.transform = light_storage->light_instance_get_shadow_transform(li, 0);
						vd.light_position = vd.transform.origin;
						vd.tile_size = Size2i(int(shadow_size), int(shadow_size));
						vd.z_near = MAX(0.001f, MIN(0.025f, range));
						vd.z_far = range + area_size.length() * 0.5f;
						vd.cut_flags = RendererRD::NEXTGEO_CUT_FLAG_SHADOW_VIEW | RendererRD::NEXTGEO_CUT_FLAG_DUAL_PARABOLOID;
						vd.dual_paraboloid = true;
						vd.dual_paraboloid_flip = false;
						vd.flip_y = true;
						vd.flip_cull = (vd.dual_paraboloid_flip != vd.flip_y) != rev_cull;

						const Plane planes[6] = {
							vd.transform.xform(Plane(Vector3(0, 0, -1), range)),
							vd.transform.xform(Plane(Vector3(1, 0, 0), range + half_size.x)),
							vd.transform.xform(Plane(Vector3(-1, 0, 0), range + half_size.x)),
							vd.transform.xform(Plane(Vector3(0, 1, 0), range + half_size.y)),
							vd.transform.xform(Plane(Vector3(0, -1, 0), range + half_size.y)),
							vd.transform.xform(Plane(Vector3(0, 0, 1), 0.02f * range)),
						};
						for (int p = 0; p < 6; p++) {
							vd.frustum[p] = RendererRD::nextgeo_plane_to_frustum(planes[p]);
						}
					} else {
						return;
					}
				}

				render_shadow_to_view[p_shadow_index] = int32_t(shadow_views.size());
				shadow_views.push_back(vd);
			};

			for (const int &index : p_render_data->cube_shadows) {
				append_shadow_view(index);
			}
			for (uint32_t i = 0; i < p_render_data->directional_shadows.size(); i++) {
				append_shadow_view(p_render_data->directional_shadows[i]);
			}
			for (uint32_t i = 0; i < p_render_data->shadows.size(); i++) {
				append_shadow_view(p_render_data->shadows[i]);
			}

			if (!shadow_views.is_empty()) {
				RSG::virtual_mesh_storage->flush();
				nextgeo_cut.prepare_gpu_records(RSG::virtual_mesh_storage);
				if (!nextgeo_cut.dispatch_shadows(RSG::virtual_mesh_storage, shadow_views.ptr(), shadow_views.size(), bool(GLOBAL_GET("rendering/nextgeo/debug_stats")))) {
					for (uint32_t i = 0; i < render_shadow_to_view.size(); i++) {
						render_shadow_to_view[i] = -1;
					}
				}
			}
		}

		if (p_render_data->cube_shadows.size()) {
			RENDER_TIMESTAMP("Render OmniLight Shadows");
			// Cube shadows are rendered in their own way.
			for (const int &index : p_render_data->cube_shadows) {
				_render_shadow_pass(p_render_data->render_shadows[index].light, p_render_data->shadow_atlas, p_render_data->render_shadows[index].pass, p_render_data->render_shadows[index].instances, lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, true, true, true, p_render_data->render_info, viewport_size, p_render_data->scene_data->cam_transform, render_shadow_to_view[index]);
			}
		}

		if (p_render_data->directional_shadows.size()) {
			//open the pass for directional shadows
			light_storage->update_directional_shadow_atlas();
			RD::get_singleton()->draw_list_begin(light_storage->direction_shadow_get_fb(), RD::DRAW_CLEAR_DEPTH, Vector<Color>(), 0.0f);
			RD::get_singleton()->draw_list_end();
		}
	}

	// Render GI

	bool render_shadows = p_render_data->directional_shadows.size() || p_render_data->shadows.size();
	bool render_gi = rb.is_valid() && p_use_gi;
	// GodotNext: nextgi_frame_block
	// NextGI writes these buffers except when VoxelGI supplies diffuse lighting in reflections-only mode.
	render_gi = render_gi && (!p_use_nextgi || RendererRD::NextGI::is_reflections_only());

	if (render_shadows && render_gi) {
		RENDER_TIMESTAMP("Render GI + Render Directional/SpotLight Shadows (Parallel)");
	} else if (render_shadows) {
		RENDER_TIMESTAMP("Render Directional/SpotLight Shadows");
	} else if (render_gi) {
		RENDER_TIMESTAMP("Render GI");
	}

	//prepare shadow rendering
	if (render_shadows) {
		_render_shadow_begin();

		//render directional shadows
		for (uint32_t i = 0; i < p_render_data->directional_shadows.size(); i++) {
			const int shadow_index = p_render_data->directional_shadows[i];
			_render_shadow_pass(p_render_data->render_shadows[shadow_index].light, p_render_data->shadow_atlas, p_render_data->render_shadows[shadow_index].pass, p_render_data->render_shadows[shadow_index].instances, lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, false, i == p_render_data->directional_shadows.size() - 1, false, p_render_data->render_info, viewport_size, p_render_data->scene_data->cam_transform, render_shadow_to_view[shadow_index]);
		}
		//render positional shadows
		for (uint32_t i = 0; i < p_render_data->shadows.size(); i++) {
			const int shadow_index = p_render_data->shadows[i];
			_render_shadow_pass(p_render_data->render_shadows[shadow_index].light, p_render_data->shadow_atlas, p_render_data->render_shadows[shadow_index].pass, p_render_data->render_shadows[shadow_index].instances, lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, i == 0, i == p_render_data->shadows.size() - 1, true, p_render_data->render_info, viewport_size, p_render_data->scene_data->cam_transform, render_shadow_to_view[shadow_index]);
		}

		_render_shadow_process();
	}

	if (render_gi) {
		gi.process_gi(rb, p_normal_roughness_slices, p_voxel_gi_buffer_slices, p_render_data->environment, p_render_data->scene_data->view_count, p_render_data->scene_data->view_projection, p_render_data->scene_data->view_eye_offset, p_render_data->scene_data->cam_transform, *p_render_data->voxel_gi_instances);
	}

	if (render_shadows) {
		_render_shadow_end();
	}

	// GodotNext: light_function_atlas (#138, plan §21.3 contract M8). Bake dirty static and per-frame
	// animated light functions once per frame before NextGI SRC relight, light buffers, and volumetric fog.
	light_function_atlas.bake(RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0, time);

	// GodotNext: nextgi_frame_block
	// Right after the shadow batch, which shares RENDER_LIST_SECONDARY with SRC card capture.
	// The last-frame color is allocated first so the screen trace can read the previous frame.
	if (rb.is_valid() && ss_effects && (p_use_ssil || p_use_ssr || p_use_nextgi)) {
		ss_effects->allocate_last_frame_buffer(rb, p_use_ssil, p_use_ssr, p_use_nextgi);
	}
	if (p_use_nextgi && rb.is_valid()) {
		next_gi.set_active_debug_view(RendererRD::NextGI::debug_view_from_viewport(get_debug_draw_mode()));
		_render_nextgi_translucency_mark(p_render_data);
		next_gi.render_frame(p_render_data, p_normal_roughness_slices, gi.is_half_resolution(), rt_shadows_view);
	} else if (rt_shadows_view && rb.is_valid()) {
		// GodotNext: rt_shadows (#141). Standalone HWRT ray-traced shadows prepare the RT scene TLAS
		// and shadow visibility tables without running NextGI diffuse/reflection passes.
		next_gi.prepare_rt_scene_for_shadows(p_render_data);
	}

	if (rb_data.is_valid() && ss_effects) {
		// Note, in multiview we're allocating buffers for each eye/view we're rendering.
		// This should allow most of the processing to happen in parallel even if we're doing
		// drawcalls per eye/view. It will all sync up at the barrier.

		if ((p_use_ssil || p_use_ssr) && !p_use_nextgi) {
			ss_effects->allocate_last_frame_buffer(rb, p_use_ssil, p_use_ssr, false);
		}

		if (p_use_ssao || p_use_ssil) {
			RENDER_TIMESTAMP("Prepare Depth for SSAO/SSIL");
			// Convert our depth buffer data to linear data in
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				ss_effects->downsample_depth(rb, v, p_render_data->scene_data->view_projection[v]);
			}

			if (p_use_ssao) {
				_process_ssao(rb, p_render_data->environment, p_normal_roughness_slices, p_render_data->scene_data->view_projection);
			}

			if (p_use_ssil) {
				_process_ssil(rb, p_render_data->environment, p_normal_roughness_slices, p_render_data->scene_data->view_projection, p_render_data->scene_data->cam_transform);
			}
		}

		if (p_use_sscs && !p_render_data->contact_shadow_lights.is_empty()) {
			_process_sscs(rb, p_render_data->scene_data->view_projection, p_render_data->scene_data->cam_transform, p_render_data->contact_shadow_lights, p_render_data->scene_data->taa_frame_count);
		}

		if (p_use_ssr) {
			_process_ssr(rb, p_render_data->environment, p_normal_roughness_slices, p_render_data->scene_data->view_projection, p_render_data->scene_data->view_eye_offset, p_render_data->scene_data->cam_transform);
		}
	}

	RENDER_TIMESTAMP("Pre Opaque Render");

	if (current_cluster_builder) {
		// Note: when rendering stereoscopic (multiview) we are using our combined frustum projection to create
		// our cluster data. We use reprojection in the shader to adjust for our left/right eye.
		// This only works as we don't filter our cluster by depth buffer.
		// If we ever make this optimization we should make it optional and only use it in mono.
		// What we win by filtering out a few lights, we loose by having to do the work double for stereo.
		current_cluster_builder->begin(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection, !p_render_data->reflection_probe.is_valid());
		// GodotNext: light_grid (#139). Enable packed light grid collection only when active for this view.
		current_cluster_builder->set_packed_grid_active(packed_light_grid_active);
	}

	bool using_shadows = true;

	if (p_render_data->reflection_probe.is_valid()) {
		if (!RSG::light_storage->reflection_probe_renders_shadows(light_storage->reflection_probe_instance_get_probe(p_render_data->reflection_probe))) {
			using_shadows = false;
		}
	} else {
		//do not render reflections when rendering a reflection probe
		light_storage->update_reflection_probe_buffer(p_render_data, *p_render_data->reflection_probes, p_render_data->scene_data->cam_transform.affine_inverse(), p_render_data->environment);
	}

	uint32_t directional_light_count = 0;
	uint32_t positional_light_count = 0;
	// GodotNext: vsm_projection (#222) / df_shadows (#214). The lights of a view that samples VSM get
	// LIGHT_FLAGS_SAMPLE, and DF-enabled lights get layer assignments for df_shadow.glsl.
	RendererRD::VirtualShadowMapStorage *vsm_storage = RendererRD::VirtualShadowMapStorage::get_singleton();
	if (vsm_storage != nullptr) {
		vsm_storage->set_view_samples(vsm_sample_view);
		const RendererVirtualShadowMapStorage::PoolQuality df_quality = vsm_storage->is_active() ? vsm_storage->get_quality() : RendererVirtualShadowMapStorage::read_quality();
		vsm_storage->set_df_view_enabled(df_shadows_view, RendererVirtualShadowMapStorage::df_read_max_lights(df_quality));
	}
	// GodotNext: rt_shadows (#141). Open per-view RT shadow registration before LightStorage fills LightData.
	if (rt_shadows_view) {
		RendererRD::NextGIRTShadows *rt_shadows = next_gi.ensure_rt_shadows();
		if (rt_shadows != nullptr) {
			const RendererVirtualShadowMapStorage::PoolQuality rt_quality = (vsm_storage != nullptr && vsm_storage->is_active()) ? vsm_storage->get_quality() : RendererVirtualShadowMapStorage::read_quality();
			const uint32_t rt_tile_budget = RendererRD::nextgi_rt_shadow_preset_tile_budget(rt_quality);
			rt_shadows->begin_view(p_render_data, next_gi.get_rt_scene(), rt_tile_budget);
		}
	}
	light_storage->update_light_buffers(p_render_data, *p_render_data->lights, p_render_data->scene_data->cam_transform, p_render_data->shadow_atlas, using_shadows, directional_light_count, positional_light_count, p_render_data->directional_light_soft_shadows);
	if (vsm_storage != nullptr) {
		if (vsm_sample_view) {
			const uint32_t vsm_local_culled = p_render_data->sdfgi_update_data != nullptr ? p_render_data->sdfgi_update_data->vsm_local_light_count : 0u;
			vsm_storage->record_projection_view(vsm_atlas_passes, vsm_atlas_passes_skipped, vsm_local_culled);
		}
		vsm_storage->set_view_samples(false);
	}
	texture_storage->update_decal_buffer(*p_render_data->decals, p_render_data->scene_data->cam_transform);

	p_render_data->directional_light_count = directional_light_count;

	if (current_cluster_builder) {
		// GodotNext: light_grid (#139, plan §21.3 contract M2).
		if (packed_light_grid_active) {
			RID hiz_depth_tex;
			if (packed_light_grid_has_depth_prepass && rb_data.is_valid() && !p_render_data->reflection_probe.is_valid() && rb->get_view_count() == 1) {
				hiz_depth_tex = rb->get_depth_texture(0);
			}
			RID hiz_sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			const bool cull_front_of_min = render_list[RENDER_LIST_ALPHA].elements.is_empty();
			current_cluster_builder->bake_packed_light_grid(hiz_depth_tex, hiz_sampler, cull_front_of_min);

			const RendererRD::LightGrid::ZParams zp = current_cluster_builder->get_packed_z_params();
			p_render_data->packed_light_grid_headers_buffer = current_cluster_builder->get_packed_cell_headers_buffer();
			p_render_data->packed_light_grid_index_buffer = current_cluster_builder->get_packed_index_buffer();
			p_render_data->packed_light_grid_dims = current_cluster_builder->get_packed_grid_dims();
			p_render_data->packed_light_grid_flags = current_cluster_builder->get_packed_grid_flags();
			p_render_data->packed_light_grid_z_near = zp.z_near;
			p_render_data->packed_light_grid_z_far = zp.z_far;
			p_render_data->packed_light_grid_z_scale = zp.z_scale;
			p_render_data->packed_light_grid_z_bias = zp.z_bias;
		} else {
			p_render_data->packed_light_grid_headers_buffer = cluster_builder_shared.get_default_light_grid_buffer();
			p_render_data->packed_light_grid_index_buffer = cluster_builder_shared.get_default_light_grid_buffer();
			p_render_data->packed_light_grid_flags = 0u;
		}

		current_cluster_builder->bake_cluster();

		if (!p_render_data->reflection_probe.is_valid() && RendererRD::LightGrid::consume_check_request()) {
			RendererRD::LightGrid::store_check_result(current_cluster_builder->run_packed_grid_check());
		}
	}

	// GodotNext: megalights_sampling (#142, plan §21.3 contract M3, GM-1, GM-2).
	// Run per-pixel weighted light sampling (or reference accumulation / GPU verification check)
	// after the prepass and packed froxel light grid are built, leaving Forward+ Set 1 untouched
	// for #141/#143.
	if (megalights_inputs_active && !p_render_data->reflection_probe.is_valid() && rb.is_valid() && rb->get_view_count() == 1 && rb_data.is_valid() && rb->has_megalights_inputs()) {
		const RID ml_depth = rb->get_depth_texture(0);
		const RID ml_normal_roughness = (p_normal_roughness_slices != nullptr && p_normal_roughness_slices[0].is_valid()) ? p_normal_roughness_slices[0] : rb_data->get_normal_roughness(0);
		const RID ml_albedo = rb->get_texture_slice(RB_SCOPE_MEGALIGHTS, RB_TEX_ML_ALBEDO, 0, 0);
		const RID ml_material = rb->get_texture_slice(RB_SCOPE_MEGALIGHTS, RB_TEX_ML_MATERIAL, 0, 0);
		if (ml_depth.is_valid() && ml_normal_roughness.is_valid() && ml_albedo.is_valid() && ml_material.is_valid()) {
			if (mega_lights == nullptr) {
				mega_lights = memnew(RendererRD::MegaLights);
			}
			RendererRD::MegaLights::FrameInputs ml_inputs;
			ml_inputs.render_data = p_render_data;
			ml_inputs.depth_texture = ml_depth;
			ml_inputs.normal_roughness_texture = ml_normal_roughness;
			ml_inputs.ml_albedo_texture = ml_albedo;
			ml_inputs.ml_material_texture = ml_material;
			ml_inputs.dfg_lut_texture = dfg_lut.texture;
			ml_inputs.ltc_lut1_texture = texture_storage->texture_get_rd_texture(ltc.lut1_texture);
			ml_inputs.ltc_lut2_texture = texture_storage->texture_get_rd_texture(ltc.lut2_texture);
			ml_inputs.light_function_atlas = &light_function_atlas;
			ml_inputs.next_gi = &next_gi;
			ml_inputs.flip_y = p_render_data->scene_data->flip_y;
			ml_inputs.use_soft_shadows = true;
			mega_lights->render_sampling(ml_inputs, rb);
		}
	}

	// GodotNext: vsm_page_marking (#219) / vsm_non_nanite (#218) / vsm_nanite (#217). After the light buffers (vsm_map_base) and the cluster.
	if (vsm_mark_view && rb_data.is_valid() && current_cluster_builder) {
		// GodotNext: vsm_nanite (#217). Optional test/debug hook (off by default until #212 lands):
		// marks cached pages' dynamic layer dirty before allocation so dynamic-layer routing and
		// next-frame Hi-Z culling can be exercised end-to-end on the GPU.
		if (vsm_storage != nullptr && bool(_vsm_setting("debug_nanite_dynamic_layer", false))) {
			nextgeo_cut.mark_vsm_dynamic_test(vsm_storage->get_page_table_buffer(), vsm_storage->get_table_capacity(), vsm_storage->get_pool_page_count());
		}
		// GodotNext: vsm_settings_debug (#221). Whether this view wrote the dirty list the debug draws read
		// (vsm_storage from vsm_projection (#222) above).
		const uint64_t vsm_marked_before = vsm_storage != nullptr ? vsm_storage->get_marked_view_count() : 0;
		_vsm_mark_pages(p_render_data);
		vsm_debug_view_marked = vsm_storage != nullptr && vsm_storage->get_marked_view_count() != vsm_marked_before;
		_vsm_raster_non_nanite(p_render_data);
		_vsm_raster_nanite(p_render_data);
	}
	vsm_mark_view = false;
	// GodotNext: vsm_projection (#222). What this view's light loop reads to sample the maps.
	if (vsm_sample_view) {
		_vsm_update_projection(p_render_data);
	}
	// GodotNext: df_shadows (#214, plan §21.3 contract S7). Half-resolution cone-traced DF shadow mask pass.
	if (df_shadows_view) {
		_render_df_shadows(p_render_data, p_normal_roughness_slices != nullptr ? p_normal_roughness_slices[0] : RID(), p_use_nextgi);
	} else {
		df_bind_mask = RID();
		df_bind_guide = RID();
		if (vsm_storage != nullptr) {
			RendererRD::VirtualShadowMapStorage::DFStats empty_stats;
			const RendererVirtualShadowMapStorage::PoolQuality df_quality = vsm_storage->is_active() ? vsm_storage->get_quality() : RendererVirtualShadowMapStorage::read_quality();
			empty_stats.max_lights = RendererVirtualShadowMapStorage::df_read_max_lights(df_quality);
			vsm_storage->record_df_stats(empty_stats);
		}
	}
	if (vsm_storage != nullptr) {
		vsm_storage->set_df_view_enabled(false);
	}
	// GodotNext: rt_shadows (#141, plan §21.3 contract M9). Standalone HWRT tile-sparse ray-traced shadows.
	if (rt_shadows_view) {
		RendererRD::NextGIRTShadows *rt_shadows = next_gi.get_rt_shadows();
		if (rt_shadows != nullptr) {
			const RID nr_tex = p_normal_roughness_slices != nullptr ? p_normal_roughness_slices[0] : RID();
			const bool vel_enabled = rb.is_valid() && rb->has_velocity_buffer(false);
			const RID vel_tex = vel_enabled ? rb->get_velocity_buffer(false) : RID();
			rt_shadows->render(p_render_data, &next_gi, nr_tex, vel_tex, vel_enabled);
			rt_bind_mask = rt_shadows->get_bound_mask_atlas();
			rt_bind_lookup = rt_shadows->get_bound_tile_lookup();
			rt_bind_hit_dist = rt_shadows->get_bound_hit_dist_atlas();
			if (vsm_storage != nullptr) {
				const RendererRD::NextGIRTShadows::ViewStats &st = rt_shadows->get_last_stats();
				RendererRD::VirtualShadowMapStorage::RTShadowStats rs;
				rs.lights_traced = st.lights_traced;
				rs.directional_lights = st.directional_lights;
				rs.local_lights = st.local_lights;
				rs.tiles_allocated = st.tiles_allocated;
				rs.tile_budget = st.tile_budget;
				rs.overflow_tiles = st.overflow_tiles;
				rs.overflow_lights = st.overflow_lights;
				rs.fallback_lights = st.fallback_lights;
				rs.atlas_vram_bytes = st.atlas_vram_bytes;
				rs.atlas_passes_skipped = vsm_atlas_passes_skipped;
				rs.retrace_active = st.retrace_active;
				vsm_storage->record_rt_shadow_stats(rs);
			}
			rt_shadows->end_view();
		}
		next_gi.compact_after_traces();
	} else {
		rt_bind_mask = RID();
		rt_bind_lookup = RID();
		rt_bind_hit_dist = RID();
		if (vsm_storage != nullptr) {
			vsm_storage->record_rt_shadow_stats(RendererRD::VirtualShadowMapStorage::RTShadowStats());
		}
	}

	if (rb_data.is_valid()) {
		RENDER_TIMESTAMP("Update Volumetric Fog");
		bool directional_shadows = RendererRD::LightStorage::get_singleton()->has_directional_shadows(directional_light_count);
		_update_volumetric_fog(rb, p_render_data->environment, p_render_data->scene_data->cam_projection, p_render_data->scene_data->cam_transform, p_render_data->scene_data->prev_cam_transform.affine_inverse(), p_render_data->shadow_atlas, directional_light_count, directional_shadows, positional_light_count, p_render_data->voxel_gi_count, *p_render_data->fog_volumes);
	}
}

// GodotNext: vsm_page_marking (#219) / vsm_projection (#222). The views that mark pages are the views whose
// light loop samples them; the scene cull asks the same question (vsm_view_samples()).
static bool _vsm_view_marks(const Ref<RenderSceneBuffers> &p_render_buffers, bool p_reflection_probe, uint32_t p_view_count) {
	RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
	if (vsm == nullptr || !vsm->is_active() || p_reflection_probe || p_render_buffers.is_null() || p_view_count != 1) {
		return false;
	}
	const Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	if (rb.is_null() || !rb->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		return false;
	}
	return vsm->is_marking_enabled();
}

bool RenderForwardClustered::_vsm_marks_view(const RenderDataRD *p_render_data) const {
	// GodotNext: vsm_page_marking (#219).
	return _vsm_view_marks(p_render_data->render_buffers, p_render_data->reflection_probe.is_valid(), p_render_data->scene_data->view_count);
}

// GodotNext: vsm_projection (#222) / vsm_nanite (#217). True while any NextGeo virtual-mesh
// instance is live; views still sample VSM once NextGeoCut's VSM raster is ready.
static bool _vsm_nextgeo_casters_live() {
	return RSG::virtual_mesh_storage != nullptr && RSG::virtual_mesh_storage->get_live_record_count() > 0;
}

bool RenderForwardClustered::vsm_view_samples(const Ref<RenderSceneBuffers> &p_render_buffers, bool p_reflection_probe, uint32_t p_view_count) {
	// GodotNext: vsm_projection (#222) / vsm_nanite (#217).
	return _vsm_view_marks(p_render_buffers, p_reflection_probe, p_view_count) && (!_vsm_nextgeo_casters_live() || nextgeo_cut.is_vsm_available());
}

bool RenderForwardClustered::vsm_light_skips_atlas(RID p_light_instance, RID p_environment) const {
	// GodotNext: vsm_projection (#222, issue step 7) / vsm_fog_translucency (#215) / vsm_local_lights (#216).
	// A shadowed light whose method resolves to VIRTUAL bypasses the shadow atlas from its first frame onward
	// (including the 88-slot positional atlas and MAX_UPDATE_SHADOWS = 512) whenever VSM has table capacity
	// for its maps; update_light_buffers() allocates them later in the same view before marking and raster.
	(void)p_environment;
	RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
	if (vsm == nullptr || !vsm->is_active()) {
		return false;
	}
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	if (!light_storage->owns_light_instance(p_light_instance)) {
		return false;
	}
	const RID base = light_storage->light_instance_get_base_light(p_light_instance);
	if (base.is_null() || !light_storage->light_has_shadow(base)) {
		return false;
	}
	// Only a light whose shadow method resolves to VIRTUAL (#221's resolve_light_shadow_method(), tier rule
	// included). Its maps outlive a change to ATLAS until update_light() frees them later in the frame, so the
	// method is checked here: an ATLAS light keeps its atlas pass from the frame it changes.
	if (!RendererVirtualShadowMapStorage::light_uses_vsm(light_storage->light_get_shadow_method(base))) {
		return false;
	}
	if (vsm->map_get(p_light_instance) != nullptr) {
		// Keep this light's map range from being LRU-evicted by another light before update_light_buffers() runs.
		vsm->map_touch(p_light_instance);
		return true;
	}
	// Frame 1: bypass the atlas as soon as the page table can hold this light's maps.
	uint32_t needed_maps = 0;
	switch (light_storage->light_get_type(base)) {
		case RSE::LIGHT_DIRECTIONAL: {
			float max_distance = light_storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_MAX_DISTANCE);
			if (max_distance <= 0.0f) {
				max_distance = float(Math::INF);
			}
			needed_maps = RendererVirtualShadowMapStorage::clipmap_level_count(max_distance);
		} break;
		case RSE::LIGHT_SPOT: {
			needed_maps = 1;
		} break;
		case RSE::LIGHT_OMNI: {
			needed_maps = 6;
		} break;
		case RSE::LIGHT_AREA: {
			needed_maps = 5;
		} break;
		default:
			return false;
	}
	return vsm->can_allocate_maps(needed_maps);
}

// GodotNext: vsm_projection (#222) / vsm_fog_translucency (#215). Shared readiness check for
// _vsm_update_projection() and _setup_render_pass_uniform_set() so counts.x is nonzero only when every bound
// VSM resource is valid and sized.
static bool _vsm_projection_buffers_ready(const RendererRD::VirtualShadowMapStorage *p_vsm) {
	if (p_vsm == nullptr || !p_vsm->is_active()) {
		return false;
	}
	const uint32_t table_maps = p_vsm->get_table_capacity();
	return table_maps > 0 && p_vsm->get_map_buffers_capacity() == table_maps &&
			p_vsm->get_pool_texture().is_valid() && p_vsm->get_page_table_buffer().is_valid() &&
			p_vsm->get_map_data_buffer().is_valid() && p_vsm->get_map_state_buffer().is_valid() &&
			p_vsm->get_coarse_miss_buffer().is_valid();
}

bool RenderForwardClustered::vsm_raster_check_armed() const {
	// GodotNext: vsm_projection (#222).
	const RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
	return vsm != nullptr && vsm->is_raster_check_armed();
}

// GodotNext: vsm_projection (#222). A pixel's footprint in meters, view depth * r_scale + r_bias: the world
// size of one internal-resolution pixel at that depth, the finer axis. Page marking requests pages by it and
// the projection reads them by it.
static void _vsm_pixel_footprint(const RenderSceneDataRD *p_scene_data, const Size2i &p_size, float &r_scale, float &r_bias) {
	const Projection &projection = p_scene_data->cam_projection;
	const float size_x = 2.0f / (Math::abs(float(projection.columns[0][0])) * float(MAX(p_size.x, 1)));
	const float size_y = 2.0f / (Math::abs(float(projection.columns[1][1])) * float(MAX(p_size.y, 1)));
	if (projection.is_orthogonal()) {
		r_scale = 0.0f;
		r_bias = MIN(size_x, size_y);
	} else {
		r_scale = MIN(size_x, size_y);
		r_bias = 0.0f;
	}
}

Variant RenderForwardClustered::_vsm_setting(const char *p_name, const Variant &p_default) {
	// GodotNext: vsm_projection (#222).
	const String key = String("rendering/lights_and_shadows/virtual_shadows/") + p_name;
	ProjectSettings *settings = ProjectSettings::get_singleton();
	return (settings != nullptr && settings->has_setting(key)) ? settings->get_setting(key) : p_default;
}

void RenderForwardClustered::_ensure_vsm_projection_resources() {
	// GodotNext: vsm_projection (#222).
	RenderingDevice *rd = RD::get_singleton();
	if (vsm_projection_params_buffer.is_null()) {
		vsm_projection_params_buffer = rd->storage_buffer_create(sizeof(VSMProjectionParams));
		rd->set_resource_name(vsm_projection_params_buffer, "VSM Projection Params");
		rd->buffer_clear(vsm_projection_params_buffer, 0, sizeof(VSMProjectionParams));
	}
	if (vsm_projection_default_buffer.is_null()) {
		// Covers one VSMMapData record (96 bytes) and the params, all zero: every lookup misses.
		vsm_projection_default_buffer = rd->storage_buffer_create(256);
		rd->set_resource_name(vsm_projection_default_buffer, "VSM Projection Default Buffer");
		rd->buffer_clear(vsm_projection_default_buffer, 0, 256);
	}
	if (vsm_projection_default_miss_buffer.is_null()) {
		// GodotNext: vsm_fog_translucency (#215). Separate writable fallback so SyncVal never sees the same
		// fallback buffer bound both readonly (page table/maps) and read-write (coarse miss counters).
		const uint32_t miss_bytes = RendererVirtualShadowMapStorage::COARSE_MISS_WORDS * uint32_t(sizeof(uint32_t));
		vsm_projection_default_miss_buffer = rd->storage_buffer_create(miss_bytes);
		rd->set_resource_name(vsm_projection_default_miss_buffer, "VSM Projection Default Coarse Miss");
		rd->buffer_clear(vsm_projection_default_miss_buffer, 0, miss_bytes);
	}
	if (vsm_projection_default_pool.is_null()) {
		RD::TextureFormat format;
		format.format = RD::DATA_FORMAT_R32_UINT;
		format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		format.width = 1;
		format.height = 1;
		format.array_layers = RendererVirtualShadowMapStorage::POOL_LAYERS;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		Vector<uint8_t> layer;
		layer.resize_initialized(4);
		Vector<Vector<uint8_t>> layers;
		for (uint32_t i = 0; i < RendererVirtualShadowMapStorage::POOL_LAYERS; i++) {
			layers.push_back(layer);
		}
		vsm_projection_default_pool = rd->texture_create(format, RD::TextureView(), layers);
		rd->set_resource_name(vsm_projection_default_pool, "VSM Projection Default Pool");
	}
	if (vsm_projection_blue_noise.is_null()) {
		RD::TextureFormat format;
		format.width = 64;
		format.height = 64;
		format.format = RD::DATA_FORMAT_R8G8_UNORM;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		Vector<uint8_t> noise;
		noise.resize(64 * 64 * 2);
		memcpy(noise.ptrw(), nextgi_blue_noise_tile, 64 * 64 * 2);
		Vector<Vector<uint8_t>> data;
		data.push_back(noise);
		vsm_projection_blue_noise = rd->texture_create(format, RD::TextureView(), data);
		rd->set_resource_name(vsm_projection_blue_noise, "VSM Projection Blue Noise");
	}
}

void RenderForwardClustered::_free_vsm_projection_resources() {
	// GodotNext: vsm_projection (#222).
	RenderingDevice *rd = RD::get_singleton();
	for (RID *rid : { &vsm_projection_params_buffer, &vsm_projection_default_buffer, &vsm_projection_default_miss_buffer, &vsm_projection_default_pool, &vsm_projection_blue_noise }) {
		if (rd != nullptr && rid->is_valid()) {
			rd->free_rid(*rid);
		}
		*rid = RID();
	}
}

void RenderForwardClustered::_vsm_update_projection(const RenderDataRD *p_render_data) {
	// GodotNext: vsm_projection (#222) / vsm_fog_translucency (#215). The light loop and volumetric fog
	// read these to sample the maps.
	RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
	ERR_FAIL_NULL(vsm);
	_ensure_vsm_projection_resources();
	vsm->clear_view_coarse_miss_counters();
	VSMProjectionParams params;
	const Size2i size = p_render_data->render_buffers.is_valid() ? p_render_data->render_buffers->get_internal_size() : Size2i(1, 1);
	_vsm_pixel_footprint(p_render_data->scene_data, size, params.footprint[0], params.footprint[1]);
	params.bias[0] = vsm->get_lod_bias_directional();
	params.bias[1] = vsm->get_lod_bias_local();
	// The table's map count while every bound buffer matches it (0 otherwise: every lookup misses).
	params.counts[0] = _vsm_projection_buffers_ready(vsm) ? vsm->get_table_capacity() : 0u;
	using VSM = RendererVirtualShadowMapStorage;
	const VSM::PoolQuality quality = vsm->get_quality();
	params.counts[1] = VSM::smrt_setting(int64_t(_vsm_setting("smrt_rays_local", -1)), VSM::smrt_preset_rays(quality), VSM::SMRT_MAX_RAYS);
	params.counts[2] = VSM::smrt_setting(int64_t(_vsm_setting("smrt_rays_directional", -1)), VSM::smrt_preset_rays(quality), VSM::SMRT_MAX_RAYS);
	params.counts[3] = MAX(VSM::smrt_setting(int64_t(_vsm_setting("smrt_steps", -1)), VSM::smrt_preset_steps(quality), VSM::SMRT_MAX_STEPS), 1u);
	params.misc[0] = bool(_vsm_setting("prefiltered_distant", false)) ? 1u : 0u;
	const VSM::TranslucentQuality translucent_quality = VSM::read_translucent_quality();
	vsm_translucent_coarse = (translucent_quality == VSM::TRANSLUCENT_QUALITY_COARSE);
	RD::get_singleton()->buffer_update(vsm_projection_params_buffer, 0, sizeof(VSMProjectionParams), &params);
	vsm->set_projection_params(params.counts[1], params.counts[2], params.counts[3], params.misc[0] != 0u, uint32_t(translucent_quality));
}

// GodotNext: df_shadows (#214, plan §21.3 contract S7, GV-10).
bool RenderForwardClustered::df_view_samples(const Ref<RenderSceneBuffers> &p_render_buffers, bool p_reflection_probe, uint32_t p_view_count) {
	if (p_reflection_probe || p_render_buffers.is_null() || p_view_count != 1) {
		return false;
	}
	const Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	if (rb.is_null() || !rb->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		return false;
	}
	return RendererVirtualShadowMapStorage::is_df_available();
}

bool RenderForwardClustered::df_light_skips_atlas(RID p_light_instance, RID p_environment) const {
	if (!RendererVirtualShadowMapStorage::is_df_available()) {
		return false;
	}
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	if (!light_storage->owns_light_instance(p_light_instance)) {
		return false;
	}
	const RID base = light_storage->light_instance_get_base_light(p_light_instance);
	if (base.is_null() || !light_storage->light_has_shadow(base)) {
		return false;
	}
	if (!RendererVirtualShadowMapStorage::light_uses_df(light_storage->light_get_shadow_method(base))) {
		return false;
	}
	if (p_environment.is_valid() && environment_get_volumetric_fog_enabled(p_environment) && light_storage->light_get_param(base, RSE::LIGHT_PARAM_VOLUMETRIC_FOG_ENERGY) > 0.0f) {
		return false;
	}
	return true;
}

bool RenderForwardClustered::_df_view_has_lights(const RenderDataRD *p_render_data) const {
	if (p_render_data == nullptr || p_render_data->lights == nullptr || p_render_data->scene_data == nullptr) {
		return false;
	}
	if (p_render_data->reflection_probe.is_valid() || p_render_data->render_buffers.is_null() || p_render_data->scene_data->view_count != 1) {
		return false;
	}
	if (!p_render_data->render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		return false;
	}
	if (!RendererVirtualShadowMapStorage::is_df_available()) {
		return false;
	}
	RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
	const RendererVirtualShadowMapStorage::PoolQuality df_quality = (vsm != nullptr && vsm->is_active()) ? vsm->get_quality() : RendererVirtualShadowMapStorage::read_quality();
	if (RendererVirtualShadowMapStorage::df_read_max_lights(df_quality) == 0u) {
		return false;
	}
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	for (uint32_t i = 0; i < p_render_data->lights->size(); i++) {
		const RID li = (*p_render_data->lights)[i];
		if (!light_storage->owns_light_instance(li)) {
			continue;
		}
		const RID base = light_storage->light_instance_get_base_light(li);
		if (base.is_null() || !light_storage->light_has_shadow(base)) {
			continue;
		}
		const RSE::LightShadowMethod sm = light_storage->light_get_shadow_method(base);
		if (RendererVirtualShadowMapStorage::light_uses_df(sm)) {
			return true;
		}
		if (light_storage->light_get_type(base) == RSE::LIGHT_DIRECTIONAL && light_storage->light_directional_get_sky_mode(base) != RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
			const float start = light_storage->light_get_param(base, RSE::LIGHT_PARAM_DISTANCE_FIELD_SHADOW_START);
			const float dist = light_storage->light_get_param(base, RSE::LIGHT_PARAM_DISTANCE_FIELD_SHADOW_DISTANCE);
			const float shadow_max = light_storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_MAX_DISTANCE);
			if (RendererVirtualShadowMapStorage::df_directional_handoff_start(start, dist, shadow_max, true) > 0.0f) {
				return true;
			}
		}
	}
	return false;
}

// GodotNext: rt_shadows (#141, plan §21.3 contract M9).
bool RenderForwardClustered::rt_view_samples(const Ref<RenderSceneBuffers> &p_render_buffers, bool p_reflection_probe, uint32_t p_view_count) {
	if (p_reflection_probe || p_render_buffers.is_null() || p_view_count != 1) {
		return false;
	}
	const Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	if (rb.is_null() || !rb->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		return false;
	}
	return RendererVirtualShadowMapStorage::is_rt_available();
}

bool RenderForwardClustered::rt_light_skips_atlas(RID p_light_instance, RID p_environment, const Ref<RenderSceneBuffers> &p_render_buffers) const {
	(void)p_environment;
	if (!RendererVirtualShadowMapStorage::is_rt_available()) {
		return false;
	}
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	if (!light_storage->owns_light_instance(p_light_instance)) {
		return false;
	}
	const RID base = light_storage->light_instance_get_base_light(p_light_instance);
	if (base.is_null() || !light_storage->light_has_shadow(base)) {
		return false;
	}
	if (!RendererVirtualShadowMapStorage::light_uses_rt(light_storage->light_get_shadow_method(base))) {
		return false;
	}
	// Overflow-fallback lights keep VSM/atlas until they fit inside the tile budget again.
	const Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	if (RendererRD::NextGIRTShadows::is_light_in_overflow_fallback(rb, p_light_instance)) {
		return false;
	}
	return true;
}

bool RenderForwardClustered::rt_light_in_overflow_fallback(RID p_light_instance, const Ref<RenderSceneBuffers> &p_render_buffers) const {
	const Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	return RendererRD::NextGIRTShadows::is_light_in_overflow_fallback(rb, p_light_instance);
}

bool RenderForwardClustered::_rt_view_has_lights(const RenderDataRD *p_render_data) const {
	if (p_render_data == nullptr || p_render_data->lights == nullptr || p_render_data->scene_data == nullptr) {
		return false;
	}
	if (p_render_data->reflection_probe.is_valid() || p_render_data->render_buffers.is_null() || p_render_data->scene_data->view_count != 1) {
		return false;
	}
	if (!p_render_data->render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		return false;
	}
	if (!RendererVirtualShadowMapStorage::is_rt_available()) {
		return false;
	}
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	for (uint32_t i = 0; i < p_render_data->lights->size(); i++) {
		const RID li = (*p_render_data->lights)[i];
		if (!light_storage->owns_light_instance(li)) {
			continue;
		}
		const RID base = light_storage->light_instance_get_base_light(li);
		if (base.is_null() || !light_storage->light_has_shadow(base)) {
			continue;
		}
		if (light_storage->light_get_type(base) == RSE::LIGHT_DIRECTIONAL &&
				light_storage->light_directional_get_sky_mode(base) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
			continue;
		}
		if (RendererVirtualShadowMapStorage::light_uses_rt(light_storage->light_get_shadow_method(base))) {
			return true;
		}
	}
	return false;
}

void RenderForwardClustered::_ensure_rt_shadow_default_resources() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	if (rt_default_mask.is_null()) {
		RD::TextureFormat fmt;
		fmt.format = RD::DATA_FORMAT_R8_UNORM;
		fmt.texture_type = RD::TEXTURE_TYPE_2D;
		fmt.width = 1;
		fmt.height = 1;
		fmt.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		Vector<uint8_t> one;
		one.resize(1);
		one.write[0] = 255u;
		Vector<Vector<uint8_t>> slices;
		slices.push_back(one);
		rt_default_mask = rd->texture_create(fmt, RD::TextureView(), slices);
		rd->set_resource_name(rt_default_mask, "RT Shadow Default Mask");
	}
	if (rt_default_hit_dist.is_null()) {
		RD::TextureFormat fmt;
		fmt.format = RD::DATA_FORMAT_R16_SFLOAT;
		fmt.texture_type = RD::TEXTURE_TYPE_2D;
		fmt.width = 1;
		fmt.height = 1;
		fmt.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		Vector<uint8_t> miss;
		miss.resize(2);
		const uint16_t miss_fp16 = Math::make_half_float(RendererRD::NEXTGI_RT_SHADOW_HIT_MISS);
		memcpy(miss.ptrw(), &miss_fp16, sizeof(uint16_t));
		Vector<Vector<uint8_t>> slices;
		slices.push_back(miss);
		rt_default_hit_dist = rd->texture_create(fmt, RD::TextureView(), slices);
		rd->set_resource_name(rt_default_hit_dist, "RT Shadow Default Hit Dist");
	}
	if (rt_default_lookup.is_null()) {
		rt_default_lookup = rd->storage_buffer_create(16u);
		rd->set_resource_name(rt_default_lookup, "RT Shadow Default Lookup");
		rd->buffer_clear(rt_default_lookup, 0, 16u);
	}
}

void RenderForwardClustered::_free_rt_shadow_default_resources() {
	RenderingDevice *rd = RD::get_singleton();
	for (RID *rid : { &rt_default_mask, &rt_default_hit_dist, &rt_default_lookup }) {
		if (rd != nullptr && rid->is_valid()) {
			rd->free_rid(*rid);
		}
		*rid = RID();
	}
	rt_bind_mask = RID();
	rt_bind_lookup = RID();
	rt_bind_hit_dist = RID();
}

void RenderForwardClustered::_ensure_df_default_textures() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	if (df_default_mask.is_null()) {
		RD::TextureFormat fmt;
		fmt.format = RD::DATA_FORMAT_R8_UNORM;
		fmt.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		fmt.width = 1;
		fmt.height = 1;
		fmt.array_layers = RendererVirtualShadowMapStorage::DF_MAX_LIGHTS_PER_VIEW;
		fmt.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		Vector<uint8_t> one;
		one.resize(1);
		one.write[0] = 255u;
		Vector<Vector<uint8_t>> layers;
		for (uint32_t i = 0; i < RendererVirtualShadowMapStorage::DF_MAX_LIGHTS_PER_VIEW; i++) {
			layers.push_back(one);
		}
		df_default_mask = rd->texture_create(fmt, RD::TextureView(), layers);
		rd->set_resource_name(df_default_mask, "DF Shadow Default Mask");
	}
	if (df_default_guide.is_null()) {
		RD::TextureFormat fmt;
		fmt.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		fmt.texture_type = RD::TEXTURE_TYPE_2D;
		fmt.width = 1;
		fmt.height = 1;
		fmt.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		Vector<uint8_t> data;
		data.resize_initialized(8); // (0, 0, 0, 0): linear depth 0 -> bilateral upsample falls back to unshadowed 1.0.
		Vector<Vector<uint8_t>> slices;
		slices.push_back(data);
		df_default_guide = rd->texture_create(fmt, RD::TextureView(), slices);
		rd->set_resource_name(df_default_guide, "DF Shadow Default Guide");
	}
}

bool RenderForwardClustered::_ensure_df_resources() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	_ensure_df_default_textures();
	if (df_shader_ready) {
		return true;
	}
	if (df_shader_failed) {
		return false;
	}
	Vector<String> modes;
	modes.push_back("");
	df_shader.initialize(modes);
	df_shader_version = df_shader.version_create();
	if (df_shader_version.is_null()) {
		df_shader_failed = true;
		return false;
	}
	const RID shader_rid = df_shader.version_get_shader(df_shader_version, 0);
	if (shader_rid.is_null()) {
		df_shader.version_free(df_shader_version);
		df_shader_version = RID();
		df_shader_failed = true;
		return false;
	}
	df_pipeline = rd->compute_pipeline_create(shader_rid);
	if (df_pipeline.is_null()) {
		df_shader.version_free(df_shader_version);
		df_shader_version = RID();
		df_shader_failed = true;
		return false;
	}
	if (df_params_buffer.is_null()) {
		df_params_buffer = rd->storage_buffer_create(sizeof(DFParamsGPU));
		rd->set_resource_name(df_params_buffer, "DF Shadow Params");
	}
	if (df_lights_buffer.is_null()) {
		df_lights_buffer = rd->storage_buffer_create(sizeof(DFLightGPU) * RendererVirtualShadowMapStorage::DF_MAX_LIGHTS_PER_VIEW);
		rd->set_resource_name(df_lights_buffer, "DF Shadow Lights");
	}
	if (df_nearest_sampler.is_null()) {
		RD::SamplerState sstate;
		sstate.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		sstate.min_filter = RD::SAMPLER_FILTER_NEAREST;
		sstate.mip_filter = RD::SAMPLER_FILTER_NEAREST;
		sstate.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		sstate.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		sstate.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		df_nearest_sampler = rd->sampler_create(sstate);
	}
	df_shader_ready = true;
	return true;
}

void RenderForwardClustered::_free_df_resources() {
	if (df_swrt_trace != nullptr) {
		memdelete(df_swrt_trace);
		df_swrt_trace = nullptr;
	}
	RenderingDevice *rd = RD::get_singleton();
	for (RID *rid : { &df_pipeline, &df_params_buffer, &df_lights_buffer, &df_default_mask, &df_default_guide, &df_nearest_sampler }) {
		if (rd != nullptr && rid->is_valid()) {
			rd->free_rid(*rid);
		}
		*rid = RID();
	}
	if (df_shader_version.is_valid()) {
		df_shader.version_free(df_shader_version);
		df_shader_version = RID();
	}
	df_shader_ready = false;
	df_shader_failed = false;
	df_bind_mask = RID();
	df_bind_guide = RID();
}

void RenderForwardClustered::_render_df_shadows(RenderDataRD *p_render_data, RID p_normal_roughness, bool p_use_nextgi) {
	// GodotNext: df_shadows (#214, plan §21.3 contract S7, GV-10).
	df_bind_mask = RID();
	df_bind_guide = RID();
	RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
	RenderingDevice *rd = RD::get_singleton();
	if (vsm == nullptr || rd == nullptr || p_render_data == nullptr || p_render_data->scene_data == nullptr || !RendererVirtualShadowMapStorage::is_df_available()) {
		return;
	}
	const LocalVector<RendererRD::VirtualShadowMapStorage::DFViewLight> &df_lights = vsm->get_df_lights();
	const RendererVirtualShadowMapStorage::PoolQuality df_quality = vsm->is_active() ? vsm->get_quality() : RendererVirtualShadowMapStorage::read_quality();
	RendererRD::VirtualShadowMapStorage::DFStats stats;
	stats.max_lights = vsm->get_df_view_max_lights();
	stats.lights_dropped = vsm->get_df_dropped_lights();
	if (df_lights.is_empty()) {
		vsm->record_df_stats(stats);
		return;
	}
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	if (rb.is_null() || !rb->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		vsm->record_df_stats(stats);
		return;
	}
	Ref<RenderBufferDataForwardClustered> rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	if (rb_data.is_null() || !_ensure_df_resources()) {
		vsm->record_df_stats(stats);
		return;
	}

	const float max_df_dist = vsm->get_df_max_distance();
	uint32_t target_levels = NextGIGlobalSDF::BASE_LEVELS;
	while (target_levels < NextGIGlobalSDF::MAX_LEVELS && NextGIGlobalSDF::level_voxel_size(target_levels - 1u) * 60.0f < max_df_dist) {
		target_levels++;
	}

	NextGISWRTTrace *swrt_trace = nullptr;
	bool reused_nextgi = false;
	if (p_use_nextgi && next_gi.get_tier_result().tier == RendererRD::NextGI::TIER_SOFTWARE && next_gi.get_swrt_trace() != nullptr && next_gi.get_swrt_trace()->is_ready()) {
		swrt_trace = next_gi.get_swrt_trace();
		reused_nextgi = true;
		swrt_trace->update_extra_levels_for_df(p_render_data, target_levels);
		const bool reuse_froxels = !RendererRD::NextGI::is_lumen_lite() && !RendererRD::NextGI::is_reflections_only() && swrt_trace->is_detail_active();
		swrt_trace->prepare_df_view(p_render_data, next_gi.get_voxel_lighting(), reuse_froxels);
	} else {
		if (df_swrt_trace == nullptr) {
			df_swrt_trace = memnew(NextGISWRTTrace);
		}
		df_swrt_trace->update_scene_for_df(p_render_data, target_levels);
		df_swrt_trace->prepare_df_view(p_render_data, nullptr, false);
		swrt_trace = df_swrt_trace;
	}

	if (swrt_trace == nullptr || !swrt_trace->is_ready()) {
		vsm->record_df_stats(stats);
		return;
	}

	const Size2i full_size = rb->get_internal_size();
	const uint32_t light_count = MIN(df_lights.size(), RendererVirtualShadowMapStorage::DF_MAX_LIGHTS_PER_VIEW);
	rb_data->ensure_df_shadow_targets(full_size, light_count);
	const RID mask_tex = rb_data->get_df_shadow_mask();
	const RID guide_tex = rb_data->get_df_shadow_guide();
	const Size2i half_size = rb_data->get_df_shadow_half_size();
	const RID depth_tex = rb->get_depth_texture(0);
	if (mask_tex.is_null() || guide_tex.is_null() || depth_tex.is_null()) {
		vsm->record_df_stats(stats);
		return;
	}

	const uint32_t active_levels = MIN(swrt_trace->get_df_active_levels(), NextGIGlobalSDF::MAX_LEVELS);
	const bool has_normal = p_normal_roughness.is_valid();
	const bool has_detail = swrt_trace->has_df_detail_froxels();
	const uint32_t fresh_mask = swrt_trace->get_df_coarse_mask(active_levels);

	DFParamsGPU params;
	params.sizes[0] = uint32_t(MAX(full_size.x, 1));
	params.sizes[1] = uint32_t(MAX(full_size.y, 1));
	params.sizes[2] = uint32_t(MAX(half_size.x, 1));
	params.sizes[3] = uint32_t(MAX(half_size.y, 1));
	params.counts[0] = light_count;
	params.counts[1] = RendererVirtualShadowMapStorage::df_preset_max_steps(df_quality);
	params.counts[2] = active_levels;
	params.counts[3] = (has_normal ? 1u : 0u) | (has_detail ? 2u : 0u);
	params.fresh[0] = fresh_mask;
	params.fresh[1] = fresh_mask;
	for (uint32_t i = 0; i < 4u; i++) {
		const uint32_t lvl = 6u + i;
		const Vector3i origin = swrt_trace->get_clipmap_origin(lvl);
		params.extra_origins[i][0] = origin.x;
		params.extra_origins[i][1] = origin.y;
		params.extra_origins[i][2] = origin.z;
		params.extra_origins[i][3] = swrt_trace->get_level_status(lvl);
		params.extra_voxel_size[i] = NextGIGlobalSDF::level_voxel_size(lvl);
	}
	const Basis &cam_basis = p_render_data->scene_data->cam_transform.basis;
	for (int i = 0; i < 3; i++) {
		const Vector3 col = cam_basis.get_column(i).normalized();
		params.world_to_view[i][0] = float(col.x);
		params.world_to_view[i][1] = float(col.y);
		params.world_to_view[i][2] = float(col.z);
		params.world_to_view[i][3] = 0.0f;
	}

	DFLightGPU gpu_lights[RendererVirtualShadowMapStorage::DF_MAX_LIGHTS_PER_VIEW] = {};
	for (uint32_t i = 0; i < light_count; i++) {
		const RendererRD::VirtualShadowMapStorage::DFViewLight &src = df_lights[i];
		gpu_lights[i].pos_range[0] = float(src.position.x);
		gpu_lights[i].pos_range[1] = float(src.position.y);
		gpu_lights[i].pos_range[2] = float(src.position.z);
		gpu_lights[i].pos_range[3] = src.range;
		gpu_lights[i].dir_radius[0] = float(src.direction.x);
		gpu_lights[i].dir_radius[1] = float(src.direction.y);
		gpu_lights[i].dir_radius[2] = float(src.direction.z);
		gpu_lights[i].dir_radius[3] = src.source_radius;
		gpu_lights[i].handoff_spot[0] = src.handoff_start;
		gpu_lights[i].handoff_spot[1] = src.handoff_end;
		gpu_lights[i].handoff_spot[2] = src.cos_spot_angle;
		gpu_lights[i].handoff_spot[3] = 0.0f;
		uint32_t ltype = 0u;
		switch (src.type) {
			case RSE::LIGHT_DIRECTIONAL:
				ltype = 0u;
				break;
			case RSE::LIGHT_OMNI:
				ltype = 1u;
				break;
			case RSE::LIGHT_SPOT:
				ltype = 2u;
				break;
			case RSE::LIGHT_AREA:
				ltype = 3u;
				break;
		}
		gpu_lights[i].meta[0] = ltype;
		gpu_lights[i].meta[1] = src.full_df ? 1u : 0u;
	}

	rd->buffer_update(df_params_buffer, 0, sizeof(DFParamsGPU), &params);
	rd->buffer_update(df_lights_buffer, 0, sizeof(DFLightGPU) * light_count, gpu_lights);

	const RID shader_rid = df_shader.version_get_shader(df_shader_version, 0);
	const RID normal_tex = has_normal ? p_normal_roughness : RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	Vector<RD::Uniform> set0_uniforms;
	{
		RD::Uniform u(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, df_nearest_sampler);
		u.append_id(depth_tex);
		set0_uniforms.push_back(u);
	}
	{
		RD::Uniform u(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, df_nearest_sampler);
		u.append_id(normal_tex);
		set0_uniforms.push_back(u);
	}
	set0_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, df_params_buffer));
	set0_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, df_lights_buffer));
	set0_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, mask_tex));
	set0_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, guide_tex));
	for (uint32_t i = 0; i < 4u; i++) {
		set0_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6 + i, swrt_trace->get_page_buffer(6u + i)));
	}

	const RID set0 = rd->uniform_set_create(set0_uniforms, shader_rid, 0);
	const RID set1 = swrt_trace->create_trace_uniform_set(shader_rid, 1, true, reused_nextgi ? next_gi.get_voxel_lighting() : nullptr);
	if (set0.is_valid() && set1.is_valid()) {
		RENDER_TIMESTAMP("Distance-Field Shadow Mask");
		rd->draw_command_begin_label("Distance-Field Shadow Mask");
		RD::ComputeListID clist = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(clist, df_pipeline);
		rd->compute_list_bind_uniform_set(clist, set0, 0);
		rd->compute_list_bind_uniform_set(clist, set1, 1);
		rd->compute_list_dispatch(clist, (uint32_t(half_size.x) + 7u) / 8u, (uint32_t(half_size.y) + 7u) / 8u, light_count);
		rd->compute_list_end();
		rd->draw_command_end_label();

		df_bind_mask = mask_tex;
		df_bind_guide = guide_tex;

		stats.lights_traced = light_count;
		stats.global_sdf_levels = active_levels;
		stats.sdf_instances = swrt_trace->get_instance_count();
		stats.half_width = uint32_t(half_size.x);
		stats.half_height = uint32_t(half_size.y);
		stats.detail_refinement = has_detail;
		stats.reused_nextgi_swrt = reused_nextgi;
	}
	if (set0.is_valid()) {
		rd->free_rid(set0);
	}
	if (set1.is_valid()) {
		rd->free_rid(set1);
	}
	vsm->record_df_stats(stats);
}

void RenderForwardClustered::_vsm_mark_pages(RenderDataRD *p_render_data) {
	// GodotNext: vsm_page_marking (#219, plan §21.3 contract S3).
	RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_NULL(vsm);
	ERR_FAIL_COND(rb.is_null());
	const RenderSceneDataRD *scene_data = p_render_data->scene_data;
	RENDER_TIMESTAMP("VSM Page Marking");

	// GodotNext: nextgeo_tessellation (#163). Invalidate cached VSM pages whenever VSM tessellation settings change.
	{
		const bool tess_on = bool(GLOBAL_GET("rendering/nextgeo/experimental/tessellation"));
		const bool tess_dir = bool(_vsm_setting("tessellation_directional", true));
		const bool tess_loc = bool(_vsm_setting("tessellation_local", false));
		const float tess_rate = RendererRD::nextgeo_tessellation_clamp_dicing_rate(float(GLOBAL_GET("rendering/nextgeo/tessellation/dicing_rate")));
		if (tess_on != vsm_last_tessellation_enabled || tess_dir != vsm_last_tess_directional || tess_loc != vsm_last_tess_local || (tess_on && tess_rate != vsm_last_tess_dicing_rate)) {
			vsm_last_tessellation_enabled = tess_on;
			vsm_last_tess_directional = tess_dir;
			vsm_last_tess_local = tess_loc;
			vsm_last_tess_dicing_rate = tess_rate;
			vsm->invalidate_all_maps(RendererVirtualShadowMapStorage::ENTRY_DIRTY_STATIC);
			nextgeo_cut.invalidate_vsm_hiz();
		}
	}

	RendererRD::VirtualShadowMapStorage::MarkInput input;
	input.depth_texture = rb->get_depth_texture(0);
	input.size = rb->get_internal_size();
	input.inv_projection = NextGIView::inv_projection(*scene_data);
	input.cam_transform = scene_data->cam_transform;
	// A pixel's footprint: the world size of one pixel at the point's view depth, the finer axis.
	// GodotNext: vsm_projection (#222). Shared with the projection.
	const Projection &projection = scene_data->cam_projection;
	_vsm_pixel_footprint(scene_data, input.size, input.pixel_scale, input.pixel_bias);
	input.z_far = scene_data->z_far;
	input.cluster_buffer = p_render_data->cluster_buffer;
	input.cluster_size = p_render_data->cluster_size;
	input.cluster_max_elements = p_render_data->cluster_max_elements;
	input.omni_buffer = light_storage->get_omni_light_buffer();
	input.spot_buffer = light_storage->get_spot_light_buffer();
	input.area_buffer = light_storage->get_area_light_buffer();
	input.omni_count = light_storage->get_omni_light_count();
	input.spot_count = light_storage->get_spot_light_count();
	input.area_count = light_storage->get_area_light_count();

	// Volumetric fog: the froxel grid of _update_volumetric_fog() and Fog::volumetric_fog_update().
	if (p_render_data->environment.is_valid() && environment_get_volumetric_fog_enabled(p_render_data->environment)) {
		const float ratio = float(input.size.x) / float((input.size.x + input.size.y) / 2);
		input.fog = true;
		input.fog_size = Vector3i(int32_t(float(get_volumetric_fog_size()) * ratio), int32_t(float(get_volumetric_fog_size()) / ratio), int32_t(get_volumetric_fog_depth()));
		input.fog_length = environment_get_volumetric_fog_length(p_render_data->environment);
		input.fog_spread = environment_get_volumetric_fog_detail_spread(p_render_data->environment);
		const Vector2 frustum_near_size = projection.get_viewport_half_extents();
		const Vector2 frustum_far_size = projection.get_far_plane_half_extents();
		const float z_near = projection.get_z_near();
		const float z_far = projection.get_z_far();
		input.fog_far_size = frustum_near_size.lerp(frustum_far_size, (input.fog_length - z_near) / (z_far - z_near));
		input.fog_near_size = projection.is_orthogonal() ? input.fog_far_size : frustum_near_size.maxf(0.001);
	}

	// Transparent instances: their world boxes request coarse pages.
	HashSet<const GeometryInstanceForwardClustered *> seen;
	for (const GeometryInstanceSurfaceDataCache *surface : render_list[RENDER_LIST_ALPHA].elements) {
		if (surface->owner == nullptr || seen.has(surface->owner)) {
			continue;
		}
		seen.insert(surface->owner);
		input.boxes.push_back(surface->owner->transformed_aabb);
	}

	// GodotNext: vsm_caching (#212, plan §21.3 contract S5). Pass per-scenario swept-bounds invalidation
	// boxes into mark_and_allocate() so vsm_invalidate.glsl runs before marking and allocation.
	LocalVector<RendererVirtualShadowMapStorage::InvalidationBox> inv_boxes;
	if (p_render_data->sdfgi_update_data != nullptr) {
		const RenderSDFGIUpdateData *sd = p_render_data->sdfgi_update_data;
		input.invalidation_epoch = sd->vsm_invalidation_epoch;
		input.static_mode_invalidations = sd->vsm_static_mode_invalidations;
		if (sd->vsm_invalidations != nullptr && sd->vsm_invalidation_count > 0) {
			inv_boxes.resize(sd->vsm_invalidation_count);
			for (uint32_t i = 0; i < sd->vsm_invalidation_count; i++) {
				const RenderVSMInvalidation &src = sd->vsm_invalidations[i];
				const Vector3 lo = src.world_aabb.position;
				const Vector3 hi = src.world_aabb.position + src.world_aabb.size;
				RendererVirtualShadowMapStorage::InvalidationBox &dst = inv_boxes[i];
				dst.min_pos[0] = float(lo.x);
				dst.min_pos[1] = float(lo.y);
				dst.min_pos[2] = float(lo.z);
				dst.layer_mask = src.layer_mask;
				dst.max_pos[0] = float(hi.x);
				dst.max_pos[1] = float(hi.y);
				dst.max_pos[2] = float(hi.z);
				dst.dirty_flags = src.dirty_flags;
				if ((src.dirty_flags & RendererVirtualShadowMapStorage::ENTRY_DIRTY_DYNAMIC) != 0u) {
					input.has_dynamic_casters = true;
				}
			}
			input.invalidation_boxes = inv_boxes.ptr();
			input.invalidation_box_count = inv_boxes.size();
		}
		if (!input.has_dynamic_casters && sd->vsm_shadows != nullptr) {
			for (uint32_t s = 0; s < sd->vsm_shadow_count && !input.has_dynamic_casters; s++) {
				for (uint32_t i = 0; i < sd->vsm_shadows[s].casters.size(); i++) {
					if (sd->vsm_shadows[s].casters[i].vsm_dynamic_reasons != 0u) {
						input.has_dynamic_casters = true;
						break;
					}
				}
			}
		}
	}

	vsm->mark_and_allocate(input);
}

void RenderForwardClustered::_vsm_debug_draw(const RenderDataRD *p_render_data) {
	// GodotNext: vsm_settings_debug (#221, plan §21.3.2). Viewport.DEBUG_DRAW_VSM_* over the final image.
	uint32_t mode = 0;
	switch (get_debug_draw_mode()) {
		case RSE::VIEWPORT_DEBUG_DRAW_VSM_MASK:
			mode = 0;
			break;
		case RSE::VIEWPORT_DEBUG_DRAW_VSM_MIP:
			mode = 1;
			break;
		case RSE::VIEWPORT_DEBUG_DRAW_VSM_PAGES:
			mode = 2;
			break;
		case RSE::VIEWPORT_DEBUG_DRAW_VSM_CACHE:
			mode = 3;
			break;
		case RSE::VIEWPORT_DEBUG_DRAW_VSM_OVERDRAW:
			mode = 4;
			break;
		case RSE::VIEWPORT_DEBUG_DRAW_VSM_CLIPMAP:
			mode = 5;
			break;
		default:
			return;
	}
	RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	if (vsm == nullptr || !vsm->is_active() || p_render_data == nullptr || p_render_data->render_buffers.is_null() || p_render_data->scene_data == nullptr || p_render_data->scene_data->view_count != 1) {
		return;
	}
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	const RenderSceneDataRD *scene_data = p_render_data->scene_data;
	const uint32_t table_maps = vsm->get_table_capacity();
	if (table_maps == 0 || vsm->get_map_buffers_capacity() < table_maps || vsm->get_pool_page_count() == 0) {
		return;
	}

	RendererRD::DebugEffects::VSMDebugInput input;
	input.mode = mode;
	input.depth_texture = rb->get_depth_texture(0);
	input.internal_size = rb->get_internal_size();
	input.output_size = texture_storage->render_target_get_size(rb->get_render_target());
	input.dest_fb = texture_storage->render_target_get_rd_framebuffer(rb->get_render_target());
	input.inv_projection = NextGIView::inv_projection(*scene_data);
	input.cam_transform = scene_data->cam_transform;
	// A pixel's footprint, as _vsm_mark_pages() computes it.
	const Projection &projection = scene_data->cam_projection;
	const float size_x = 2.0f / (Math::abs(float(projection.columns[0][0])) * float(MAX(input.internal_size.x, 1)));
	const float size_y = 2.0f / (Math::abs(float(projection.columns[1][1])) * float(MAX(input.internal_size.y, 1)));
	if (projection.is_orthogonal()) {
		input.pixel_bias = MIN(size_x, size_y);
	} else {
		input.pixel_scale = MIN(size_x, size_y);
	}
	const RendererVirtualShadowMapStorage::Scalability &scalability = vsm->get_scalability();
	input.lod_bias_directional = scalability.lod_bias_directional;
	input.lod_bias_local = scalability.lod_bias_local;
	input.page_table = vsm->get_page_table_buffer();
	input.table_maps = table_maps;
	input.map_data = vsm->get_map_data_buffer();
	input.pool = vsm->get_pool_texture();
	input.pool_pages = vsm->get_pool_page_count();
	input.grid_width = uint32_t(RendererVirtualShadowMapStorage::pool_page_grid(input.pool_pages).x);
	// The dirty list and the pairs only when this view wrote them: a view that marked no pages, or whose
	// lights had no non-Nanite casters, would otherwise show an earlier view's pages and draws.
	if (vsm_debug_view_marked && vsm->get_page_buffers_capacity() >= input.pool_pages) {
		input.alloc_state = vsm->get_state_buffer();
		input.dirty = vsm->get_dirty_buffer();
		if (vsm_debug_view_pairs || vsm_debug_view_nanite_pairs) {
			input.max_dispatch_groups = uint32_t(CLAMP(RD::get_singleton()->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X), uint64_t(1), uint64_t(UINT32_MAX)));
		}
		if (vsm_debug_view_pairs) {
			input.pairs = vsm_pairs_buffer;
			input.pair_capacity = vsm_pairs_capacity;
			input.cull_state = vsm_cull_state_buffer;
		}
		// GodotNext: vsm_nanite (#217). Include NextGeo (cluster, page) pairs in VSM_OVERDRAW.
		if (vsm_debug_view_nanite_pairs) {
			input.nanite_clusters[0] = nextgeo_cut.get_vsm_clusters_buffer(0);
			input.nanite_clusters[1] = nextgeo_cut.get_vsm_clusters_buffer(1);
			input.nanite_counters[0] = nextgeo_cut.get_vsm_counters_buffer(0);
			input.nanite_counters[1] = nextgeo_cut.get_vsm_counters_buffer(1);
			input.nanite_cluster_capacity = RendererRD::NEXTGEO_VSM_CLUSTER_CAPACITY;
		}
	}

	// The view's lights with virtual maps, directional lights first.
	if (p_render_data->lights != nullptr) {
		for (int pass = 0; pass < 2; pass++) {
			for (uint32_t i = 0; i < p_render_data->lights->size() && input.lights.size() < RendererRD::DebugEffects::VSM_DEBUG_MAX_LIGHTS; i++) {
				const RID light_instance = (*p_render_data->lights)[i];
				if (!light_storage->owns_light_instance(light_instance)) {
					continue;
				}
				const RID base = light_storage->light_instance_get_base_light(light_instance);
				const RSE::LightType type = light_storage->light_get_type(base);
				if ((type == RSE::LIGHT_DIRECTIONAL) != (pass == 0)) {
					continue;
				}
				const RendererVirtualShadowMapStorage::MapRange *range = vsm->map_get(light_instance);
				if (range == nullptr || range->first == RendererVirtualShadowMapStorage::MAP_NONE || range->count == 0) {
					continue;
				}
				RendererRD::DebugEffects::VSMDebugLight light;
				light.map_base = range->first;
				// GodotNext: vsm_local_lights (#216). Include per-light resolution bias and distant coarse flag.
				light.light_flags = RendererVirtualShadowMapStorage::light_flags(range->kind, range->count) |
						RendererVirtualShadowMapStorage::pack_resolution_bias(light_storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_RESOLUTION_BIAS));
				light.kind = uint32_t(range->kind);
				if (type != RSE::LIGHT_DIRECTIONAL) {
					const Transform3D transform = light_storage->light_instance_get_base_transform(light_instance);
					const Vector3 direction = -transform.basis.get_column(2).normalized();
					float reach = MAX(light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE), 0.001f);
					if (type == RSE::LIGHT_AREA) {
						reach += float(light_storage->light_area_get_size(base).length()) * 0.5f;
					}
					const Vector3 view_pos = scene_data->cam_transform.affine_inverse().xform(transform.origin);
					const float screen_radius = RendererVirtualShadowMapStorage::local_light_projected_radius(view_pos, reach, projection);
					if (RendererVirtualShadowMapStorage::is_local_light_distant(screen_radius, scalability.distant_light_screen_radius)) {
						light.light_flags |= RendererVirtualShadowMapStorage::LIGHT_FLAGS_DISTANT_COARSE;
					}
					light.inv_reach = 1.0f / reach;
					light.position[0] = float(transform.origin.x);
					light.position[1] = float(transform.origin.y);
					light.position[2] = float(transform.origin.z);
					light.cos_angle = Math::cos(Math::deg_to_rad(light_storage->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ANGLE)));
					light.direction[0] = float(direction.x);
					light.direction[1] = float(direction.y);
					light.direction[2] = float(direction.z);
				}
				input.lights.push_back(light);
			}
		}
	}
	debug_effects->draw_vsm(input);
}

// GodotNext: vsm_non_nanite (#218, plan §21.3 contract S4). Per-page GPU instance culling and indirect
// raster of the non-Nanite shadow casters into the virtual shadow map pool, and its GPU check.
bool RenderForwardClustered::is_vsm_active() const {
	const RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
	return vsm != nullptr && vsm->is_active();
}

void RenderForwardClustered::_arm_vsm_raster_check_cb(void *p_userdata) {
	RenderForwardClustered *self = static_cast<RenderForwardClustered *>(p_userdata);
	if (self != nullptr) {
		self->_free_vsm_raster_check_buffers();
	}
}

Dictionary RenderForwardClustered::_get_vsm_raster_check_cb(void *p_userdata) {
	RenderForwardClustered *self = static_cast<RenderForwardClustered *>(p_userdata);
	return self != nullptr ? self->get_vsm_raster_check_result() : Dictionary();
}

bool RenderForwardClustered::_ensure_vsm_cull_shader() {
	if (vsm_cull_ready) {
		return true;
	}
	if (vsm_cull_failed) {
		return false;
	}
	Vector<String> modes;
	modes.push_back("\n#define MODE_CLEAR\n");
	modes.push_back("\n#define MODE_COUNT_BOXES\n");
	modes.push_back("\n#define MODE_COUNT_INSTANCES\n");
	modes.push_back("\n#define MODE_SCAN\n");
	modes.push_back("\n#define MODE_WRITE_BOXES\n");
	modes.push_back("\n#define MODE_WRITE_INSTANCES\n");
	modes.push_back("\n#define MODE_FIXUP\n");
	vsm_cull_shader.initialize(modes, RendererVirtualShadowMapStorage::get_glsl_defines());
	vsm_cull_version = vsm_cull_shader.version_create();
	bool ok = vsm_cull_version.is_valid();
	for (int m = 0; m < VSM_CULL_MODE_MAX && ok; m++) {
		const RID shader_rid = vsm_cull_shader.version_get_shader(vsm_cull_version, m);
		ok = shader_rid.is_valid();
		if (ok) {
			vsm_cull_pipelines[m] = RD::get_singleton()->compute_pipeline_create(shader_rid);
			ok = vsm_cull_pipelines[m].is_valid();
		}
	}
	if (!ok) {
		if (vsm_cull_version.is_valid()) {
			vsm_cull_shader.version_free(vsm_cull_version);
			vsm_cull_version = RID();
		}
		for (RID &pipeline : vsm_cull_pipelines) {
			pipeline = RID();
		}
		vsm_cull_failed = true;
		ERR_FAIL_V_MSG(false, "The virtual shadow map non-Nanite instance culling shader did not compile.");
	}
	vsm_cull_ready = true;
	return true;
}

void RenderForwardClustered::_ensure_vsm_cull_buffers(uint32_t p_box_records, uint32_t p_sub_records, uint32_t p_groups, uint32_t p_pair_capacity, bool p_capture) {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);

	if (vsm_empty_fb.is_null()) {
		vsm_empty_fb = rd->framebuffer_create_empty(Size2i(int32_t(RendererVirtualShadowMapStorage::PAGE_SIZE), int32_t(RendererVirtualShadowMapStorage::PAGE_SIZE)));
	}
	if (vsm_cull_state_buffer.is_null()) {
		vsm_cull_state_buffer = rd->storage_buffer_create(VSM_CULL_STATE_WORDS * uint32_t(sizeof(uint32_t)));
		rd->set_resource_name(vsm_cull_state_buffer, "VSM Cull State");
		rd->buffer_clear(vsm_cull_state_buffer, 0, VSM_CULL_STATE_WORDS * uint32_t(sizeof(uint32_t)));
	}

	const uint32_t want_boxes = MAX(p_box_records, 1u);
	if (vsm_box_records_buffer.is_null() || vsm_box_records_capacity < want_boxes) {
		if (vsm_box_records_buffer.is_valid()) {
			rd->free_rid(vsm_box_records_buffer);
		}
		vsm_box_records_capacity = Math::nearest_power_of_2_templated(MAX(want_boxes, 64u));
		vsm_box_records_buffer = rd->storage_buffer_create(vsm_box_records_capacity * uint32_t(sizeof(VSMBoxCullRecord)));
		rd->set_resource_name(vsm_box_records_buffer, "VSM Box Cull Records");
	}

	const uint32_t want_subs = MAX(p_sub_records, 1u);
	if (vsm_sub_records_buffer.is_null() || vsm_sub_records_capacity < want_subs) {
		if (vsm_sub_records_buffer.is_valid()) {
			rd->free_rid(vsm_sub_records_buffer);
		}
		vsm_sub_records_capacity = Math::nearest_power_of_2_templated(MAX(want_subs, 16u));
		vsm_sub_records_buffer = rd->storage_buffer_create(vsm_sub_records_capacity * uint32_t(sizeof(VSMSubCullRecord)));
		rd->set_resource_name(vsm_sub_records_buffer, "VSM Sub Cull Records");
	}

	const uint32_t want_groups = MAX(p_groups, 1u);
	if (vsm_group_draw_meta_buffer.is_null() || vsm_groups_capacity < want_groups) {
		for (RID *buf : { &vsm_group_draw_meta_buffer, &vsm_group_counts_buffer, &vsm_group_offsets_buffer, &vsm_group_cursors_buffer, &vsm_indirect_commands_buffer, &vsm_indirect_counts_buffer }) {
			if (buf->is_valid()) {
				rd->free_rid(*buf);
				*buf = RID();
			}
		}
		vsm_groups_capacity = Math::nearest_power_of_2_templated(MAX(want_groups, 32u));
		vsm_group_draw_meta_buffer = rd->storage_buffer_create(vsm_groups_capacity * 2u * uint32_t(sizeof(uint32_t)));
		rd->set_resource_name(vsm_group_draw_meta_buffer, "VSM Group Draw Meta");
		vsm_group_counts_buffer = rd->storage_buffer_create(vsm_groups_capacity * uint32_t(sizeof(uint32_t)));
		rd->set_resource_name(vsm_group_counts_buffer, "VSM Group Counts");
		vsm_group_offsets_buffer = rd->storage_buffer_create(vsm_groups_capacity * uint32_t(sizeof(uint32_t)));
		rd->set_resource_name(vsm_group_offsets_buffer, "VSM Group Offsets");
		vsm_group_cursors_buffer = rd->storage_buffer_create(vsm_groups_capacity * uint32_t(sizeof(uint32_t)));
		rd->set_resource_name(vsm_group_cursors_buffer, "VSM Group Cursors");
		vsm_indirect_commands_buffer = rd->storage_buffer_create(vsm_groups_capacity * 5u * uint32_t(sizeof(uint32_t)), Span<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		rd->set_resource_name(vsm_indirect_commands_buffer, "VSM Indirect Commands");
		vsm_indirect_counts_buffer = rd->storage_buffer_create(vsm_groups_capacity * uint32_t(sizeof(uint32_t)), Span<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		rd->set_resource_name(vsm_indirect_counts_buffer, "VSM Indirect Counts");
	}

	const uint32_t want_pairs = MAX(p_pair_capacity, RendererVirtualShadowMapStorage::RASTER_PAIR_LIST_MIN);
	if (vsm_pairs_buffer.is_null() || vsm_pairs_capacity != want_pairs) {
		for (RID *buf : { &vsm_pairs_buffer, &vsm_pair_texels_buffer }) {
			if (buf->is_valid()) {
				rd->free_rid(*buf);
				*buf = RID();
			}
		}
		vsm_pairs_capacity = want_pairs;
		vsm_pairs_buffer = rd->storage_buffer_create(vsm_pairs_capacity * 4u * uint32_t(sizeof(uint32_t)));
		rd->set_resource_name(vsm_pairs_buffer, "VSM Instance-Page Pairs");
		// A fragment counter and an outside-vertex counter per pair, written only by a raster check's view
		// (the pair's w is then its caster).
		vsm_pair_texels_buffer = rd->storage_buffer_create((2u * vsm_pairs_capacity + 1u) * uint32_t(sizeof(uint32_t)));
		rd->set_resource_name(vsm_pair_texels_buffer, "VSM Pair Texels");
		rd->buffer_clear(vsm_pair_texels_buffer, 0, (2u * vsm_pairs_capacity + 1u) * uint32_t(sizeof(uint32_t)));
	}
	// The pairs' cull boxes are written only while a raster check captures; otherwise one unused entry.
	const uint32_t want_boxes_pairs = p_capture ? vsm_pairs_capacity : 1u;
	if (vsm_pair_boxes_buffer.is_null() || vsm_pair_boxes_capacity != want_boxes_pairs) {
		if (vsm_pair_boxes_buffer.is_valid()) {
			rd->free_rid(vsm_pair_boxes_buffer);
		}
		vsm_pair_boxes_capacity = want_boxes_pairs;
		vsm_pair_boxes_buffer = rd->storage_buffer_create(vsm_pair_boxes_capacity * 8u * uint32_t(sizeof(float)));
		rd->set_resource_name(vsm_pair_boxes_buffer, "VSM Pair Cull Boxes");
	}
}

void RenderForwardClustered::_free_vsm_raster_check_buffers() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr) {
		for (RID *buf : { &vsm_raster_check.dirty_snap, &vsm_raster_check.alloc_state_snap, &vsm_raster_check.map_state_snap, &vsm_raster_check.group_counts_snap, &vsm_raster_check.group_offsets_snap, &vsm_raster_check.indirect_commands_snap, &vsm_raster_check.indirect_counts_snap, &vsm_raster_check.cull_state_snap, &vsm_raster_check.pairs_snap, &vsm_raster_check.pair_texels_snap, &vsm_raster_check.page_table_snap, &vsm_raster_check.pool_snap }) {
			if (buf->is_valid()) {
				rd->free_rid(*buf);
				*buf = RID();
			}
		}
		for (RID &sub_snap : vsm_raster_check.sub_transform_snaps) {
			if (sub_snap.is_valid()) {
				rd->free_rid(sub_snap);
			}
		}
	}
	vsm_raster_check = VSMRasterCheckCapture();
}

void RenderForwardClustered::_free_vsm_raster_resources() {
	_free_vsm_raster_check_buffers();
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr) {
		for (RID *buf : { &vsm_box_records_buffer, &vsm_sub_records_buffer, &vsm_group_draw_meta_buffer, &vsm_group_counts_buffer, &vsm_group_offsets_buffer, &vsm_group_cursors_buffer, &vsm_indirect_commands_buffer, &vsm_indirect_counts_buffer, &vsm_pairs_buffer, &vsm_cull_state_buffer, &vsm_pair_texels_buffer, &vsm_pair_boxes_buffer, &vsm_empty_fb }) {
			if (buf->is_valid()) {
				rd->free_rid(*buf);
				*buf = RID();
			}
		}
		if (vsm_cull_version.is_valid()) {
			vsm_cull_shader.version_free(vsm_cull_version);
			vsm_cull_version = RID();
		}
	}
	for (RID &pipeline : vsm_cull_pipelines) {
		pipeline = RID();
	}
	vsm_cull_ready = false;
	vsm_box_records_capacity = 0;
	vsm_sub_records_capacity = 0;
	vsm_groups_capacity = 0;
	vsm_pairs_capacity = 0;
	vsm_pair_boxes_capacity = 0;
}

void RenderForwardClustered::_vsm_cull_state_received(const Vector<uint8_t> &p_data) {
	// GodotNext: vsm_non_nanite (#218). A frame or more after the cull: a pair list that overflowed grows.
	// Its pages were marked and are drawn again in full the next time a view requests them.
	RenderForwardClustered *self = singleton;
	if (self == nullptr) {
		return;
	}
	self->vsm_cull_state_pending = false;
	if (p_data.size() < int(VSM_CULL_STATE_WORDS * sizeof(uint32_t))) {
		return;
	}
	uint32_t state[VSM_CULL_STATE_WORDS] = {};
	memcpy(state, p_data.ptr(), sizeof(state));
	const uint32_t total = state[VSM_CULL_STATE_TOTAL_PAIRS];
	const uint32_t dropped = state[VSM_CULL_STATE_DROPPED_PAIRS];
	self->vsm_redraw_marks_total += state[VSM_CULL_STATE_REDRAW_MARKS];
	self->vsm_write_mismatch_total += state[VSM_CULL_STATE_WRITE_MISMATCH];
	self->vsm_df_far_culled_last = state[VSM_CULL_STATE_DF_FAR_CULLED];
	self->vsm_df_far_culled_total += self->vsm_df_far_culled_last;
	if (dropped == 0) {
		return;
	}
	self->vsm_overflow_frames++;
	self->vsm_dropped_pairs_total += dropped;
	const uint64_t wanted = uint64_t(total) + uint64_t(total) / 4u;
	const uint32_t grown = wanted >= uint64_t(VSM_PAIR_CAPACITY_MAX) ? VSM_PAIR_CAPACITY_MAX : Math::nearest_power_of_2_templated(uint32_t(wanted));
	if (grown > self->vsm_pair_capacity_floor) {
		self->vsm_pair_capacity_floor = grown;
	}
	if (!self->vsm_overflow_warned) {
		self->vsm_overflow_warned = true;
		WARN_PRINT(vformat("VSM: the non-Nanite instance-page pair list overflowed (%d pairs for %d places). The pages are drawn again, and the list grows to %d pairs.", total, self->vsm_last_pair_capacity, MIN(MAX(self->vsm_pair_capacity_floor, self->vsm_last_pair_capacity), VSM_PAIR_CAPACITY_MAX)));
	}
	if (self->vsm_last_pair_capacity >= VSM_PAIR_CAPACITY_MAX && !self->vsm_overflow_cap_warned) {
		self->vsm_overflow_cap_warned = true;
		WARN_PRINT(vformat("VSM: the non-Nanite pair list is at its %d-pair limit; pages that do not fit are drawn over the next frames.", VSM_PAIR_CAPACITY_MAX));
	}
}

namespace {

struct VSMGroupKey {
	RID pipeline_rd;
	RID vertex_array_rd;
	RID index_array_rd;
	RID xforms_uniform_set;
	RID material_uniform_set;
	uint32_t current_offset = 0;
	uint32_t drawn_count = 0;
	uint32_t ubershader = 0;
	uint32_t cull_mode = 0;
	uint32_t spec0 = 0;
	uint32_t spec1 = 0;
	uint32_t spec2 = 0;

	bool operator==(const VSMGroupKey &p_other) const {
		return pipeline_rd == p_other.pipeline_rd &&
				vertex_array_rd == p_other.vertex_array_rd &&
				index_array_rd == p_other.index_array_rd &&
				xforms_uniform_set == p_other.xforms_uniform_set &&
				material_uniform_set == p_other.material_uniform_set &&
				current_offset == p_other.current_offset &&
				drawn_count == p_other.drawn_count &&
				ubershader == p_other.ubershader &&
				cull_mode == p_other.cull_mode &&
				spec0 == p_other.spec0 &&
				spec1 == p_other.spec1 &&
				spec2 == p_other.spec2;
	}
};

struct VSMGroupKeyHasher {
	static _FORCE_INLINE_ uint32_t hash(const VSMGroupKey &p_key) {
		uint32_t h = hash_murmur3_one_64(p_key.pipeline_rd.get_id());
		h = hash_murmur3_one_64(p_key.vertex_array_rd.get_id(), h);
		h = hash_murmur3_one_64(p_key.index_array_rd.get_id(), h);
		h = hash_murmur3_one_64(p_key.xforms_uniform_set.get_id(), h);
		h = hash_murmur3_one_64(p_key.material_uniform_set.get_id(), h);
		h = hash_murmur3_one_32(p_key.current_offset, h);
		h = hash_murmur3_one_32(p_key.drawn_count, h);
		h = hash_murmur3_one_32(p_key.ubershader, h);
		h = hash_murmur3_one_32(p_key.cull_mode, h);
		h = hash_murmur3_one_32(p_key.spec0, h);
		h = hash_murmur3_one_32(p_key.spec1, h);
		h = hash_murmur3_one_32(p_key.spec2, h);
		return hash_fmix32(h);
	}
};

struct VSMPairTuple {
	uint32_t group = 0;
	uint32_t elem = 0;
	uint32_t sub = 0;
	uint32_t dirty = 0;
	uint32_t caster = 0;

	bool operator==(const VSMPairTuple &p_other) const {
		return group == p_other.group && elem == p_other.elem && sub == p_other.sub && dirty == p_other.dirty && caster == p_other.caster;
	}
};

struct VSMPairTupleHasher {
	static _FORCE_INLINE_ uint32_t hash(const VSMPairTuple &p_tuple) {
		uint32_t h = hash_murmur3_one_32(p_tuple.group);
		h = hash_murmur3_one_32(p_tuple.elem, h);
		h = hash_murmur3_one_32(p_tuple.sub, h);
		h = hash_murmur3_one_32(p_tuple.dirty, h);
		h = hash_murmur3_one_32(p_tuple.caster, h);
		return hash_fmix32(h);
	}
};

LocalVector<uint32_t> _vsm_read_words(RID p_buffer) {
	LocalVector<uint32_t> words;
	if (p_buffer.is_null()) {
		return words;
	}
	const Vector<uint8_t> bytes = RD::get_singleton()->buffer_get_data(p_buffer);
	const uint32_t count = uint32_t(bytes.size()) / uint32_t(sizeof(uint32_t));
	words.resize(count);
	if (count > 0) {
		memcpy(words.ptr(), bytes.ptr(), count * sizeof(uint32_t));
	}
	return words;
}

} // namespace

void RenderForwardClustered::_vsm_raster_non_nanite(RenderDataRD *p_render_data) {
	// GodotNext: vsm_non_nanite (#218, plan §21.3 contract S4, budget V3).
	RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::ParticlesStorage *particles_storage = RendererRD::ParticlesStorage::get_singleton();
	RenderingDevice *rd = RD::get_singleton();
	if (vsm == nullptr || !vsm->is_active() || rd == nullptr || p_render_data == nullptr || p_render_data->scene_data == nullptr) {
		return;
	}
	// The compositor creates the storage after the scene renderer, so the check registers here.
	vsm->set_raster_check_callbacks(&RenderForwardClustered::_arm_vsm_raster_check_cb, &RenderForwardClustered::_get_vsm_raster_check_cb, this);
	const RID pool_tex = vsm->get_pool_texture();
	const RID map_data_buf = vsm->get_map_data_buffer();
	const RID map_state_buf = vsm->get_map_state_buffer();
	const RID dirty_buf = vsm->get_dirty_buffer();
	const RID alloc_state_buf = vsm->get_state_buffer();
	const uint32_t table_maps = vsm->get_map_buffers_capacity();
	const uint32_t pool_pages = vsm->get_page_buffers_capacity();
	if (pool_tex.is_null() || map_data_buf.is_null() || map_state_buf.is_null() || dirty_buf.is_null() || alloc_state_buf.is_null() || vsm->get_page_table_buffer().is_null() || table_maps == 0 || pool_pages == 0) {
		return;
	}
	if (!_ensure_vsm_cull_shader()) {
		return;
	}
	if (!scene_shader.is_vsm_shader_group_enabled()) {
		scene_shader.enable_vsm_shader_group();
	}

	RENDER_TIMESTAMP("VSM Non-Nanite Raster");
	_update_dirty_geometry_instances();

	const bool capture = vsm->consume_raster_check_request();
	if (capture) {
		_free_vsm_raster_check_buffers();
	}

	const bool skip_virtual_for_vsm = nextgeo_virtual_active &&
			!p_render_data->reflection_probe.is_valid() &&
			RSG::virtual_mesh_storage != nullptr &&
			RSG::virtual_mesh_storage->get_live_record_count() > 0 &&
			RSG::virtual_mesh_storage->get_record_count() > 0 &&
			RSG::virtual_mesh_storage->get_record_buffer().is_valid() &&
			RSG::virtual_mesh_storage->get_max_bvh_levels() > 0 &&
			nextgeo_cut.is_vsm_available();
	// GodotNext: vsm_nanite (#217, step 7). Programmable-bin virtual surfaces hand off to VSM Nanite
	// whenever this view runs programmable raster bins; otherwise they stay on the non-Nanite VSM path.
	const bool skip_prog_for_vsm = skip_virtual_for_vsm && nextgeo_programmable_view_active;

	auto has_non_virtual_shadow_surface = [&](GeometryInstanceForwardClustered *p_inst) -> bool {
		if (p_inst == nullptr || p_inst->data == nullptr || p_inst->instance_count == 0) {
			return false;
		}
		for (const GeometryInstanceSurfaceDataCache *s = p_inst->surface_caches; s != nullptr; s = s->next) {
			if ((s->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW) == 0) {
				continue;
			}
			if (skip_virtual_for_vsm && (s->flags & GeometryInstanceSurfaceDataCache::FLAG_VIRTUAL) != 0 && (s->nextgeo_raster_bin == 0u || skip_prog_for_vsm)) {
				continue;
			}
			if (s->surface_shadow != nullptr && s->shader_shadow != nullptr) {
				return true;
			}
		}
		return false;
	};

	struct CasterLightRef {
		uint32_t map_base = RendererVirtualShadowMapStorage::MAP_NONE;
		uint32_t light_flags = 0;
		bool reverse_cull = false;
		float df_handoff_start = 0.0f;
	};
	struct UniqueCaster {
		GeometryInstanceForwardClustered *inst = nullptr;
		uint64_t object_id = 0;
		float extra_margin = 0.0f;
		bool moved_this_frame = false;
		// GodotNext: vsm_caching (#212).
		RSE::ShadowCacheInvalidation shadow_cache_invalidation = RSE::SHADOW_CACHE_INVALIDATION_AUTO;
		uint64_t vsm_last_moved_frame = RendererVirtualShadowMapStorage::CACHE_NEVER_MOVED;
		uint32_t vsm_shader_dynamic_flags = 0;
		uint32_t vsm_dynamic_reasons = 0; // RendererSceneCull's classification at the instance's last sync.
		uint32_t saved_flags = 0;
		float saved_depth = 0.0f;
		LocalVector<CasterLightRef> lights;
	};

	HashMap<GeometryInstanceForwardClustered *, uint32_t> caster_id_map;
	HashSet<GeometryInstanceForwardClustered *> virtual_skipped_set;
	LocalVector<UniqueCaster> unique_casters;
	uint32_t active_vsm_lights = 0;
	// GodotNext: vsm_local_lights (#216). Track local lights processed and indirect draws issued.
	uint32_t active_vsm_local_lights = 0;
	// Raster check only: the casters each light with virtual maps rasterizes, for the per-light atlas diff.
	HashMap<RID, HashSet<GeometryInstanceForwardClustered *>> vsm_light_casters;
	LocalVector<VSMRasterCheckCapture::Light> check_lights;

	const RenderVSMShadowData *vsm_shadows = p_render_data->sdfgi_update_data != nullptr ? p_render_data->sdfgi_update_data->vsm_shadows : nullptr;
	const uint32_t vsm_shadow_count = p_render_data->sdfgi_update_data != nullptr ? p_render_data->sdfgi_update_data->vsm_shadow_count : 0;

	for (uint32_t s = 0; s < vsm_shadow_count && vsm_shadows != nullptr; s++) {
		const RenderVSMShadowData &vs = vsm_shadows[s];
		if (!light_storage->owns_light_instance(vs.light)) {
			continue;
		}
		const RendererVirtualShadowMapStorage::MapRange *range = vsm->map_get(vs.light);
		if (range == nullptr || range->first == RendererVirtualShadowMapStorage::MAP_NONE || range->count == 0) {
			continue;
		}
		active_vsm_lights++;
		if (range->kind != RendererVirtualShadowMapStorage::MAP_KIND_DIRECTIONAL) {
			active_vsm_local_lights++;
		}
		const RID base_light = light_storage->light_instance_get_base_light(vs.light);
		const bool rev_cull = base_light.is_valid() && light_storage->light_get_reverse_cull_face_mode(base_light);
		const uint32_t lflags = RendererVirtualShadowMapStorage::light_flags(range->kind, range->count);
		const float df_handoff_start = (range->kind == RendererVirtualShadowMapStorage::MAP_KIND_DIRECTIONAL) ? vsm->get_df_handoff_start(vs.light) : 0.0f;
		HashSet<GeometryInstanceForwardClustered *> *light_casters = capture ? &vsm_light_casters[vs.light] : nullptr;
		if (capture) {
			VSMRasterCheckCapture::Light check_light;
			check_light.map_first = range->first;
			check_light.map_count = range->count;
			check_light.kind = uint32_t(range->kind);
			check_light.base_light_id = base_light.get_id();
			check_light.transform = light_storage->light_instance_get_base_transform(vs.light);
			check_lights.push_back(check_light);
		}

		for (uint32_t i = 0; i < vs.casters.size(); i++) {
			const RenderVSMCaster &c = vs.casters[i];
			GeometryInstanceForwardClustered *inst = static_cast<GeometryInstanceForwardClustered *>(c.geometry_instance);
			if (inst == nullptr || inst->data == nullptr || inst->instance_count == 0) {
				continue;
			}
			if (!has_non_virtual_shadow_surface(inst)) {
				for (const GeometryInstanceSurfaceDataCache *sc = inst->surface_caches; sc != nullptr; sc = sc->next) {
					if ((sc->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW) != 0 && skip_virtual_for_vsm && (sc->flags & GeometryInstanceSurfaceDataCache::FLAG_VIRTUAL) != 0 && (sc->nextgeo_raster_bin == 0u || skip_prog_for_vsm)) {
						virtual_skipped_set.insert(inst);
						break;
					}
				}
				continue;
			}
			if (light_casters != nullptr) {
				light_casters->insert(inst);
			}
			uint32_t *existing = caster_id_map.getptr(inst);
			uint32_t cid = 0;
			if (existing != nullptr) {
				cid = *existing;
			} else {
				cid = unique_casters.size();
				caster_id_map[inst] = cid;
				UniqueCaster uc;
				uc.inst = inst;
				uc.object_id = c.object_id;
				uc.extra_margin = c.extra_margin;
				// A set_transform() with the same value is not a move (light changes re-set transforms).
				uc.moved_this_frame = (inst->transform != inst->prev_transform) || (inst->spline_deform_active && inst->spline_changed_this_frame);
				uc.shadow_cache_invalidation = c.shadow_cache_invalidation;
				uc.vsm_last_moved_frame = c.vsm_last_moved_frame;
				uc.vsm_shader_dynamic_flags = c.vsm_shader_dynamic_flags;
				uc.vsm_dynamic_reasons = c.vsm_dynamic_reasons;
				uc.saved_flags = inst->flags_cache;
				uc.saved_depth = inst->depth;
				unique_casters.push_back(uc);
			}
			LocalVector<CasterLightRef> &clights = unique_casters[cid].lights;
			bool dup = false;
			for (const CasterLightRef &prev : clights) {
				if (prev.map_base == range->first) {
					dup = true;
					break;
				}
			}
			if (!dup) {
				CasterLightRef ref;
				ref.map_base = range->first;
				ref.light_flags = lflags;
				ref.reverse_cull = rev_cull;
				ref.df_handoff_start = df_handoff_start;
				clights.push_back(ref);
			}
		}
	}

	uint32_t atlas_non_virtual_count = 0;
	uint32_t missing_from_vsm_count = 0;
	uint32_t vsm_extra_casters_count = 0;
	if (capture) {
		HashSet<GeometryInstanceForwardClustered *> atlas_set;
		HashMap<RID, HashSet<GeometryInstanceForwardClustered *>> atlas_by_light;
		for (int s = 0; s < p_render_data->render_shadow_count && p_render_data->render_shadows != nullptr; s++) {
			const PagedArray<RenderGeometryInstance *> &arr = p_render_data->render_shadows[s].instances;
			HashSet<GeometryInstanceForwardClustered *> &light_atlas = atlas_by_light[p_render_data->render_shadows[s].light];
			for (uint32_t i = 0; i < arr.size(); i++) {
				GeometryInstanceForwardClustered *inst = static_cast<GeometryInstanceForwardClustered *>(arr[i]);
				if (has_non_virtual_shadow_surface(inst)) {
					atlas_set.insert(inst);
					light_atlas.insert(inst);
				}
			}
		}
		atlas_non_virtual_count = atlas_set.size();
		// Per light: a caster that the atlas draws for a light must be in that light's virtual maps. A light
		// without virtual maps falls back to "in any light's virtual maps".
		for (const KeyValue<RID, HashSet<GeometryInstanceForwardClustered *>> &kv : atlas_by_light) {
			// GodotNext: vsm_settings_debug (#221). A light whose shadow method keeps the atlas has no virtual maps
			// by design.
			const RID atlas_base = light_storage->owns_light_instance(kv.key) ? light_storage->light_instance_get_base_light(kv.key) : RID();
			if (atlas_base.is_valid() && !RendererVirtualShadowMapStorage::light_uses_vsm(light_storage->light_get_shadow_method(atlas_base))) {
				continue;
			}
			const HashSet<GeometryInstanceForwardClustered *> *light_casters = vsm_light_casters.getptr(kv.key);
			for (GeometryInstanceForwardClustered *inst : kv.value) {
				if (light_casters != nullptr ? !light_casters->has(inst) : !caster_id_map.has(inst)) {
					missing_from_vsm_count++;
				}
			}
		}
		for (const UniqueCaster &uc : unique_casters) {
			if (!atlas_set.has(uc.inst)) {
				vsm_extra_casters_count++;
			}
		}
	}

	// Every shadowed light of the view that has virtual maps needs a caster list from the scene cull.
	uint32_t view_vsm_lights = 0;
	LocalVector<uint64_t> lights_without_casters;
	if (capture && p_render_data->lights != nullptr) {
		HashSet<RID> listed;
		for (uint32_t s = 0; s < vsm_shadow_count && vsm_shadows != nullptr; s++) {
			listed.insert(vsm_shadows[s].light);
		}
		for (uint32_t i = 0; i < p_render_data->lights->size(); i++) {
			const RID light_instance = (*p_render_data->lights)[i];
			if (!light_storage->owns_light_instance(light_instance)) {
				continue;
			}
			const RID base_light = light_storage->light_instance_get_base_light(light_instance);
			if (base_light.is_null() || !light_storage->light_has_shadow(base_light)) {
				continue;
			}
			if (light_storage->light_get_type(base_light) == RSE::LIGHT_DIRECTIONAL && light_storage->light_directional_get_sky_mode(base_light) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
				continue;
			}
			const RendererVirtualShadowMapStorage::MapRange *range = vsm->map_get(light_instance);
			if (range == nullptr || range->first == RendererVirtualShadowMapStorage::MAP_NONE || range->count == 0) {
				continue;
			}
			view_vsm_lights++;
			if (!listed.has(light_instance)) {
				lights_without_casters.push_back(light_instance.get_id());
			}
		}
	}

	// The pair list: the pool's raster_pair_capacity() (or GODOT_DEV_VSM_PAIR_CAPACITY, to test an overflow),
	// grown after an overflow up to VSM_PAIR_CAPACITY_MAX.
#ifdef DEBUG_ENABLED
	if (!vsm_pair_capacity_env_read) {
		vsm_pair_capacity_env_read = true;
		const String value = OS::get_singleton() != nullptr ? OS::get_singleton()->get_environment("GODOT_DEV_VSM_PAIR_CAPACITY").strip_edges() : String();
		if (!value.is_empty()) {
			const int64_t requested = value.to_int();
			if (value.is_valid_int() && requested >= 64 && requested <= int64_t(VSM_PAIR_CAPACITY_MAX)) {
				vsm_pair_capacity_override = uint32_t(requested);
				print_line(vformat("VSM: GODOT_DEV_VSM_PAIR_CAPACITY is set: the non-Nanite pair list starts at %d pairs.", requested));
			} else {
				WARN_PRINT(vformat("GODOT_DEV_VSM_PAIR_CAPACITY=%s is not a whole number from 64 to %d; it is ignored.", value, VSM_PAIR_CAPACITY_MAX));
			}
		}
	}
#endif
	const uint32_t base_pair_capacity = vsm_pair_capacity_override > 0 ? vsm_pair_capacity_override : RendererVirtualShadowMapStorage::raster_pair_capacity(pool_pages);
	const uint32_t pair_capacity = MIN(MAX(base_pair_capacity, vsm_pair_capacity_floor), VSM_PAIR_CAPACITY_MAX);
	vsm_last_pair_capacity = pair_capacity;
	const float min_caster_texels = vsm->get_min_caster_texels();
	const bool use_indirect_count = rd->has_feature(RD::SUPPORTS_DRAW_INDIRECT_COUNT);

	// Save per-surface prepass velocity flags before _fill_render_list touches RENDER_LIST_SECONDARY.
	struct SavedSurfaceVelocity {
		GeometryInstanceSurfaceDataCache *surf = nullptr;
		bool uses_prepass_velocity = false;
	};
	LocalVector<SavedSurfaceVelocity> saved_surf_vel;
	vsm_cull_instances.clear();
	for (const UniqueCaster &uc : unique_casters) {
		vsm_cull_instances.push_back(uc.inst);
		for (GeometryInstanceSurfaceDataCache *sc = uc.inst->surface_caches; sc != nullptr; sc = sc->next) {
			if (sc->uses_prepass_velocity) {
				saved_surf_vel.push_back({ sc, true });
			}
		}
	}

	RenderSceneDataRD vsm_scene_data;
	vsm_scene_data.flip_y = false;
	vsm_scene_data.cam_projection.set_orthogonal(-1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f);
	vsm_scene_data.cam_transform = Transform3D();
	vsm_scene_data.view_projection[0] = vsm_scene_data.cam_projection;
	vsm_scene_data.cam_orthogonal = false;
	vsm_scene_data.z_near = 0.0f;
	vsm_scene_data.z_far = p_render_data->scene_data->z_far;
	vsm_scene_data.lod_distance_multiplier = p_render_data->scene_data->cam_projection.get_lod_multiplier();
	vsm_scene_data.screen_mesh_lod_threshold = (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_DISABLE_LOD) ? 0.0f : p_render_data->scene_data->screen_mesh_lod_threshold;
	vsm_scene_data.opaque_prepass_threshold = 0.1f;
	vsm_scene_data.time = time;
	vsm_scene_data.time_step = time_step;
	vsm_scene_data.main_cam_transform = p_render_data->scene_data->cam_transform;
	vsm_scene_data.shadow_pass = true;

	RenderDataRD vsm_rd;
	vsm_rd.scene_data = &vsm_scene_data;
	vsm_rd.cluster_size = 1;
	vsm_rd.cluster_max_elements = 32;
	vsm_rd.instances = &vsm_cull_instances;

	_update_render_base_uniform_set();

	const Size2i page_screen_size = Size2i(int32_t(RendererVirtualShadowMapStorage::PAGE_SIZE), int32_t(RendererVirtualShadowMapStorage::PAGE_SIZE));
	const uint32_t uniform_buffer_index = _setup_environment(&vsm_rd, true, page_screen_size, page_screen_size, Color(), false, false, false);

	{
		NextGeoBoolScope vsm_nextgeo_scope(nextgeo_shadow_pass_active, skip_virtual_for_vsm);
		_fill_render_list(RENDER_LIST_SECONDARY, &vsm_rd, PASS_MODE_SHADOW, false, false, false, false);
	}
	render_list[RENDER_LIST_SECONDARY].sort_by_key();
	_fill_instance_data(RENDER_LIST_SECONDARY, nullptr, 0, -1, true);

	for (const UniqueCaster &uc : unique_casters) {
		uc.inst->flags_cache = uc.saved_flags;
		uc.inst->depth = uc.saved_depth;
	}
	for (const SavedSurfaceVelocity &sv : saved_surf_vel) {
		sv.surf->uses_prepass_velocity = sv.uses_prepass_velocity;
	}

	const uint32_t element_count = render_list[RENDER_LIST_SECONDARY].elements.size();
	_ensure_vsm_cull_buffers(1u, 1u, 1u, pair_capacity, capture);
	const RD::FramebufferFormatID empty_fb_format = rd->framebuffer_get_format(vsm_empty_fb);

	LocalVector<VSMCasterTelemetry> caster_telemetry;
	if (capture) {
		caster_telemetry.resize(unique_casters.size());
		for (uint32_t c = 0; c < unique_casters.size(); c++) {
			const GeometryInstanceForwardClustered *inst = unique_casters[c].inst;
			VSMCasterTelemetry &tel = caster_telemetry[c];
			tel.object_id = unique_casters[c].object_id;
			tel.base_type = uint32_t(inst->data->base_type);
			tel.instance_count = inst->instance_count;
			tel.is_skinned = mesh_storage->skeleton_is_valid(inst->data->skeleton);
			tel.has_mesh_instance = inst->mesh_instance.is_valid();
			tel.is_multimesh = (inst->base_flags & INSTANCE_DATA_FLAG_MULTIMESH) != 0 && (inst->base_flags & INSTANCE_DATA_FLAG_PARTICLES) == 0;
			tel.is_particles = (inst->base_flags & INSTANCE_DATA_FLAG_PARTICLES) != 0;
		}
	}

	LocalVector<VSMGroupKey> groups;
	LocalVector<uint32_t> group_draw_meta; // 2 uints per group: x = drawn_count, y = indexed_flag.
	HashMap<VSMGroupKey, uint32_t, VSMGroupKeyHasher> group_map;
	LocalVector<VSMBoxCullRecord> box_records;
	LocalVector<VSMSubCullRecord> sub_records;
	LocalVector<RID> sub_record_buffers;
	LocalVector<uint32_t> sub_record_buffer_bytes;
	bool should_request_redraw = false;
	uint32_t point_size_skipped = 0;

	// The custom AABB of the mesh that a MultiMesh or particle surface belongs to, or AABB().
	auto surface_custom_aabb = [&](GeometryInstanceForwardClustered *p_inst, const GeometryInstanceSurfaceDataCache *p_surf) -> AABB {
		if ((p_inst->base_flags & INSTANCE_DATA_FLAG_PARTICLES) != 0) {
			const int passes = particles_storage->particles_get_draw_passes(p_inst->data->base);
			for (int pass = 0; pass < passes; pass++) {
				const RID pass_mesh = particles_storage->particles_get_draw_pass_mesh(p_inst->data->base, pass);
				if (pass_mesh.is_valid() && int(p_surf->surface_index) < mesh_storage->mesh_get_surface_count(pass_mesh) && mesh_storage->mesh_get_surface(pass_mesh, p_surf->surface_index) == p_surf->surface) {
					return mesh_storage->mesh_get_custom_aabb(pass_mesh);
				}
			}
			return AABB();
		}
		const RID mm_mesh = mesh_storage->_multimesh_get_mesh(p_inst->data->base);
		return mm_mesh.is_valid() ? mesh_storage->mesh_get_custom_aabb(mm_mesh) : AABB();
	};

	auto get_or_create_group = [&](const VSMGroupKey &p_key) -> uint32_t {
		uint32_t *existing = group_map.getptr(p_key);
		if (existing != nullptr) {
			return *existing;
		}
		const uint32_t idx = groups.size();
		group_map[p_key] = idx;
		groups.push_back(p_key);
		group_draw_meta.push_back(p_key.drawn_count);
		group_draw_meta.push_back(p_key.index_array_rd.is_valid() ? 1u : 0u);
		return idx;
	};

	for (uint32_t e = 0; e < element_count; e++) {
		GeometryInstanceSurfaceDataCache *surf = render_list[RENDER_LIST_SECONDARY].elements[e];
		const RenderElementInfo &elem_info = render_list[RENDER_LIST_SECONDARY].element_info[e];
		GeometryInstanceForwardClustered *inst = surf->owner;
		if (inst == nullptr || inst->instance_count == 0) {
			continue;
		}
		const uint32_t *cid_ptr = caster_id_map.getptr(inst);
		if (cid_ptr == nullptr) {
			continue;
		}
		const uint32_t cid = *cid_ptr;
		const UniqueCaster &uc = unique_casters[cid];
		void *mesh_surface = surf->surface_shadow;
		SceneShaderForwardClustered::ShaderData *shader = surf->shader_shadow;
		if (mesh_surface == nullptr || shader == nullptr) {
			continue;
		}
		// GodotNext: vsm_nanite (#217). Never double-draw surfaces handled by _vsm_raster_nanite().
		if (skip_virtual_for_vsm && (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_VIRTUAL) != 0 && (surf->nextgeo_raster_bin == 0u || skip_prog_for_vsm)) {
			continue;
		}
		if (shader->uses_time || (surf->shader != nullptr && surf->shader->uses_time)) {
			should_request_redraw = true;
		}

		const bool uses_particles = (inst->base_flags & INSTANCE_DATA_FLAG_PARTICLES) != 0;
		const bool uses_multimesh = (inst->base_flags & INSTANCE_DATA_FLAG_MULTIMESH) != 0;
		// GodotNext: vsm_caching (#212, plan §21.3 contract S5). Route static (layer 0) vs dynamic (layer 1)
		// by the instance's classification from RendererSceneCull (classify_shadow_cache_dynamic() over every
		// material of the instance, including those uses_shared_shadow_material() swaps out), as stored at the
		// instance's last sync: the classification its invalidation boxes were emitted for, so every surface
		// draws into the layer those boxes clear (a surface in the other layer would only reach new pages).
		const uint32_t dynamic_reasons = uc.vsm_dynamic_reasons;
		const bool is_dynamic = dynamic_reasons != 0u;

		if (capture) {
			VSMCasterTelemetry &tel = caster_telemetry[cid];
			tel.is_dynamic = tel.is_dynamic || is_dynamic;
			tel.dynamic_reasons |= dynamic_reasons;
			tel.shadow_cache_invalidation = uint32_t(uc.shadow_cache_invalidation);
			tel.uses_alpha_scissor = tel.uses_alpha_scissor || shader->uses_discard || shader->uses_alpha_clip;
			tel.uses_vertex_displacement = tel.uses_vertex_displacement || shader->uses_vertex || (surf->shader != nullptr && surf->shader->uses_vertex);
		}

		// The VSM variant reads its pair from gl_InstanceIndex, which point-size emulation needs for the
		// vertex: a surface that sets POINT_SIZE on a device without point sizes (D3D12) is not drawn.
		if (shader->uses_point_size && scene_shader.emulate_point_size) {
			point_size_skipped++;
			continue;
		}
		const uint32_t lod_index = elem_info.lod_index;
		const RID index_array_rd = mesh_storage->mesh_surface_get_index_array(mesh_surface, lod_index);
		const uint32_t drawn_count = mesh_storage->mesh_surface_get_lod_drawn_count(mesh_surface, lod_index);
		if (drawn_count == 0) {
			continue;
		}

		uint32_t current_offset = 0;
		uint32_t prev_offset_unused = 0;
		if (uses_particles) {
			particles_storage->particles_get_instance_buffer_motion_vectors_offsets(inst->data->base, current_offset, prev_offset_unused);
		} else if (uses_multimesh) {
			mesh_storage->_multimesh_get_motion_vectors_offsets(inst->data->base, current_offset, prev_offset_unused);
		}

		SceneShaderForwardClustered::ShaderSpecialization spec = {};
		spec.multimesh = uses_multimesh;
		spec.multimesh_format_2d = (inst->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D) != 0;
		spec.multimesh_has_color = (inst->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR) != 0;
		spec.multimesh_has_custom_data = (inst->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA) != 0;

		auto resolve_group_for_rev_cull = [&](bool p_rev_cull, uint32_t &r_group) -> bool {
			SceneShaderForwardClustered::ShaderData::CullVariant cull_variant = SceneShaderForwardClustered::ShaderData::CULL_VARIANT_DOUBLE_SIDED;
			if ((surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_DOUBLE_SIDED_SHADOWS) == 0 && shader->cull_mode != RSE::CULL_MODE_DISABLED) {
				const bool flip_cull = !p_rev_cull;
				const bool mirror = inst->mirror != flip_cull;
				cull_variant = mirror ? SceneShaderForwardClustered::ShaderData::CULL_VARIANT_REVERSED : SceneShaderForwardClustered::ShaderData::CULL_VARIANT_NORMAL;
			}
			const RD::PolygonCullMode cull_mode = shader->get_cull_mode_from_cull_variant(cull_variant);

			SceneShaderForwardClustered::ShaderData::PipelineKey pipeline_key;
			pipeline_key.framebuffer_format_id = empty_fb_format;
			pipeline_key.primitive_type = surf->primitive;
			pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_VSM_DEPTH;
			pipeline_key.color_pass_flags = 0;
			pipeline_key.wireframe = false;
			pipeline_key.ubershader = 0;
			pipeline_key.zero_velocity_write = 0;

			RID vertex_array_rd;
			RID pipeline_rd;
			while (pipeline_key.ubershader < 2) {
				RD::VertexFormatID vertex_format = -1;
				const uint64_t input_mask = shader->get_vertex_input_mask(pipeline_key.version, 0, pipeline_key.ubershader);
				if (inst->mesh_instance.is_valid()) {
					mesh_storage->mesh_instance_surface_get_vertex_arrays_and_format(inst->mesh_instance, surf->surface_index, input_mask, false, false, vertex_array_rd, vertex_format);
				} else {
					mesh_storage->mesh_surface_get_vertex_arrays_and_format(mesh_surface, input_mask, false, false, vertex_array_rd, vertex_format);
				}
				pipeline_key.vertex_format_id = vertex_format;
				if (pipeline_key.ubershader) {
					pipeline_key.shader_specialization = {};
					pipeline_key.cull_mode = RD::POLYGON_CULL_DISABLED;
				} else {
					pipeline_key.shader_specialization = spec;
					pipeline_key.cull_mode = cull_mode;
				}
				const uint32_t pipeline_hash = pipeline_key.hash();
				const RSE::PipelineSource p_source = pipeline_key.ubershader ? RSE::PIPELINE_SOURCE_DRAW : RSE::PIPELINE_SOURCE_SPECIALIZATION;
				pipeline_rd = shader->pipeline_hash_map.get_pipeline(pipeline_key, pipeline_hash, pipeline_key.ubershader, p_source);
				if (pipeline_rd.is_valid()) {
					break;
				}
				pipeline_key.ubershader++;
			}
			if (pipeline_rd.is_null() || vertex_array_rd.is_null()) {
				return false;
			}
			VSMGroupKey gkey;
			gkey.pipeline_rd = pipeline_rd;
			gkey.vertex_array_rd = vertex_array_rd;
			gkey.index_array_rd = index_array_rd;
			gkey.xforms_uniform_set = inst->transforms_uniform_set;
			gkey.material_uniform_set = surf->material_uniform_set_shadow;
			gkey.current_offset = current_offset;
			gkey.drawn_count = drawn_count;
			gkey.ubershader = pipeline_key.ubershader;
			gkey.cull_mode = pipeline_key.ubershader ? uint32_t(cull_mode) : 0u;
			gkey.spec0 = pipeline_key.ubershader ? spec.packed_0 : 0u;
			gkey.spec1 = pipeline_key.ubershader ? spec.packed_1 : 0u;
			gkey.spec2 = pipeline_key.ubershader ? spec.packed_2 : 0u;
			r_group = get_or_create_group(gkey);
			return true;
		};

		uint32_t cached_group[2] = { UINT32_MAX, UINT32_MAX };
		bool group_checked[2] = { false, false };

		const uint32_t record_caster_id = capture ? cid : 0xFFFFFFFFu;
		const uint32_t is_dyn_u = is_dynamic ? 1u : 0u;
		// A billboard (MODELVIEW_MATRIX written) turns the mesh about its instance's origin, which the mesh's
		// box through the transform does not hold: the cull box then also covers the largest |x| + |y| + |z|
		// of a corner around that origin.
		const bool view_facing = shader->writes_modelview_or_projection;

		if (uses_multimesh) {
			RID sub_buf;
			uint32_t stride_vec4 = 0;
			const bool format_2d = (inst->base_flags & INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D) != 0;
			const bool uses_trails = uses_particles && (surf->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_PARTICLE_TRAILS) != 0 && inst->trail_steps > 1;
			const uint32_t effective_trail_steps = uses_trails ? inst->trail_steps : 1u;
			const uint32_t sub_count = uses_trails ? (inst->instance_count / inst->trail_steps) : inst->instance_count;
			if (uses_particles) {
				sub_buf = particles_storage->particles_get_instance_buffer(inst->data->base);
				stride_vec4 = 5u;
			} else {
				sub_buf = mesh_storage->_multimesh_get_buffer_rd_rid(inst->data->base);
				stride_vec4 = mesh_storage->multimesh_get_stride(inst->data->base) / 4u;
			}
			if (sub_buf.is_null() || sub_count == 0 || stride_vec4 == 0) {
				continue;
			}
			// Mesh.custom_aabb replaces the surface's box, as it replaces the mesh's box in the scene cull.
			// MultiMesh.custom_aabb bounds the whole MultiMesh, not an instance, so it is not used here.
			AABB local_aabb = mesh_storage->mesh_surface_get_aabb(mesh_surface);
			const AABB custom_aabb = surface_custom_aabb(inst, surf);
			if (custom_aabb != AABB()) {
				local_aabb = custom_aabb;
			}
			const Vector3 lmin = local_aabb.position;
			const Vector3 lmax = local_aabb.position + local_aabb.size;
			const float corner_reach = RendererVirtualShadowMapStorage::view_facing_reach(local_aabb);
			const SceneState::InstanceData &idata = scene_state.curr_gpu_ptr[RENDER_LIST_SECONDARY][e];
			const uint32_t sub_words = uses_trails ? (sub_count * effective_trail_steps * 5u * 4u) : ((current_offset + sub_count) * stride_vec4 * 4u);
			const uint32_t sub_bytes = sub_words * uint32_t(sizeof(float));

			for (const CasterLightRef &lref : uc.lights) {
				const int rc_idx = lref.reverse_cull ? 1 : 0;
				if (!group_checked[rc_idx]) {
					group_checked[rc_idx] = true;
					uint32_t resolved = UINT32_MAX;
					if (resolve_group_for_rev_cull(lref.reverse_cull, resolved)) {
						cached_group[rc_idx] = resolved;
					}
				}
				if (cached_group[rc_idx] == UINT32_MAX) {
					continue;
				}
				VSMSubCullRecord rec;
				rec.meta0[0] = e;
				rec.meta0[1] = cached_group[rc_idx];
				rec.meta0[2] = lref.map_base;
				rec.meta0[3] = lref.light_flags;
				rec.meta1[0] = is_dyn_u;
				rec.meta1[1] = record_caster_id;
				rec.meta1[2] = sub_count;
				rec.meta1[3] = current_offset;
				rec.meta2[0] = stride_vec4;
				rec.meta2[1] = format_2d ? 1u : 0u;
				rec.meta2[2] = effective_trail_steps;
				rec.meta2[3] = view_facing ? VSM_SUB_FLAG_VIEW_FACING : 0u;
				rec.local_min[0] = float(lmin.x);
				rec.local_min[1] = float(lmin.y);
				rec.local_min[2] = float(lmin.z);
				rec.local_min[3] = MAX(uc.extra_margin, 0.0f);
				rec.local_max[0] = float(lmax.x);
				rec.local_max[1] = float(lmax.y);
				rec.local_max[2] = float(lmax.z);
				rec.local_max[3] = corner_reach;
				memcpy(rec.model_row0, &idata.transform[0], 4 * sizeof(float));
				memcpy(rec.model_row1, &idata.transform[4], 4 * sizeof(float));
				memcpy(rec.model_row2, &idata.transform[8], 4 * sizeof(float));
				rec.df_params[0] = lref.df_handoff_start;
				sub_records.push_back(rec);
				sub_record_buffers.push_back(sub_buf);
				sub_record_buffer_bytes.push_back(sub_bytes);
			}
		} else {
			// The scene cull already grew this box by extra_cull_margin (RendererSceneCull::_update_instance_aabb()).
			AABB world_box = inst->transformed_aabb;
			if (view_facing) {
				// A MeshInstance3D with a billboard material keeps the mesh's flat box (Sprite3D and Label3D
				// enlarge theirs): add the same bound as for sub-instances, times max(1, the longest axis) for
				// Keep Scale, plus the margin.
				AABB local_aabb = mesh_storage->mesh_surface_get_aabb(mesh_surface);
				if (mesh_storage->owns_mesh(inst->data->base)) {
					const AABB custom_aabb = mesh_storage->mesh_get_custom_aabb(inst->data->base);
					if (custom_aabb != AABB()) {
						local_aabb = custom_aabb;
					}
				}
				const Basis &basis = inst->transform.basis;
				const float scale = MAX(1.0f, float(MAX(basis.get_column(0).length(), MAX(basis.get_column(1).length(), basis.get_column(2).length()))));
				const float reach = RendererVirtualShadowMapStorage::view_facing_reach(local_aabb) * scale + MAX(uc.extra_margin, 0.0f);
				world_box.merge_with(AABB(inst->transform.origin - Vector3(reach, reach, reach), Vector3(reach, reach, reach) * 2.0f));
			}
			const Vector3 bmin = world_box.position;
			const Vector3 bmax = world_box.position + world_box.size;

			for (const CasterLightRef &lref : uc.lights) {
				const int rc_idx = lref.reverse_cull ? 1 : 0;
				if (!group_checked[rc_idx]) {
					group_checked[rc_idx] = true;
					uint32_t resolved = UINT32_MAX;
					if (resolve_group_for_rev_cull(lref.reverse_cull, resolved)) {
						cached_group[rc_idx] = resolved;
					}
				}
				if (cached_group[rc_idx] == UINT32_MAX) {
					continue;
				}
				VSMBoxCullRecord rec;
				rec.meta0[0] = e;
				rec.meta0[1] = cached_group[rc_idx];
				rec.meta0[2] = lref.map_base;
				rec.meta0[3] = lref.light_flags;
				rec.meta1[0] = is_dyn_u;
				rec.meta1[1] = record_caster_id;
				rec.meta1[2] = 0u;
				rec.meta1[3] = 0u;
				rec.box_min[0] = float(bmin.x);
				rec.box_min[1] = float(bmin.y);
				rec.box_min[2] = float(bmin.z);
				rec.box_min[3] = lref.df_handoff_start;
				rec.box_max[0] = float(bmax.x);
				rec.box_max[1] = float(bmax.y);
				rec.box_max[2] = float(bmax.z);
				rec.box_max[3] = 0.0f;
				box_records.push_back(rec);
			}
		}
	}

	if (should_request_redraw) {
		RenderingServerDefault::redraw_request();
	}

	_ensure_vsm_cull_buffers(box_records.size(), sub_records.size(), groups.size(), pair_capacity, capture);

	if (!box_records.is_empty()) {
		rd->buffer_update(vsm_box_records_buffer, 0, box_records.size() * uint32_t(sizeof(VSMBoxCullRecord)), box_records.ptr());
	}
	if (!sub_records.is_empty()) {
		rd->buffer_update(vsm_sub_records_buffer, 0, sub_records.size() * uint32_t(sizeof(VSMSubCullRecord)), sub_records.ptr());
	}
	if (!group_draw_meta.is_empty()) {
		rd->buffer_update(vsm_group_draw_meta_buffer, 0, group_draw_meta.size() * uint32_t(sizeof(uint32_t)), group_draw_meta.ptr());
	}
	if (capture) {
		rd->buffer_clear(vsm_pair_texels_buffer, 0, (pair_capacity + 1u) * uint32_t(sizeof(uint32_t)));
	}

	rd->draw_command_begin_label("VSM Non-Nanite Cull and Raster");

	uint32_t indirect_draw_count = 0;
	if (!groups.is_empty()) {
		LocalVector<RD::Uniform> cull_uniforms;
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, map_data_buf));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, map_state_buf));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, dirty_buf));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, alloc_state_buf));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, vsm_box_records_buffer));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, vsm_sub_records_buffer));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, vsm_group_draw_meta_buffer));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, vsm_group_counts_buffer));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, vsm_group_offsets_buffer));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, vsm_group_cursors_buffer));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, vsm_pairs_buffer));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, vsm_indirect_commands_buffer));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, vsm_indirect_counts_buffer));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, vsm_cull_state_buffer));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, vsm->get_page_table_buffer()));
		cull_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, vsm_pair_boxes_buffer));

		VSMCullPushConstant push;
		push.table_maps = table_maps;
		push.pool_pages = pool_pages;
		push.group_count = groups.size();
		push.pair_capacity = pair_capacity;
		push.box_record_count = box_records.size();
		push.sub_record_index = 0;
		push.min_caster_texels = min_caster_texels;
		push.flags = capture ? VSM_CULL_FLAG_CAPTURE : 0u;
		const Vector3 cam_origin = p_render_data->scene_data->cam_transform.origin;
		push.camera_pos_unused[0] = float(cam_origin.x);
		push.camera_pos_unused[1] = float(cam_origin.y);
		push.camera_pos_unused[2] = float(cam_origin.z);
		push.camera_pos_unused[3] = 0.0f;

		auto bind_cull = [&](RD::ComputeListID p_list, VSMCullMode p_mode) -> RID {
			const RID shader_rid = vsm_cull_shader.version_get_shader(vsm_cull_version, p_mode);
			rd->compute_list_bind_compute_pipeline(p_list, vsm_cull_pipelines[p_mode]);
			rd->compute_list_bind_uniform_set(p_list, UniformSetCacheRD::get_singleton()->get_cache_vec(shader_rid, 0, cull_uniforms), 0);
			return shader_rid;
		};

		RD::ComputeListID clist = rd->compute_list_begin();
		bind_cull(clist, VSM_CULL_MODE_CLEAR);
		rd->compute_list_set_push_constant(clist, &push, sizeof(VSMCullPushConstant));
		rd->compute_list_dispatch_threads(clist, MAX(groups.size(), 1u), 1, 1);
		rd->compute_list_add_barrier(clist);

		if (!box_records.is_empty()) {
			bind_cull(clist, VSM_CULL_MODE_COUNT_BOXES);
			rd->compute_list_set_push_constant(clist, &push, sizeof(VSMCullPushConstant));
			rd->compute_list_dispatch_threads(clist, box_records.size(), 1, 1);
		}
		if (!sub_records.is_empty()) {
			const RID shader_rid = bind_cull(clist, VSM_CULL_MODE_COUNT_INSTANCES);
			for (uint32_t sr = 0; sr < sub_records.size(); sr++) {
				push.sub_record_index = sr;
				RD::Uniform u_sub(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, sub_record_buffers[sr]);
				rd->compute_list_bind_uniform_set(clist, UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 1, u_sub), 1);
				rd->compute_list_set_push_constant(clist, &push, sizeof(VSMCullPushConstant));
				rd->compute_list_dispatch_threads(clist, sub_records[sr].meta1[2], 1, 1);
			}
			push.sub_record_index = 0;
		}
		rd->compute_list_add_barrier(clist);

		bind_cull(clist, VSM_CULL_MODE_SCAN);
		rd->compute_list_set_push_constant(clist, &push, sizeof(VSMCullPushConstant));
		rd->compute_list_dispatch(clist, 1, 1, 1);
		rd->compute_list_add_barrier(clist);

		if (!box_records.is_empty()) {
			bind_cull(clist, VSM_CULL_MODE_WRITE_BOXES);
			rd->compute_list_set_push_constant(clist, &push, sizeof(VSMCullPushConstant));
			rd->compute_list_dispatch_threads(clist, box_records.size(), 1, 1);
		}
		if (!sub_records.is_empty()) {
			const RID shader_rid = bind_cull(clist, VSM_CULL_MODE_WRITE_INSTANCES);
			for (uint32_t sr = 0; sr < sub_records.size(); sr++) {
				push.sub_record_index = sr;
				RD::Uniform u_sub(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, sub_record_buffers[sr]);
				rd->compute_list_bind_uniform_set(clist, UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 1, u_sub), 1);
				rd->compute_list_set_push_constant(clist, &push, sizeof(VSMCullPushConstant));
				rd->compute_list_dispatch_threads(clist, sub_records[sr].meta1[2], 1, 1);
			}
			push.sub_record_index = 0;
		}
		rd->compute_list_add_barrier(clist);

		bind_cull(clist, VSM_CULL_MODE_FIXUP);
		rd->compute_list_set_push_constant(clist, &push, sizeof(VSMCullPushConstant));
		rd->compute_list_dispatch_threads(clist, groups.size(), 1, 1);
		rd->compute_list_end();
		// GodotNext: vsm_settings_debug (#221). vsm_pairs_buffer and its count now belong to this view.
		vsm_debug_view_pairs = true;

		const RID vsm_rp_uniform_set = _setup_vsm_render_pass_uniform_set(RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), uniform_buffer_index);
		if (vsm_rp_uniform_set.is_valid()) {
			RD::DrawListID dlist = rd->draw_list_begin(vsm_empty_fb, RD::DRAW_DEFAULT_ALL);
			rd->draw_list_bind_uniform_set(dlist, render_base_uniform_set, SCENE_UNIFORM_SET);
			rd->draw_list_bind_uniform_set(dlist, vsm_rp_uniform_set, RENDER_PASS_UNIFORM_SET);
			rd->draw_list_bind_uniform_set(dlist, scene_shader.default_vec4_xform_uniform_set, TRANSFORMS_UNIFORM_SET);

			RID prev_pipeline;
			RID prev_vertex_array;
			RID prev_index_array;
			RID prev_xforms_set = scene_shader.default_vec4_xform_uniform_set;
			RID prev_material_set;

			for (uint32_t g = 0; g < groups.size(); g++) {
				const VSMGroupKey &grp = groups[g];
				if (prev_vertex_array != grp.vertex_array_rd) {
					rd->draw_list_bind_vertex_array(dlist, grp.vertex_array_rd);
					prev_vertex_array = grp.vertex_array_rd;
				}
				if (prev_index_array != grp.index_array_rd) {
					if (grp.index_array_rd.is_valid()) {
						rd->draw_list_bind_index_array(dlist, grp.index_array_rd);
					}
					prev_index_array = grp.index_array_rd;
				}
				if (prev_pipeline != grp.pipeline_rd) {
					rd->draw_list_bind_render_pipeline(dlist, grp.pipeline_rd);
					prev_pipeline = grp.pipeline_rd;
				}
				const RID want_xforms = grp.xforms_uniform_set.is_valid() ? grp.xforms_uniform_set : scene_shader.default_vec4_xform_uniform_set;
				if (prev_xforms_set != want_xforms) {
					rd->draw_list_bind_uniform_set(dlist, want_xforms, TRANSFORMS_UNIFORM_SET);
					prev_xforms_set = want_xforms;
				}
				if (prev_material_set != grp.material_uniform_set) {
					if (grp.material_uniform_set.is_valid() && rd->uniform_set_is_valid(grp.material_uniform_set)) {
						rd->draw_list_bind_uniform_set(dlist, grp.material_uniform_set, MATERIAL_UNIFORM_SET);
					}
					prev_material_set = grp.material_uniform_set;
				}

				SceneState::PushConstant push_constant = {};
				push_constant.base_index = 0;
				push_constant.uv_offset = g;
				push_constant.multimesh_motion_vectors_current_offset = grp.current_offset;
				push_constant.multimesh_motion_vectors_previous_offset = 0;
				size_t push_size = sizeof(SceneState::PushConstant) - sizeof(SceneState::PushConstantUbershader);
				if (grp.ubershader) {
					push_constant.ubershader.specialization.packed_0 = grp.spec0;
					push_constant.ubershader.specialization.packed_1 = grp.spec1;
					push_constant.ubershader.specialization.packed_2 = grp.spec2;
					push_constant.ubershader.constants = {};
					push_constant.ubershader.constants.cull_mode = grp.cull_mode;
					push_size = sizeof(SceneState::PushConstant);
				}
				rd->draw_list_set_push_constant(dlist, &push_constant, push_size);

				const bool use_indices = grp.index_array_rd.is_valid();
				const uint32_t cmd_offset = g * 5u * uint32_t(sizeof(uint32_t));
				const uint32_t cnt_offset = g * uint32_t(sizeof(uint32_t));
				if (use_indirect_count) {
					rd->draw_list_draw_indirect_count(dlist, use_indices, vsm_indirect_commands_buffer, cmd_offset, vsm_indirect_counts_buffer, cnt_offset, 1, 0);
				} else {
					rd->draw_list_draw_indirect(dlist, use_indices, vsm_indirect_commands_buffer, cmd_offset, 1, 0);
				}
				indirect_draw_count++;
			}
			rd->draw_list_end();
		}

		// The cull state reaches _vsm_cull_state_received() a frame or more late: an overflow grows the list.
		if (!vsm_cull_state_pending) {
			vsm_cull_state_pending = rd->buffer_get_data_async(vsm_cull_state_buffer, callable_mp_static(&RenderForwardClustered::_vsm_cull_state_received), 0, VSM_CULL_STATE_WORDS * uint32_t(sizeof(uint32_t))) == OK;
		}
	}

	rd->draw_command_end_label();

	Dictionary stats;
	stats["light_count"] = int64_t(active_vsm_lights);
	// GodotNext: vsm_local_lights (#216).
	stats["local_light_count"] = int64_t(active_vsm_local_lights);
	stats["indirect_draw_count"] = int64_t(indirect_draw_count);
	stats["caster_count"] = int64_t(unique_casters.size());
	stats["surface_element_count"] = int64_t(element_count);
	stats["group_count"] = int64_t(groups.size());
	stats["box_record_count"] = int64_t(box_records.size());
	stats["sub_record_count"] = int64_t(sub_records.size());
	stats["pair_capacity"] = int64_t(pair_capacity);
	stats["min_caster_texels"] = double(min_caster_texels);
	stats["used_indirect_count"] = use_indirect_count;
	stats["point_size_skipped"] = int64_t(point_size_skipped);
	stats["pair_capacity_floor"] = int64_t(vsm_pair_capacity_floor);
	stats["overflow_frames"] = int64_t(vsm_overflow_frames);
	stats["dropped_pairs_total"] = int64_t(vsm_dropped_pairs_total);
	stats["redraw_marks_total"] = int64_t(vsm_redraw_marks_total);
	stats["write_mismatch_total"] = int64_t(vsm_write_mismatch_total);
	stats["df_far_culled"] = int64_t(vsm_df_far_culled_last);
	stats["df_far_culled_total"] = int64_t(vsm_df_far_culled_total);
	vsm->set_last_raster_stats(stats);

	if (capture) {
		vsm_raster_check.captured = true;
		vsm_raster_check.used_indirect_count = use_indirect_count;
		vsm_raster_check.table_maps = table_maps;
		vsm_raster_check.pool_pages = pool_pages;
		vsm_raster_check.pair_capacity = pair_capacity;
		vsm_raster_check.min_caster_texels = min_caster_texels;
		vsm_raster_check.camera_position = p_render_data->scene_data->cam_transform.origin;
		vsm_raster_check.light_count = active_vsm_lights;
		vsm_raster_check.surface_element_count = element_count;
		vsm_raster_check.atlas_non_virtual_casters = atlas_non_virtual_count;
		vsm_raster_check.virtual_skipped = virtual_skipped_set.size();
		vsm_raster_check.missing_from_vsm = missing_from_vsm_count;
		vsm_raster_check.vsm_extra_casters = vsm_extra_casters_count;
		vsm_raster_check.point_size_skipped = point_size_skipped;
		vsm_raster_check.view_vsm_lights = view_vsm_lights;
		vsm_raster_check.lights_without_casters = lights_without_casters;
		vsm_raster_check.lights = check_lights;
		vsm_raster_check.frame = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0;
		vsm_raster_check.map_data = vsm->get_map_data();
		vsm_raster_check.box_records = box_records;
		vsm_raster_check.sub_records = sub_records;
		vsm_raster_check.group_draw_meta = group_draw_meta;
		vsm_raster_check.casters = caster_telemetry;

		HashMap<RID, uint32_t> snap_by_buf;
		vsm_raster_check.sub_record_snap_index.resize(sub_records.size());
		for (uint32_t sr = 0; sr < sub_records.size(); sr++) {
			uint32_t *existing = snap_by_buf.getptr(sub_record_buffers[sr]);
			if (existing != nullptr) {
				vsm_raster_check.sub_record_snap_index[sr] = *existing;
			} else {
				const uint32_t snap_idx = vsm_raster_check.sub_transform_snaps.size();
				snap_by_buf[sub_record_buffers[sr]] = snap_idx;
				vsm_raster_check.sub_record_snap_index[sr] = snap_idx;
				const uint32_t buf_bytes = sub_record_buffer_bytes[sr];
				vsm_raster_check.sub_transform_snaps.push_back(vsm->snapshot_buffer(sub_record_buffers[sr], 0, buf_bytes, "Raster Sub Transforms"));
			}
		}

		vsm_raster_check.dirty_snap = vsm->snapshot_buffer(dirty_buf, 0, pool_pages * 4u * uint32_t(sizeof(uint32_t)), "Raster Dirty");
		vsm_raster_check.alloc_state_snap = vsm->snapshot_buffer(alloc_state_buf, 0, RendererVirtualShadowMapStorage::STATE_SIZE * uint32_t(sizeof(uint32_t)), "Raster Alloc State");
		vsm_raster_check.map_state_snap = vsm->snapshot_buffer(map_state_buf, 0, table_maps * RendererVirtualShadowMapStorage::MAP_STATE_REGIONS * uint32_t(sizeof(uint32_t)), "Raster Map State");
		if (!groups.is_empty()) {
			const uint32_t gcount = groups.size();
			vsm_raster_check.group_counts_snap = vsm->snapshot_buffer(vsm_group_counts_buffer, 0, gcount * uint32_t(sizeof(uint32_t)), "Raster Group Counts");
			vsm_raster_check.group_offsets_snap = vsm->snapshot_buffer(vsm_group_offsets_buffer, 0, gcount * uint32_t(sizeof(uint32_t)), "Raster Group Offsets");
			vsm_raster_check.indirect_commands_snap = vsm->snapshot_buffer(vsm_indirect_commands_buffer, 0, gcount * 5u * uint32_t(sizeof(uint32_t)), "Raster Indirect Cmds");
			vsm_raster_check.indirect_counts_snap = vsm->snapshot_buffer(vsm_indirect_counts_buffer, 0, gcount * uint32_t(sizeof(uint32_t)), "Raster Indirect Counts");
			vsm_raster_check.cull_state_snap = vsm->snapshot_buffer(vsm_cull_state_buffer, 0, VSM_CULL_STATE_WORDS * uint32_t(sizeof(uint32_t)), "Raster Cull State");
			vsm_raster_check.pairs_snap = vsm->snapshot_buffer(vsm_pairs_buffer, 0, pair_capacity * 4u * uint32_t(sizeof(uint32_t)), "Raster Pairs");
			vsm_raster_check.pair_texels_snap = vsm->snapshot_buffer(vsm_pair_texels_buffer, 0, (2u * pair_capacity + 1u) * uint32_t(sizeof(uint32_t)), "Raster Pair Texels");
		}
		const uint32_t page_table_maps = MIN(table_maps, vsm->get_table_capacity());
		if (vsm->get_page_table_buffer().is_valid() && page_table_maps > 0) {
			vsm_raster_check.page_table_maps = page_table_maps;
			vsm_raster_check.page_table_snap = vsm->snapshot_buffer(vsm->get_page_table_buffer(), 0, page_table_maps * uint32_t(RendererVirtualShadowMapStorage::MAP_TABLE_BYTES), "Raster Page Table");
		}
		vsm_raster_check.pool_snap = vsm->snapshot_pool("Raster Pool");
	}
}

void RenderForwardClustered::_vsm_raster_nanite(RenderDataRD *p_render_data) {
	// GodotNext: vsm_nanite (#217, plan §21.3 contract S4). Per-page multi-view cluster cull, hybrid
	// HW/SW rasterization (1 indirect chain for directional maps, 1 for local maps), 1-level per-page
	// Hi-Z build, and programmable raster-bin integration shared with #155.
	RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	// Keep per-page static Hi-Z in sync whenever this view marked/updated VSM pages but skips Nanite raster
	// (e.g. an MSAA SubViewport or a frame with 0 live virtual instances).
	auto sync_hiz_without_nanite = [&]() {
		if (!vsm_debug_view_marked) {
			return;
		}
		if (vsm == nullptr || !vsm->is_active()) {
			nextgeo_cut.invalidate_vsm_hiz();
			return;
		}
		const uint32_t tm = vsm->get_map_buffers_capacity();
		const uint32_t pp = vsm->get_page_buffers_capacity();
		const uint32_t gw = pp > 0u ? uint32_t(RendererVirtualShadowMapStorage::pool_page_grid(pp).x) : 0u;
		nextgeo_cut.update_vsm_hiz(vsm->get_pool_texture(), vsm->get_dirty_buffer(), vsm->get_state_buffer(), vsm->get_indirect_buffer(), tm, pp, gw, vsm->get_pool_reallocation_count());
	};
	if (vsm == nullptr || !vsm->is_active() || light_storage == nullptr || p_render_data == nullptr || p_render_data->scene_data == nullptr || p_render_data->reflection_probe.is_valid()) {
		sync_hiz_without_nanite();
		return;
	}
	const bool virtual_live = nextgeo_virtual_active &&
			RSG::virtual_mesh_storage != nullptr &&
			RSG::virtual_mesh_storage->get_live_record_count() > 0 &&
			RSG::virtual_mesh_storage->get_record_count() > 0 &&
			RSG::virtual_mesh_storage->get_record_buffer().is_valid() &&
			RSG::virtual_mesh_storage->get_max_bvh_levels() > 0;
	if (!virtual_live) {
		sync_hiz_without_nanite();
		return;
	}
	const uint32_t table_maps = vsm->get_map_buffers_capacity();
	const uint32_t pool_pages = vsm->get_page_buffers_capacity();
	if (table_maps == 0u || pool_pages == 0u || vsm->get_pool_texture().is_null() || vsm->get_page_table_buffer().is_null() ||
			vsm->get_map_data_buffer().is_null() || vsm->get_dirty_buffer().is_null() || vsm->get_state_buffer().is_null() || vsm->get_indirect_buffer().is_null()) {
		sync_hiz_without_nanite();
		return;
	}
	if (!nextgeo_cut.is_vsm_available()) {
		sync_hiz_without_nanite();
		return;
	}

	RENDER_TIMESTAMP("VSM Nanite Raster");
	_update_dirty_geometry_instances();

	NextGeoVSMInputs inputs;
	inputs.pool_texture = vsm->get_pool_texture();
	inputs.page_table_buffer = vsm->get_page_table_buffer();
	inputs.map_data_buffer = vsm->get_map_data_buffer();
	inputs.dirty_buffer = vsm->get_dirty_buffer();
	inputs.alloc_state_buffer = vsm->get_state_buffer();
	inputs.indirect_buffer = vsm->get_indirect_buffer();
	inputs.table_maps = table_maps;
	inputs.pool_pages = pool_pages;
	inputs.grid_width = uint32_t(RendererVirtualShadowMapStorage::pool_page_grid(pool_pages).x);
	inputs.pool_reallocations = vsm->get_pool_reallocation_count();
	inputs.camera_position = p_render_data->scene_data->cam_transform.origin;
	inputs.max_texel_error = RendererRD::NEXTGEO_VSM_DEFAULT_MAX_TEXEL_ERROR;
	inputs.lod_bias = 1.0f;
	inputs.sw_max_edge_texels = float(_vsm_setting("nanite_sw_raster_max_edge_texels", double(RendererRD::NEXTGEO_VSM_DEFAULT_SW_MAX_EDGE_TEXELS)));
	inputs.hiz_enabled = bool(_vsm_setting("nanite_hiz", true));
	// GodotNext: vsm_caching (#212). Enable #217's static/dynamic layer routing in production frames.
	inputs.dynamic_layer_hook = true;
	inputs.count_sw_writes = bool(GLOBAL_GET("rendering/nextgeo/debug_stats")) || bool(_vsm_setting("debug_nanite_dynamic_layer", false));
	// GodotNext: nextgeo_tessellation (#163). Per-chain VSM tessellation toggles (gated by experimental/tessellation).
	const bool vsm_tess_master = bool(GLOBAL_GET("rendering/nextgeo/experimental/tessellation"));
	inputs.tessellation_directional = vsm_tess_master && bool(_vsm_setting("tessellation_directional", true));
	inputs.tessellation_local = vsm_tess_master && bool(_vsm_setting("tessellation_local", false));
	inputs.tessellation_dicing_rate = float(GLOBAL_GET("rendering/nextgeo/tessellation/dicing_rate"));
	inputs.tessellation_debug_max_patches = uint32_t(MAX(int(GLOBAL_GET("rendering/nextgeo/tessellation/debug_max_patches")), 0));

	// Build per-map metadata (shadow_caster_mask, reverse_cull, active) so one directional chain and
	// one local chain cull every active VSM light cleanly without per-light CPU loops.
	inputs.map_meta.resize(table_maps);
	uint32_t active_lights = 0;
	if (p_render_data->lights != nullptr) {
		for (uint32_t i = 0; i < p_render_data->lights->size(); i++) {
			const RID light_instance = (*p_render_data->lights)[i];
			if (!light_storage->owns_light_instance(light_instance)) {
				continue;
			}
			const RID base_light = light_storage->light_instance_get_base_light(light_instance);
			if (base_light.is_null() || !light_storage->light_has_shadow(base_light)) {
				continue;
			}
			if (light_storage->light_get_type(base_light) == RSE::LIGHT_DIRECTIONAL && light_storage->light_directional_get_sky_mode(base_light) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
				continue;
			}
			const RendererVirtualShadowMapStorage::MapRange *range = vsm->map_get(light_instance);
			if (range == nullptr || range->first == RendererVirtualShadowMapStorage::MAP_NONE || range->count == 0) {
				continue;
			}
			active_lights++;
			const uint32_t caster_mask = light_storage->light_get_shadow_caster_mask(base_light);
			const uint32_t mflags = RendererRD::NEXTGEO_VSM_MAP_META_ACTIVE | (light_storage->light_get_reverse_cull_face_mode(base_light) ? RendererRD::NEXTGEO_VSM_MAP_META_REVERSE_CULL : 0u);
			const uint32_t end_map = MIN(range->first + range->count, table_maps);
			for (uint32_t m = range->first; m < end_map; m++) {
				inputs.map_meta[m].cull_mask = caster_mask;
				inputs.map_meta[m].flags = mflags;
			}
		}
	}
	if (active_lights == 0u) {
		sync_hiz_without_nanite();
		return;
	}

	// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9). Select mesh vs VS HW path for VSM.
	const RendererRD::NextGeoHWRasterPath vsm_hw_setting = RendererRD::nextgeo_hw_raster_path_from_variant(GLOBAL_GET("rendering/nextgeo/hw_raster_path"));
	const RendererRD::NextGeoHWRasterSelection vsm_hw_sel = nextgeo_cut.select_hw_raster_path(vsm_hw_setting, false);
	inputs.use_mesh_hw_raster = vsm_hw_sel.use_mesh;
	inputs.hw_raster_reason = vsm_hw_sel.reason;

	// Step 7: Share #155's programmable raster bins and material raster targets (group 2 depth-only).
	if (nextgeo_programmable_view_active && p_render_data->render_buffers.is_valid()) {
		Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
		const uint32_t bucket_count = RSG::virtual_mesh_storage->get_bucket_record_count();
		if (nextgeo_raster_bin_table.size() < bucket_count * RendererRD::NEXTGEO_RASTER_BIN_TABLE_WORDS_PER_RECORD) {
			const uint32_t old_words = nextgeo_raster_bin_table.size();
			nextgeo_raster_bin_table.resize(MAX(bucket_count * RendererRD::NEXTGEO_RASTER_BIN_TABLE_WORDS_PER_RECORD, 1u));
			memset(nextgeo_raster_bin_table.ptr() + old_words, 0, (nextgeo_raster_bin_table.size() - old_words) * sizeof(uint32_t));
		}
		if (nextgeo_instance_data.size() < bucket_count) {
			const uint32_t old_size = nextgeo_instance_data.size();
			nextgeo_instance_data.resize(bucket_count);
			memset(nextgeo_instance_data.ptr() + old_size, 0, (nextgeo_instance_data.size() - old_size) * sizeof(SceneState::InstanceData));
		}
		bool extra_instance_uploaded = false;
		const RenderVSMShadowData *vsm_shadows = p_render_data->sdfgi_update_data != nullptr ? p_render_data->sdfgi_update_data->vsm_shadows : nullptr;
		const uint32_t vsm_shadow_count = p_render_data->sdfgi_update_data != nullptr ? p_render_data->sdfgi_update_data->vsm_shadow_count : 0;
		for (uint32_t s = 0; s < vsm_shadow_count && vsm_shadows != nullptr; s++) {
			for (uint32_t i = 0; i < vsm_shadows[s].casters.size(); i++) {
				GeometryInstanceForwardClustered *inst = static_cast<GeometryInstanceForwardClustered *>(vsm_shadows[s].casters[i].geometry_instance);
				if (inst == nullptr || inst->nextgeo_record_index == UINT32_MAX || inst->nextgeo_bucket_slot == UINT32_MAX) {
					continue;
				}
				for (GeometryInstanceSurfaceDataCache *surf = inst->surface_caches; surf != nullptr; surf = surf->next) {
					if ((surf->flags & (GeometryInstanceSurfaceDataCache::FLAG_VIRTUAL | GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW)) != (GeometryInstanceSurfaceDataCache::FLAG_VIRTUAL | GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW)) {
						continue;
					}
					if (surf->nextgeo_raster_bin == 0u || surf->nextgeo_raster_bin >= RendererVirtualMeshStorage::RASTER_BIN_LIMIT || surf->surface_index >= 32u || surf->shader == nullptr || surf->material == nullptr) {
						continue;
					}
					const uint32_t bslot = inst->nextgeo_bucket_slot + surf->nextgeo_bucket_offset;
					if (bslot >= bucket_count) {
						continue;
					}
					const uint32_t existing_bin = RendererRD::nextgeo_raster_bin_table_get(nextgeo_raster_bin_table.ptr(), bslot, surf->surface_index);
					if (existing_bin != surf->nextgeo_raster_bin) {
						RendererRD::nextgeo_raster_bin_table_set(nextgeo_raster_bin_table.ptr(), bslot, surf->surface_index, surf->nextgeo_raster_bin);
						nextgeo_raster_bin_owners[surf->nextgeo_raster_bin].shader = surf->shader;
						nextgeo_raster_bin_owners[surf->nextgeo_raster_bin].material = surf->material;
						nextgeo_raster_bins_used |= uint64_t(1) << (surf->nextgeo_raster_bin - 1u);
						// Off-camera and SHADOWS_ONLY instances were not visited in RENDER_LIST_OPAQUE;
						// populate their SceneState::InstanceData with the exact transposed 3x4 layout
						// expected by raster_material_inc.glsl.
						SceneState::InstanceData &idata = nextgeo_instance_data[bslot];
						if (idata.material_feedback_index == 0u) {
							if (inst->store_transform_cache) {
								RendererRD::MaterialStorage::store_transform_transposed_3x4(inst->transform, idata.transform);
								RendererRD::MaterialStorage::store_transform_transposed_3x4(inst->prev_transform, idata.prev_transform);
#ifdef REAL_T_IS_DOUBLE
								RendererRD::MaterialStorage::split_double(inst->transform.origin.x, &idata.transform[3], &idata.model_precision[0]);
								RendererRD::MaterialStorage::split_double(inst->transform.origin.y, &idata.transform[7], &idata.model_precision[1]);
								RendererRD::MaterialStorage::split_double(inst->transform.origin.z, &idata.transform[11], &idata.model_precision[2]);
								RendererRD::MaterialStorage::split_double(inst->prev_transform.origin.x, &idata.prev_transform[3], &idata.prev_model_precision[0]);
								RendererRD::MaterialStorage::split_double(inst->prev_transform.origin.y, &idata.prev_transform[7], &idata.prev_model_precision[1]);
								RendererRD::MaterialStorage::split_double(inst->prev_transform.origin.z, &idata.prev_transform[11], &idata.prev_model_precision[2]);
#endif
							} else {
								RendererRD::MaterialStorage::store_transform_transposed_3x4(Transform3D(), idata.transform);
								RendererRD::MaterialStorage::store_transform_transposed_3x4(Transform3D(), idata.prev_transform);
							}
							uint32_t flags = inst->base_flags;
							if (inst->non_uniform_scale) {
								flags |= INSTANCE_DATA_FLAGS_NON_UNIFORM_SCALE;
							}
							float fade_alpha = 1.0f;
							if (inst->fade_near || inst->fade_far) {
								float fade_dist = inst->transformed_aabb.get_center().distance_to(p_render_data->scene_data->cam_transform.origin);
								if (inst->fade_far && fade_dist > inst->fade_far_begin) {
									fade_alpha = Math::smoothstep(0.0f, 1.0f, 1.0f - (fade_dist - inst->fade_far_begin) / (inst->fade_far_end - inst->fade_far_begin));
								} else if (inst->fade_near && fade_dist < inst->fade_near_end) {
									fade_alpha = Math::smoothstep(0.0f, 1.0f, (fade_dist - inst->fade_near_begin) / (inst->fade_near_end - inst->fade_near_begin));
								}
							}
							fade_alpha *= inst->force_alpha * inst->parent_fade_alpha;
							flags = (flags & ~INSTANCE_DATA_FLAGS_FADE_MASK) | (uint32_t(CLAMP(int(fade_alpha * 255.0f), 0, 255)) << INSTANCE_DATA_FLAGS_FADE_SHIFT);
							idata.flags = flags;
							idata.gi_offset = 0xFFFFFFFF;
							idata.layer_mask = inst->layer_mask;
							idata.instance_uniforms_ofs = uint32_t(inst->shader_uniforms_offset);
							idata.set_lightmap_uv_scale(inst->lightmap_uv_scale);
							idata.set_compressed_aabb(AABB(Vector3(0.0, 0.0, 0.0), Vector3(1.0, 1.0, 1.0)));
							// GodotNext: megalights_inputs (#136)
							idata.set_megalights_layer_group(megalights_inputs_active ? RendererRD::MegaLightsInputs::assign_layer_group(inst->layer_mask) : 0);
							idata.material_feedback_index = UINT32_MAX;
							idata.set_uv_scale(Vector4(0.0, 0.0, 0.0, 0.0));
							extra_instance_uploaded = true;
						}
					}
				}
			}
		}
		if (extra_instance_uploaded) {
			nextgeo_resolve.upload_tables(
					nextgeo_bucket_table.ptr(),
					nextgeo_bucket_table.size(),
					nextgeo_instance_data.ptr(),
					nextgeo_instance_data.size() * sizeof(SceneState::InstanceData));
		}
		if (nextgeo_raster_bins_used != 0u && !nextgeo_raster_bin_table.is_empty() && nextgeo_resolve.get_instance_data_buffer().is_valid() && p_render_data->scene_data->get_uniform_buffer().is_valid()) {
			for (uint32_t bin = 1; bin < RendererVirtualMeshStorage::RASTER_BIN_LIMIT; bin++) {
				const NextGeoRasterBinOwner &owner = nextgeo_raster_bin_owners[bin];
				if ((nextgeo_raster_bins_used & (uint64_t(1) << (bin - 1u))) == 0u || owner.shader == nullptr || owner.material == nullptr) {
					continue;
				}
				const RID sw_sh = owner.shader->get_raster_target_vsm_shader(false);
				const RID hw_sh = owner.shader->get_raster_target_vsm_shader(true);
				const RID hw_mesh_sh = vsm_hw_sel.use_mesh ? owner.shader->get_raster_target_vsm_mesh_shader() : RID();
				if (sw_sh.is_null() && hw_sh.is_null()) {
					continue;
				}
				NextGeoVSMBinProgram program;
				program.bin = bin;
				// GodotNext: nextgeo_displacement (#150), nextgeo_vertex_offset (#164) x vsm_nanite (#217).
				program.features = owner.material->get_raster_features();
				program.vertex_offset_max = owner.material->get_effective_vertex_offset_max(float(GLOBAL_GET("rendering/nextgeo/vertex_offset_max_default")));
				program.disp_magnitude = owner.material->displacement_magnitude;
				program.disp_center = owner.material->displacement_center;
				program.sw_shader = sw_sh;
				program.hw_shader = hw_sh;
				program.hw_mesh_shader = hw_mesh_sh;
				if ((inputs.tessellation_directional || inputs.tessellation_local) &&
						(program.features & RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) != 0u &&
						owner.shader->has_displacement && owner.shader->ensure_tessellation_target()) {
					program.tess_dice_shader = owner.shader->get_tessellation_vsm_dice_shader();
					program.tess_hw_shader = owner.shader->get_tessellation_vsm_hw_shader();
				}
				Vector<RD::Uniform> material_uniforms;
				// GodotNext: nextgeo_texture_cache_refresh (#305) x vsm_nanite (#217). Guard VSM programmable bins against freed textures mid-frame.
				bool uniforms_valid = owner.material->get_parameter_uniforms(material_uniforms, owner.shader->texture_uniforms, owner.shader->ubo_size, true);
				RenderingDevice *rd = RD::get_singleton();
				for (const RD::Uniform &uniform : material_uniforms) {
					if (uniform.uniform_type == RD::UNIFORM_TYPE_TEXTURE) {
						uniforms_valid = uniforms_valid && uniform.get_id_count() > 0;
						for (uint32_t j = 0; j < uniform.get_id_count(); j++) {
							uniforms_valid = uniforms_valid && rd->texture_is_valid(uniform.get_id(j));
						}
					}
				}
				if (!uniforms_valid) {
					RendererRD::MaterialStorage::get_singleton()->material_queue_texture_update(owner.material->get_self());
					continue;
				}
				for (const RD::Uniform &uniform : material_uniforms) {
					program.material_uniforms.push_back(uniform);
				}
				inputs.raster_bin_programs.push_back(program);
			}
			if (!inputs.raster_bin_programs.is_empty()) {
				inputs.raster_bin_table = nextgeo_raster_bin_table.ptr();
				inputs.raster_bin_table_words = nextgeo_raster_bin_table.size();
				inputs.environment_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_render_data->scene_data->get_uniform_buffer()));
				inputs.environment_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, nextgeo_resolve.get_instance_data_buffer()));
				inputs.environment_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, RendererRD::MaterialStorage::get_singleton()->global_shader_uniforms_get_storage_buffer()));
				rb->get_samplers().append_uniforms(inputs.environment_uniforms, 3);
			}
		}
	}

	nextgeo_cut.prepare_gpu_records(RSG::virtual_mesh_storage);

	Dictionary nanite_stats;
	if (nextgeo_cut.dispatch_vsm(RSG::virtual_mesh_storage, inputs, &nanite_stats)) {
		vsm_debug_view_nanite_pairs = true;
		nanite_stats["light_count"] = int64_t(active_lights);
		vsm->set_last_nanite_stats(nanite_stats);
	} else {
		sync_hiz_without_nanite();
	}
}

Dictionary RenderForwardClustered::get_vsm_raster_check_result() {
	RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
	Dictionary result;
	result["captured"] = vsm_raster_check.captured;
	result["armed"] = vsm != nullptr && vsm->is_raster_check_armed();
	result["active"] = vsm != nullptr && vsm->is_active();
	if (!vsm_raster_check.captured) {
		return result;
	}
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, result);

	PackedStringArray failures;
	const uint32_t table_maps = vsm_raster_check.table_maps;
	const uint32_t pool_pages = vsm_raster_check.pool_pages;
	const uint32_t pair_capacity = vsm_raster_check.pair_capacity;
	const float min_texels = vsm_raster_check.min_caster_texels;
	const uint32_t group_count = vsm_raster_check.group_draw_meta.size() / 2u;

	const LocalVector<uint32_t> dirty_words = _vsm_read_words(vsm_raster_check.dirty_snap);
	const LocalVector<uint32_t> alloc_state = _vsm_read_words(vsm_raster_check.alloc_state_snap);
	const LocalVector<uint32_t> map_state = _vsm_read_words(vsm_raster_check.map_state_snap);
	const LocalVector<uint32_t> group_counts = _vsm_read_words(vsm_raster_check.group_counts_snap);
	const LocalVector<uint32_t> group_offsets = _vsm_read_words(vsm_raster_check.group_offsets_snap);
	const LocalVector<uint32_t> indirect_cmds = _vsm_read_words(vsm_raster_check.indirect_commands_snap);
	const LocalVector<uint32_t> indirect_counts = _vsm_read_words(vsm_raster_check.indirect_counts_snap);
	const LocalVector<uint32_t> cull_state = _vsm_read_words(vsm_raster_check.cull_state_snap);
	const LocalVector<uint32_t> pairs_words = _vsm_read_words(vsm_raster_check.pairs_snap);
	const LocalVector<uint32_t> pair_texels = _vsm_read_words(vsm_raster_check.pair_texels_snap);
	const LocalVector<uint32_t> page_table = _vsm_read_words(vsm_raster_check.page_table_snap);

	LocalVector<LocalVector<float>> sub_transforms;
	sub_transforms.resize(vsm_raster_check.sub_transform_snaps.size());
	for (uint32_t i = 0; i < vsm_raster_check.sub_transform_snaps.size(); i++) {
		const LocalVector<uint32_t> w = _vsm_read_words(vsm_raster_check.sub_transform_snaps[i]);
		sub_transforms[i].resize(w.size());
		if (!w.is_empty()) {
			memcpy(sub_transforms[i].ptr(), w.ptr(), w.size() * sizeof(float));
		}
	}

	Vector<uint8_t> pool_layers[RendererVirtualShadowMapStorage::POOL_LAYERS];
	if (vsm_raster_check.pool_snap.is_valid()) {
		for (uint32_t layer = 0; layer < RendererVirtualShadowMapStorage::POOL_LAYERS; layer++) {
			pool_layers[layer] = rd->texture_get_data(vsm_raster_check.pool_snap, layer);
		}
	}

	const VSMRasterCheckCapture cap = vsm_raster_check;
	_free_vsm_raster_check_buffers();

	if (dirty_words.size() < pool_pages * 4u || alloc_state.size() < RendererVirtualShadowMapStorage::STATE_SIZE || map_state.size() < table_maps * RendererVirtualShadowMapStorage::MAP_STATE_REGIONS || cap.map_data.size() < table_maps) {
		failures.push_back("incomplete marking/allocation snapshot buffers");
		result["failures"] = failures;
		result["pass"] = false;
		return result;
	}
	if (group_count > 0 && (group_counts.size() < group_count || group_offsets.size() < group_count || indirect_cmds.size() < group_count * 5u || indirect_counts.size() < group_count || cull_state.size() < uint32_t(VSM_CULL_STATE_WORDS) || pairs_words.size() < pair_capacity * 4u || pair_texels.size() < 2u * pair_capacity + 1u)) {
		failures.push_back("incomplete non-Nanite cull/raster snapshot buffers");
		result["failures"] = failures;
		result["pass"] = false;
		return result;
	}

	const uint32_t dirty_limit = MIN(alloc_state[RendererVirtualShadowMapStorage::STATE_DIRTY], pool_pages);
	const RendererVirtualShadowMapStorage::DirtyPageRecord *dirty_recs = reinterpret_cast<const RendererVirtualShadowMapStorage::DirtyPageRecord *>(dirty_words.ptr());
	const uint32_t *map_dirty_counts = map_state.ptr() + RendererVirtualShadowMapStorage::MAP_STATE_DIRTY_COUNT * table_maps;
	const uint32_t *map_dirty_offsets = map_state.ptr() + RendererVirtualShadowMapStorage::MAP_STATE_DIRTY_OFFSET * table_maps;
	const RendererVirtualShadowMapStorage::MapData *maps = cap.map_data.ptr();

	// The CPU model of the cull (cull_box_dirty_pages() and sub_instance_box()) over the records the
	// view uploaded. cpu_exact holds its pairs for the exact inputs. A GPU pair outside cpu_union (the
	// boxes grown by 1e-5 of their largest coordinate, the small-caster threshold lowered by 1e-5) or a
	// cpu_robust pair (boxes shrunk, threshold raised) the GPU did not emit is a failure: float rounding
	// cannot explain it. Exact differences are reported (exact_unexplained, exact_missing).
	static constexpr float VSM_CHECK_TOLERANCE = 1e-5f;
	HashSet<VSMPairTuple, VSMPairTupleHasher> cpu_exact;
	HashSet<VSMPairTuple, VSMPairTupleHasher> cpu_union;
	HashSet<VSMPairTuple, VSMPairTupleHasher> cpu_robust;
	uint64_t cpu_total_pairs = 0;
	uint64_t cpu_small_culled = 0;
	uint64_t cpu_df_far_culled = 0;
	LocalVector<uint32_t> tmp_dirty;

	auto add_cpu_box = [&](uint32_t p_elem, uint32_t p_sub, uint32_t p_group, uint32_t p_map_base, uint32_t p_light_flags, bool p_dyn, uint32_t p_caster, const AABB &p_box, float p_df_handoff_start) {
		const uint32_t dyn_bit = p_dyn ? (1u << 31u) : 0u;
		uint32_t small = 0;
		uint32_t df_far = 0;
		RendererVirtualShadowMapStorage::cull_box_dirty_pages(maps, table_maps, map_dirty_counts, map_dirty_offsets, dirty_recs, dirty_limit, p_map_base, p_light_flags, p_box, p_dyn, min_texels, tmp_dirty, small, cap.camera_position, p_df_handoff_start, &df_far);
		cpu_total_pairs += tmp_dirty.size();
		cpu_small_culled += small;
		cpu_df_far_culled += df_far;
		for (uint32_t d : tmp_dirty) {
			cpu_exact.insert({ p_group, p_elem, p_sub, d | dyn_bit, p_caster });
		}

		const Vector3 hi = p_box.position + p_box.size;
		float max_abs = 1.0f;
		max_abs = MAX(max_abs, Math::abs(float(p_box.position.x)));
		max_abs = MAX(max_abs, Math::abs(float(p_box.position.y)));
		max_abs = MAX(max_abs, Math::abs(float(p_box.position.z)));
		max_abs = MAX(max_abs, Math::abs(float(hi.x)));
		max_abs = MAX(max_abs, Math::abs(float(hi.y)));
		max_abs = MAX(max_abs, Math::abs(float(hi.z)));
		const float eps = VSM_CHECK_TOLERANCE * max_abs;

		AABB union_box = p_box;
		union_box.grow_by(eps);
		uint32_t unused = 0;
		RendererVirtualShadowMapStorage::cull_box_dirty_pages(maps, table_maps, map_dirty_counts, map_dirty_offsets, dirty_recs, dirty_limit, p_map_base, p_light_flags, union_box, p_dyn, min_texels * (1.0f - VSM_CHECK_TOLERANCE), tmp_dirty, unused, cap.camera_position, p_df_handoff_start > 0.0f ? p_df_handoff_start * (1.0f + VSM_CHECK_TOLERANCE) : 0.0f);
		for (uint32_t d : tmp_dirty) {
			cpu_union.insert({ p_group, p_elem, p_sub, d | dyn_bit, p_caster });
		}

		const Vector3 shrink(
				MIN(eps, float(p_box.size.x) * 0.25f),
				MIN(eps, float(p_box.size.y) * 0.25f),
				MIN(eps, float(p_box.size.z) * 0.25f));
		const AABB robust_box(p_box.position + shrink, p_box.size - shrink * 2.0f);
		RendererVirtualShadowMapStorage::cull_box_dirty_pages(maps, table_maps, map_dirty_counts, map_dirty_offsets, dirty_recs, dirty_limit, p_map_base, p_light_flags, robust_box, p_dyn, min_texels * (1.0f + VSM_CHECK_TOLERANCE), tmp_dirty, unused, cap.camera_position, p_df_handoff_start > 0.0f ? p_df_handoff_start * (1.0f - VSM_CHECK_TOLERANCE) : 0.0f);
		for (uint32_t d : tmp_dirty) {
			cpu_robust.insert({ p_group, p_elem, p_sub, d | dyn_bit, p_caster });
		}
	};

	for (const VSMBoxCullRecord &rec : cap.box_records) {
		const Vector3 bmin(rec.box_min[0], rec.box_min[1], rec.box_min[2]);
		const Vector3 bmax(rec.box_max[0], rec.box_max[1], rec.box_max[2]);
		add_cpu_box(rec.meta0[0], 0u, rec.meta0[1], rec.meta0[2], rec.meta0[3], rec.meta1[0] != 0u, rec.meta1[1], AABB(bmin, bmax - bmin), rec.box_min[3]);
	}

	uint32_t sub_snapshot_short = 0;
	for (uint32_t sr = 0; sr < cap.sub_records.size(); sr++) {
		const VSMSubCullRecord &rec = cap.sub_records[sr];
		const uint32_t snap_idx = cap.sub_record_snap_index[sr];
		if (snap_idx >= sub_transforms.size() || sub_transforms[snap_idx].is_empty()) {
			sub_snapshot_short++;
			continue;
		}
		float model[12];
		memcpy(&model[0], rec.model_row0, 4 * sizeof(float));
		memcpy(&model[4], rec.model_row1, 4 * sizeof(float));
		memcpy(&model[8], rec.model_row2, 4 * sizeof(float));
		const Vector3 lmin(rec.local_min[0], rec.local_min[1], rec.local_min[2]);
		const Vector3 lmax(rec.local_max[0], rec.local_max[1], rec.local_max[2]);
		const AABB local_aabb(lmin, lmax - lmin);
		const uint32_t sub_count = rec.meta1[2];
		const uint32_t cur_ofs = rec.meta1[3];
		const uint32_t stride_vec4 = rec.meta2[0];
		const bool format_2d = rec.meta2[1] != 0u;
		const uint32_t trail_steps = rec.meta2[2];
		const uint32_t avail_floats = sub_transforms[snap_idx].size();

		for (uint32_t sub = 0; sub < sub_count; sub++) {
			const uint32_t needed_floats = trail_steps > 1u ? ((trail_steps * (sub + 1u)) * 20u) : ((sub + cur_ofs + 1u) * stride_vec4 * 4u);
			if (needed_floats > avail_floats) {
				sub_snapshot_short++;
				break;
			}
			AABB sub_box;
			if (RendererVirtualShadowMapStorage::sub_instance_box(model, sub_transforms[snap_idx].ptr(), sub, cur_ofs, stride_vec4, format_2d, trail_steps, local_aabb, rec.local_min[3], sub_box, (rec.meta2[3] & VSM_SUB_FLAG_VIEW_FACING) != 0u, rec.local_max[3])) {
				add_cpu_box(rec.meta0[0], sub, rec.meta0[1], rec.meta0[2], rec.meta0[3], rec.meta1[0] != 0u, rec.meta1[1], sub_box, rec.df_params[0]);
			}
		}
	}

	uint32_t scan_mismatches = 0;
	uint32_t pair_unexpected = 0;
	uint32_t pair_missing = 0;
	uint32_t pair_duplicates = 0;
	uint32_t pair_bad_target = 0; // Pairs whose dirty record is outside the list or whose layer is not dirty.
	uint32_t exact_unexplained = 0;
	uint32_t exact_missing = 0;
	LocalVector<VSMPairTuple> exact_differences; // Pairs in only one of the GPU and the exact CPU model.
	const uint32_t gpu_total_pairs = group_count > 0 ? cull_state[VSM_CULL_STATE_TOTAL_PAIRS] : 0u;
	const uint32_t gpu_dropped_pairs = group_count > 0 ? cull_state[VSM_CULL_STATE_DROPPED_PAIRS] : 0u;
	const uint32_t gpu_small_culled = group_count > 0 ? cull_state[VSM_CULL_STATE_SMALL_CULLED] : 0u;
	const uint32_t cull_sink = group_count > 0 ? cull_state[VSM_CULL_STATE_SINK] : 0u;
	const uint32_t redraw_marks = group_count > 0 ? cull_state[VSM_CULL_STATE_REDRAW_MARKS] : 0u;
	const uint32_t write_mismatch = group_count > 0 ? cull_state[VSM_CULL_STATE_WRITE_MISMATCH] : 0u;
	const uint32_t gpu_df_far_culled = group_count > 0 ? cull_state[VSM_CULL_STATE_DF_FAR_CULLED] : 0u;
	const uint32_t raster_sink = !pair_texels.is_empty() ? pair_texels[0] : 0u;

	// The page-table entry of a dirty record, and whether the cull marked it to be drawn again (a pair the
	// full list could not hold): after the emit pass no requested entry keeps ENTRY_SCROLLED otherwise.
	auto dirty_entry_index = [&](const RendererVirtualShadowMapStorage::DirtyPageRecord &p_rec) -> uint32_t {
		return RendererVirtualShadowMapStorage::entry_index(p_rec.map, p_rec.packed_page & 0xFFu, (p_rec.packed_page >> 8u) & 0xFFu, (p_rec.packed_page >> 16u) & 0xFFu);
	};
	auto dirty_marked = [&](uint32_t p_dirty) -> bool {
		if (p_dirty >= dirty_limit || dirty_recs[p_dirty].map >= cap.page_table_maps) {
			return false;
		}
		const uint32_t index = dirty_entry_index(dirty_recs[p_dirty]);
		return index < page_table.size() && (page_table[index] & RendererVirtualShadowMapStorage::ENTRY_SCROLLED) != 0u;
	};
	// The base light of each map, for each caster's texels per light.
	HashMap<uint32_t, uint32_t> map_light; // map -> index into cap.lights.
	for (uint32_t l = 0; l < cap.lights.size(); l++) {
		for (uint32_t m = 0; m < cap.lights[l].map_count; m++) {
			map_light[cap.lights[l].map_first + m] = l;
		}
	}

	// Per pair: the fragments its draw wrote (pair_texels[1 + pair]), summed per caster and per dirty
	// page and layer.
	HashSet<VSMPairTuple, VSMPairTupleHasher> gpu_emitted;
	LocalVector<uint32_t> caster_pairs;
	// GodotNext: vsm_caching (#212). Per-caster layer-0/layer-1 pair counts + ALWAYS layer-0 invariant.
	LocalVector<uint32_t> caster_layer0_pairs;
	LocalVector<uint32_t> caster_layer1_pairs;
	uint64_t always_layer0_pairs = 0;
	LocalVector<uint64_t> caster_fragments;
	LocalVector<uint64_t> caster_outside; // Vertices outside the cull box of their pair.
	LocalVector<HashSet<uint32_t>> caster_groups; // The draw groups each caster's pairs went to.
	LocalVector<HashMap<uint32_t, uint64_t>> caster_light_fragments; // Per caster: light index -> fragments.
	caster_pairs.resize(cap.casters.size());
	caster_layer0_pairs.resize(cap.casters.size());
	caster_layer1_pairs.resize(cap.casters.size());
	caster_fragments.resize(cap.casters.size());
	caster_outside.resize(cap.casters.size());
	caster_groups.resize(cap.casters.size());
	caster_light_fragments.resize(cap.casters.size());
	for (uint32_t c = 0; c < caster_pairs.size(); c++) {
		caster_pairs[c] = 0;
		caster_layer0_pairs[c] = 0;
		caster_layer1_pairs[c] = 0;
		caster_fragments[c] = 0;
		caster_outside[c] = 0;
	}
	uint64_t total_outside_vertices = 0;
	LocalVector<uint32_t> page_layer_pairs;
	LocalVector<uint64_t> page_layer_fragments;
	page_layer_pairs.resize(dirty_limit * 2u);
	page_layer_fragments.resize(dirty_limit * 2u);
	for (uint32_t i = 0; i < dirty_limit * 2u; i++) {
		page_layer_pairs[i] = 0;
		page_layer_fragments[i] = 0;
	}
	uint64_t total_fragments = 0;

	uint32_t running_excl = 0;
	for (uint32_t g = 0; g < group_count; g++) {
		const uint32_t raw = group_counts[g];
		const uint32_t expected_ofs = MIN(running_excl, pair_capacity);
		const uint32_t avail = running_excl < pair_capacity ? (pair_capacity - running_excl) : 0u;
		const uint32_t expected_cnt = MIN(raw, avail);
		const uint32_t expected_drawn = cap.group_draw_meta[g * 2u + 0u];
		const uint32_t expected_icnt = expected_cnt > 0u ? 1u : 0u;

		if (group_offsets[g] != expected_ofs ||
				indirect_cmds[g * 5u + 0u] != expected_drawn ||
				indirect_cmds[g * 5u + 1u] != expected_cnt ||
				indirect_cmds[g * 5u + 2u] != 0u ||
				indirect_cmds[g * 5u + 3u] != 0u ||
				indirect_cmds[g * 5u + 4u] != 0u ||
				indirect_counts[g] != expected_icnt) {
			scan_mismatches++;
		}
		running_excl += raw;

		const uint32_t gpu_ofs = group_offsets[g];
		const uint32_t gpu_cnt = MIN(indirect_cmds[g * 5u + 1u], gpu_ofs < pair_capacity ? (pair_capacity - gpu_ofs) : 0u);
		for (uint32_t k = 0; k < gpu_cnt; k++) {
			const uint32_t pair_index = gpu_ofs + k;
			const uint32_t *pw = pairs_words.ptr() + pair_index * 4u;
			const VSMPairTuple tup{ g, pw[0], pw[1], pw[2], pw[3] };
			if (gpu_emitted.has(tup)) {
				pair_duplicates++;
			} else {
				gpu_emitted.insert(tup);
			}
			if (!cpu_union.has(tup)) {
				pair_unexpected++;
			}
			if (!cpu_exact.has(tup)) {
				exact_unexplained++;
				exact_differences.push_back(tup);
			}
			const uint32_t fragments = (2u * pair_index + 1u) < pair_texels.size() ? pair_texels[2u * pair_index + 1u] : 0u;
			const uint32_t outside = (2u * pair_index + 2u) < pair_texels.size() ? pair_texels[2u * pair_index + 2u] : 0u;
			total_fragments += fragments;
			total_outside_vertices += outside;
			const uint32_t d = pw[2] & 0x7FFFFFFFu;
			const uint32_t layer = pw[2] >> 31u;
			if (pw[3] < caster_pairs.size()) {
				caster_pairs[pw[3]]++;
				if (layer != 0u) {
					caster_layer1_pairs[pw[3]]++;
				} else {
					caster_layer0_pairs[pw[3]]++;
					if (cap.casters[pw[3]].shadow_cache_invalidation == uint32_t(RSE::SHADOW_CACHE_INVALIDATION_ALWAYS)) {
						always_layer0_pairs++;
					}
				}
				caster_fragments[pw[3]] += fragments;
				caster_outside[pw[3]] += outside;
				caster_groups[pw[3]].insert(g);
				if (d < dirty_limit) {
					const uint32_t *light_index = map_light.getptr(dirty_recs[d].map);
					if (light_index != nullptr) {
						caster_light_fragments[pw[3]][*light_index] += fragments;
					}
				}
			}
			const uint32_t layer_flag = layer != 0u ? RendererVirtualShadowMapStorage::ENTRY_DIRTY_DYNAMIC : RendererVirtualShadowMapStorage::ENTRY_DIRTY_STATIC;
			if (d >= dirty_limit || (dirty_recs[d].flags & layer_flag) == 0u) {
				pair_bad_target++;
				continue;
			}
			page_layer_pairs[d * 2u + layer]++;
			page_layer_fragments[d * 2u + layer] += fragments;
		}
	}
	if (group_count > 0) {
		const uint32_t expected_dropped = running_excl > pair_capacity ? (running_excl - pair_capacity) : 0u;
		if (gpu_total_pairs != running_excl || gpu_dropped_pairs != expected_dropped) {
			scan_mismatches++;
		}
	}
	// A pair the GPU did not draw is a failure, unless the full pair list dropped it and marked its page
	// to be drawn again in full.
	uint32_t pair_missing_marked = 0;
	for (const VSMPairTuple &tup : cpu_robust) {
		if (!gpu_emitted.has(tup)) {
			if (gpu_dropped_pairs > 0 && dirty_marked(tup.dirty & 0x7FFFFFFFu)) {
				pair_missing_marked++;
			} else {
				pair_missing++;
			}
		}
	}
	// The pages this capture marked; the next frame's capture checks that they were drawn again.
	HashSet<uint32_t> marked_entries;
	HashSet<uint32_t> dirty_entries;
	for (uint32_t d = 0; d < dirty_limit; d++) {
		if (dirty_recs[d].map >= cap.page_table_maps) {
			continue;
		}
		const uint32_t index = dirty_entry_index(dirty_recs[d]);
		dirty_entries.insert(index);
		if (dirty_marked(d)) {
			marked_entries.insert(index);
		}
	}
	const bool consecutive = vsm_check_marked_frame != 0 && cap.frame == vsm_check_marked_frame + 1;
	uint32_t marked_not_redrawn = 0;
	if (consecutive) {
		for (uint32_t index : vsm_check_marked_entries) {
			// A marked page the view still requests is dirty now; one it does not request was evicted.
			const bool allocated = index < page_table.size() && (page_table[index] & RendererVirtualShadowMapStorage::ENTRY_ALLOCATED) != 0u;
			if (allocated && !dirty_entries.has(index)) {
				marked_not_redrawn++;
			}
		}
	}
	const uint32_t previous_marked = vsm_check_marked_entries.size();
	vsm_check_marked_entries = marked_entries;
	vsm_check_marked_frame = cap.frame;
	if (gpu_dropped_pairs == 0) {
		for (const VSMPairTuple &tup : cpu_exact) {
			if (!gpu_emitted.has(tup)) {
				exact_missing++;
				exact_differences.push_back(tup);
			}
		}
	}

	const Vector2i pool_grid = RendererVirtualShadowMapStorage::pool_page_grid(pool_pages);
	const uint32_t pool_width = uint32_t(pool_grid.x) * RendererVirtualShadowMapStorage::PAGE_SIZE;
	const uint32_t pool_height = uint32_t(pool_grid.y) * RendererVirtualShadowMapStorage::PAGE_SIZE;
	const uint64_t expected_layer_bytes = uint64_t(pool_width) * pool_height * sizeof(uint32_t);
	const bool have_pool = uint64_t(pool_layers[0].size()) == expected_layer_bytes && uint64_t(pool_layers[1].size()) == expected_layer_bytes;

	uint64_t nonzero_static_texels = 0;
	uint64_t nonzero_dynamic_texels = 0;
	uint32_t invalid_max_uint_texels = 0;
	uint32_t non_finite_unpacked_texels = 0;
	// Page checks, per dirty page and dirty layer: texels only where a pair drew, and every pair's
	// fragments land in its own page.
	uint32_t texels_without_pair = 0; // Written texels in a page and layer no pair drew into.
	uint32_t fragments_without_texels = 0; // Fragments counted for a page and layer that holds no written texel.
	uint32_t texels_exceed_fragments = 0; // More written texels than fragments drawn into the page and layer.
	uint32_t pages_with_pairs = 0;
	uint32_t pages_with_texels = 0;
	Array dirty_pages_arr;

	if (have_pool) {
		const uint32_t *layer0 = reinterpret_cast<const uint32_t *>(pool_layers[0].ptr());
		const uint32_t *layer1 = reinterpret_cast<const uint32_t *>(pool_layers[1].ptr());
		const uint32_t psize = RendererVirtualShadowMapStorage::PAGE_SIZE;

		for (uint32_t d = 0; d < dirty_limit; d++) {
			const RendererVirtualShadowMapStorage::DirtyPageRecord &dr = dirty_recs[d];
			const uint32_t phys_x = RendererVirtualShadowMapStorage::entry_page_x(dr.phys_entry);
			const uint32_t phys_y = RendererVirtualShadowMapStorage::entry_page_y(dr.phys_entry);
			if ((phys_x + 1u) * psize > pool_width || (phys_y + 1u) * psize > pool_height) {
				continue;
			}
			uint32_t s_nz = 0;
			uint32_t d_nz = 0;
			float s_min = float(Math::INF);
			float s_max = -float(Math::INF);
			float d_min = float(Math::INF);
			float d_max = -float(Math::INF);

			for (uint32_t ly = 0; ly < psize; ly++) {
				const uint32_t row_base = (phys_y * psize + ly) * pool_width + phys_x * psize;
				for (uint32_t lx = 0; lx < psize; lx++) {
					const uint32_t p0 = layer0[row_base + lx];
					const uint32_t p1 = layer1[row_base + lx];
					if (p0 == 0xFFFFFFFFu) {
						invalid_max_uint_texels++;
					} else if (p0 != 0u) {
						s_nz++;
						const float dep = RendererVirtualShadowMapStorage::depth_unpack(p0);
						if (!Math::is_finite(dep)) {
							non_finite_unpacked_texels++;
						} else {
							s_min = MIN(s_min, dep);
							s_max = MAX(s_max, dep);
						}
					}
					if (p1 == 0xFFFFFFFFu) {
						invalid_max_uint_texels++;
					} else if (p1 != 0u) {
						d_nz++;
						const float dep = RendererVirtualShadowMapStorage::depth_unpack(p1);
						if (!Math::is_finite(dep)) {
							non_finite_unpacked_texels++;
						} else {
							d_min = MIN(d_min, dep);
							d_max = MAX(d_max, dep);
						}
					}
				}
			}
			nonzero_static_texels += s_nz;
			nonzero_dynamic_texels += d_nz;

			const uint32_t layer_nz[2] = { s_nz, d_nz };
			const uint32_t layer_flags[2] = { RendererVirtualShadowMapStorage::ENTRY_DIRTY_STATIC, RendererVirtualShadowMapStorage::ENTRY_DIRTY_DYNAMIC };
			bool page_has_pairs = false;
			bool page_has_texels = false;
			for (uint32_t layer = 0; layer < 2u; layer++) {
				if ((dr.flags & layer_flags[layer]) == 0u) {
					continue; // A clean layer keeps its cached texels.
				}
				const uint64_t fragments = page_layer_fragments[d * 2u + layer];
				page_has_pairs = page_has_pairs || page_layer_pairs[d * 2u + layer] > 0u;
				page_has_texels = page_has_texels || layer_nz[layer] > 0u;
				if (layer_nz[layer] > 0u && fragments == 0u) {
					texels_without_pair++;
				}
				if (fragments > 0u && layer_nz[layer] == 0u) {
					fragments_without_texels++;
				}
				if (uint64_t(layer_nz[layer]) > fragments) {
					texels_exceed_fragments++;
				}
			}
			pages_with_pairs += page_has_pairs ? 1u : 0u;
			pages_with_texels += page_has_texels ? 1u : 0u;

			if (dirty_pages_arr.size() < 4096) {
				auto sample_texel = [&](const uint32_t *p_layer, uint32_t p_lx, uint32_t p_ly) -> float {
					const uint32_t pv = p_layer[(phys_y * psize + p_ly) * pool_width + phys_x * psize + p_lx];
					return pv == 0u ? float(Math::INF) : RendererVirtualShadowMapStorage::depth_unpack(pv);
				};
				Dictionary pd;
				pd["dirty_index"] = int64_t(d);
				pd["map"] = int64_t(dr.map);
				pd["kind"] = dr.map < table_maps ? int64_t(maps[dr.map].kind) : int64_t(RendererVirtualShadowMapStorage::MAP_KIND_MAX);
				pd["map_index"] = dr.map < table_maps ? int64_t(maps[dr.map].index) : int64_t(0);
				pd["mip"] = int64_t(dr.packed_page & 0xFFu);
				pd["page_x"] = int64_t((dr.packed_page >> 8u) & 0xFFu);
				pd["page_y"] = int64_t((dr.packed_page >> 16u) & 0xFFu);
				pd["phys_x"] = int64_t(phys_x);
				pd["phys_y"] = int64_t(phys_y);
				pd["flags"] = int64_t(dr.flags);
				pd["static_pairs"] = int64_t(page_layer_pairs[d * 2u + 0u]);
				pd["dynamic_pairs"] = int64_t(page_layer_pairs[d * 2u + 1u]);
				pd["static_fragments"] = int64_t(page_layer_fragments[d * 2u + 0u]);
				pd["dynamic_fragments"] = int64_t(page_layer_fragments[d * 2u + 1u]);
				pd["static_nonzero"] = int64_t(s_nz);
				pd["dynamic_nonzero"] = int64_t(d_nz);
				pd["static_min_depth"] = double(s_min);
				pd["static_max_depth"] = double(s_max);
				pd["dynamic_min_depth"] = double(d_min);
				pd["dynamic_max_depth"] = double(d_max);
				pd["center_static_depth"] = double(sample_texel(layer0, psize / 2u, psize / 2u));
				pd["center_dynamic_depth"] = double(sample_texel(layer1, psize / 2u, psize / 2u));
				if (s_nz > 0 || d_nz > 0) {
					PackedFloat32Array s_row;
					PackedFloat32Array d_row;
					PackedFloat32Array s_col;
					PackedFloat32Array d_col;
					s_row.resize(psize);
					d_row.resize(psize);
					s_col.resize(psize);
					d_col.resize(psize);
					float *sr_w = s_row.ptrw();
					float *dr_w = d_row.ptrw();
					float *sc_w = s_col.ptrw();
					float *dc_w = d_col.ptrw();
					for (uint32_t i = 0; i < psize; i++) {
						sr_w[i] = sample_texel(layer0, i, psize / 2u);
						dr_w[i] = sample_texel(layer1, i, psize / 2u);
						sc_w[i] = sample_texel(layer0, psize / 2u, i);
						dc_w[i] = sample_texel(layer1, psize / 2u, i);
					}
					pd["static_row32"] = s_row;
					pd["dynamic_row32"] = d_row;
					pd["static_col32"] = s_col;
					pd["dynamic_col32"] = d_col;
				}
				dirty_pages_arr.push_back(pd);
			}
		}
	} else {
		failures.push_back("pool snapshot size did not match expected dimensions");
	}

	Array casters_arr;
	PackedInt64Array casters_without_texels;
	PackedInt64Array casters_outside_box;
	for (uint32_t c = 0; c < cap.casters.size(); c++) {
		const VSMCasterTelemetry &tel = cap.casters[c];
		Dictionary cd;
		cd["caster_id"] = int64_t(c);
		cd["object_id"] = int64_t(tel.object_id);
		cd["base_type"] = int64_t(tel.base_type);
		cd["instance_count"] = int64_t(tel.instance_count);
		cd["is_skinned"] = tel.is_skinned;
		cd["has_mesh_instance"] = tel.has_mesh_instance;
		cd["is_multimesh"] = tel.is_multimesh;
		cd["is_particles"] = tel.is_particles;
		cd["is_dynamic"] = tel.is_dynamic;
		cd["uses_alpha_scissor"] = tel.uses_alpha_scissor;
		cd["uses_vertex_displacement"] = tel.uses_vertex_displacement;
		cd["dynamic_reasons"] = int64_t(tel.dynamic_reasons);
		// GodotNext: vsm_caching (#212).
		cd["shadow_cache_invalidation"] = int64_t(tel.shadow_cache_invalidation);
		cd["layer0_pairs"] = int64_t(caster_layer0_pairs[c]);
		cd["layer1_pairs"] = int64_t(caster_layer1_pairs[c]);
		PackedInt32Array groups_used;
		for (uint32_t g : caster_groups[c]) {
			groups_used.push_back(int32_t(g));
		}
		groups_used.sort();
		cd["groups"] = groups_used;
		cd["pairs_emitted"] = int64_t(caster_pairs[c]);
		cd["texels_written"] = int64_t(caster_fragments[c]);
		cd["vertices_outside_box"] = int64_t(caster_outside[c]);
		// Fragments per light: the light's RID id (the id in the result's lights) -> fragments.
		Dictionary light_texels;
		for (const KeyValue<uint32_t, uint64_t> &kv : caster_light_fragments[c]) {
			light_texels[int64_t(cap.lights[kv.key].base_light_id)] = int64_t(kv.value);
		}
		cd["light_texels"] = light_texels;
		casters_arr.push_back(cd);
		if (caster_pairs[c] > 0u && caster_fragments[c] == 0u) {
			casters_without_texels.push_back(int64_t(tel.object_id));
		}
		if (caster_outside[c] > 0u) {
			casters_outside_box.push_back(int64_t(tel.object_id));
		}
	}

	if (scan_mismatches > 0) {
		failures.push_back(vformat("scan/indirect command mismatches: %d", scan_mismatches));
	}
	if (pair_unexpected > 0) {
		failures.push_back(vformat("GPU emitted %d pairs outside the CPU model (1e-5 tolerance)", pair_unexpected));
	}
	if (pair_missing > 0) {
		failures.push_back(vformat("GPU missed %d pairs of the CPU model (1e-5 tolerance)", pair_missing));
	}
	if (pair_duplicates > 0) {
		failures.push_back(vformat("GPU emitted %d duplicate pairs", pair_duplicates));
	}
	if (pair_bad_target > 0) {
		failures.push_back(vformat("%d pairs target a dirty record outside the list or a layer that is not dirty", pair_bad_target));
	}
	if (sub_snapshot_short > 0) {
		failures.push_back(vformat("%d MultiMesh or particle records had a short transform snapshot", sub_snapshot_short));
	}
	if (gpu_dropped_pairs > 0 && write_mismatch == 0 && redraw_marks != gpu_dropped_pairs) {
		// Every pair past the list's capacity marks its page to be drawn again.
		failures.push_back(vformat("the pair list dropped %d pairs but marked %d pages to be drawn again", gpu_dropped_pairs, redraw_marks));
	}
	if (write_mismatch > 0) {
		failures.push_back(vformat("the cull's write pass and count pass disagreed by %d pairs", write_mismatch));
	}
	if (marked_not_redrawn > 0) {
		failures.push_back(vformat("%d pages that the previous frame's full pair list marked were not drawn again", marked_not_redrawn));
	}
	if (total_outside_vertices > 0) {
		failures.push_back(vformat("%d vertices of %d casters lie outside the box that culled their pair: %s", total_outside_vertices, casters_outside_box.size(), String(Variant(casters_outside_box))));
	}
	if (cap.missing_from_vsm > 0) {
		failures.push_back(vformat("%d atlas shadow casters were missing from their light's VSM casters", cap.missing_from_vsm));
	}
	if (cap.point_size_skipped > 0) {
		// They cast in the shadow atlas (which emulates point sizes) but are not drawn into VSM pages.
		failures.push_back(vformat("%d surfaces set POINT_SIZE on a device without point sizes and were not drawn into VSM pages", cap.point_size_skipped));
	}
	if (!cap.lights_without_casters.is_empty()) {
		failures.push_back(vformat("%d shadowed lights with virtual maps had no caster list", cap.lights_without_casters.size()));
	}
	if (texels_without_pair > 0) {
		failures.push_back(vformat("%d dirty page layers hold texels that no pair drew", texels_without_pair));
	}
	if (fragments_without_texels > 0) {
		failures.push_back(vformat("%d dirty page layers received fragments but hold no texel", fragments_without_texels));
	}
	if (texels_exceed_fragments > 0) {
		failures.push_back(vformat("%d dirty page layers hold more texels than fragments drawn into them", texels_exceed_fragments));
	}
	if (invalid_max_uint_texels > 0) {
		failures.push_back(vformat("%d pool texels held reserved 0xFFFFFFFF", invalid_max_uint_texels));
	}
	if (non_finite_unpacked_texels > 0) {
		failures.push_back(vformat("%d written pool texels unpacked to non-finite depth", non_finite_unpacked_texels));
	}
	if (cull_sink != 0u || raster_sink != 0u) {
		failures.push_back("an atomic returned the reserved 0xFFFFFFFF (#277 sink)");
	}
	// GodotNext: vsm_caching (#212).
	if (always_layer0_pairs > 0) {
		failures.push_back(vformat("%d ALWAYS-mode caster pairs targeted static layer 0", always_layer0_pairs));
	}

	result["used_indirect_count"] = cap.used_indirect_count;
	result["light_count"] = int64_t(cap.light_count);
	result["view_vsm_lights"] = int64_t(cap.view_vsm_lights);
	result["lights_without_casters"] = int64_t(cap.lights_without_casters.size());
	result["caster_count"] = int64_t(cap.casters.size());
	result["surface_element_count"] = int64_t(cap.surface_element_count);
	result["group_count"] = int64_t(group_count);
	result["box_record_count"] = int64_t(cap.box_records.size());
	result["sub_record_count"] = int64_t(cap.sub_records.size());
	result["dirty_page_count"] = int64_t(dirty_limit);
	result["atlas_non_virtual_casters"] = int64_t(cap.atlas_non_virtual_casters);
	result["virtual_skipped"] = int64_t(cap.virtual_skipped);
	result["point_size_skipped"] = int64_t(cap.point_size_skipped);
	result["missing_from_vsm"] = int64_t(cap.missing_from_vsm);
	result["vsm_extra_casters"] = int64_t(cap.vsm_extra_casters);
	result["gpu_total_pairs"] = int64_t(gpu_total_pairs);
	result["gpu_dropped_pairs"] = int64_t(gpu_dropped_pairs);
	result["pair_capacity"] = int64_t(pair_capacity);
	result["redraw_marks"] = int64_t(redraw_marks);
	result["pair_missing_marked"] = int64_t(pair_missing_marked);
	result["marked_pages"] = int64_t(marked_entries.size());
	result["previous_marked_pages"] = int64_t(consecutive ? previous_marked : 0u);
	result["marked_not_redrawn"] = int64_t(marked_not_redrawn);
	result["consecutive"] = consecutive;
	result["write_mismatch"] = int64_t(write_mismatch);
	result["vertices_outside_box"] = int64_t(total_outside_vertices);
	result["casters_outside_box"] = casters_outside_box;
	// The VSM lights, so a test can find a light's entry in each caster's light_texels.
	Array lights_arr;
	for (const VSMRasterCheckCapture::Light &light : cap.lights) {
		Dictionary ld;
		ld["id"] = int64_t(light.base_light_id);
		ld["kind"] = int64_t(light.kind);
		ld["position"] = light.transform.origin;
		ld["direction"] = -light.transform.basis.get_column(2).normalized();
		lights_arr.push_back(ld);
	}
	result["lights"] = lights_arr;
	result["gpu_small_culled"] = int64_t(gpu_small_culled);
	result["cpu_total_pairs"] = int64_t(cpu_total_pairs);
	result["cpu_small_culled"] = int64_t(cpu_small_culled);
	result["gpu_df_far_culled"] = int64_t(gpu_df_far_culled);
	result["cpu_df_far_culled"] = int64_t(cpu_df_far_culled);
	result["scan_mismatches"] = int64_t(scan_mismatches);
	result["pair_unexpected"] = int64_t(pair_unexpected);
	result["pair_missing"] = int64_t(pair_missing);
	result["pair_duplicates"] = int64_t(pair_duplicates);
	result["pair_bad_target"] = int64_t(pair_bad_target);
	result["exact_unexplained"] = int64_t(exact_unexplained);
	result["exact_missing"] = int64_t(exact_missing);
	result["exact_match"] = exact_unexplained == 0u && exact_missing == 0u && pair_duplicates == 0u && gpu_dropped_pairs == 0u;
	// The first differences, to tell float rounding at a page or threshold edge from a model error.
	Array exact_samples;
	for (uint32_t i = 0; i < exact_differences.size() && i < 16u; i++) {
		const VSMPairTuple &tup = exact_differences[i];
		const uint32_t d = tup.dirty & 0x7FFFFFFFu;
		Dictionary sample;
		sample["gpu"] = gpu_emitted.has(tup);
		sample["caster_object_id"] = tup.caster < cap.casters.size() ? int64_t(cap.casters[tup.caster].object_id) : int64_t(-1);
		sample["element"] = int64_t(tup.elem);
		sample["sub_instance"] = int64_t(tup.sub);
		sample["layer"] = int64_t(tup.dirty >> 31u);
		if (d < dirty_limit) {
			const RendererVirtualShadowMapStorage::DirtyPageRecord &dr = dirty_recs[d];
			sample["map"] = int64_t(dr.map);
			sample["kind"] = dr.map < table_maps ? int64_t(maps[dr.map].kind) : int64_t(RendererVirtualShadowMapStorage::MAP_KIND_MAX);
			sample["mip"] = int64_t(dr.packed_page & 0xFFu);
			sample["page"] = Vector2i(int32_t((dr.packed_page >> 8u) & 0xFFu), int32_t((dr.packed_page >> 16u) & 0xFFu));
		}
		exact_samples.push_back(sample);
	}
	result["exact_difference_samples"] = exact_samples;
	result["total_fragments"] = int64_t(total_fragments);
	result["pages_with_pairs"] = int64_t(pages_with_pairs);
	result["pages_with_texels"] = int64_t(pages_with_texels);
	result["texels_without_pair"] = int64_t(texels_without_pair);
	result["fragments_without_texels"] = int64_t(fragments_without_texels);
	result["texels_exceed_fragments"] = int64_t(texels_exceed_fragments);
	result["casters_without_texels"] = casters_without_texels;
	result["nonzero_static_texels"] = int64_t(nonzero_static_texels);
	result["nonzero_dynamic_texels"] = int64_t(nonzero_dynamic_texels);
	result["invalid_max_uint_texels"] = int64_t(invalid_max_uint_texels);
	result["non_finite_unpacked_texels"] = int64_t(non_finite_unpacked_texels);
	// GodotNext: vsm_caching (#212).
	result["always_layer0_pairs"] = int64_t(always_layer0_pairs);
	result["casters"] = casters_arr;
	result["dirty_pages"] = dirty_pages_arr;
	result["failures"] = failures;
	result["pass"] = failures.is_empty();
	return result;
}

void RenderForwardClustered::_process_sss(Ref<RenderSceneBuffersRD> p_render_buffers, const Projection &p_camera) {
	ERR_FAIL_COND(p_render_buffers.is_null());

	Size2i internal_size = p_render_buffers->get_internal_size();
	bool can_use_effects = internal_size.x >= 8 && internal_size.y >= 8;

	if (!can_use_effects) {
		//just copy
		return;
	}

	p_render_buffers->allocate_blur_textures();

	for (uint32_t v = 0; v < p_render_buffers->get_view_count(); v++) {
		RID internal_texture = p_render_buffers->get_internal_texture(v);
		RID depth_texture = p_render_buffers->get_depth_texture(v);
		ss_effects->sub_surface_scattering(p_render_buffers, internal_texture, depth_texture, p_camera, internal_size);
	}
}

void RenderForwardClustered::_render_nextgi_translucency_mark(RenderDataRD *p_render_data) {
	if (p_render_data->scene_data->view_count != 1 || render_list[RENDER_LIST_ALPHA].elements.is_empty()) {
		return;
	}
	Ref<RenderSceneBuffersRD> wrc_buffers = p_render_data->render_buffers;
	ERR_FAIL_COND(wrc_buffers.is_null());
	RendererRD::NextGIWRC *wrc = next_gi.ensure_wrc(wrc_buffers.ptr());
	if (!wrc->uses_translucency_probes()) {
		return;
	}
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());
	const Size2i size = rb->get_internal_size();
	const Size2i mark_size(MAX((size.x + 15) / 16, 1), MAX((size.y + 15) / 16, 1));
	const RID target = wrc->get_translucency_mark_target(mark_size);
	if (target.is_null()) {
		return;
	}
	next_gi.ensure_gi_buffers(rb.ptr(), gi.is_half_resolution());
	RD::get_singleton()->draw_command_begin_label("NextGI Translucency Marks");
	const uint32_t uniform_buffer_index = _setup_environment(p_render_data, false, mark_size, mark_size, Color(), false);
	const RID uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_ALPHA, p_render_data, false, RID(), rb->get_samplers(), uniform_buffer_index, true);
	RenderListParameters params(render_list[RENDER_LIST_ALPHA].elements.ptr(), render_list[RENDER_LIST_ALPHA].element_info.ptr(), render_list[RENDER_LIST_ALPHA].elements.size(), p_render_data->scene_data->cam_transform.basis.determinant() < 0, PASS_MODE_DEPTH_MATERIAL, 0, false, p_render_data->directional_light_soft_shadows, uniform_set, false, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, 1, 0, scene_shader.default_specialization, false);
	params.nextgi_translucency_mark = true;
	params.framebuffer_format = RD::get_singleton()->framebuffer_get_format(target);
	Vector<Color> clear;
	clear.push_back(Color(0, 0, 0, 0));
	_render_list_with_draw_list(&params, target, RD::DRAW_CLEAR_ALL, clear);
	_setup_environment(p_render_data, false, size, size, Color(), false);
	RD::get_singleton()->draw_command_end_label();
}

// GodotNext: avboit (#295, plan §21.2.1)
bool RenderForwardClustered::_avboit_view_active(const RenderDataRD *p_render_data, bool p_use_msaa, bool p_reflection_probe) {
	const AVBOITPolicy::Method method = AVBOITPolicy::method_from_setting(GLOBAL_GET_CACHED(int, AVBOITPolicy::SETTING_METHOD));
	if (method == AVBOITPolicy::METHOD_SORTED) {
		const String line = avboit_log.note(method, AVBOITPolicy::REASON_SETTING);
		if (!line.is_empty()) {
			print_line(line);
		}
		return false;
	}
	// Reflection probes, the debug draw modes and views without translucency keep the sorted pass
	// without a word: they are not a view the setting is for, or there is nothing to blend.
	if (p_reflection_probe || p_render_data->render_buffers.is_null() || render_list[RENDER_LIST_ALPHA].elements.is_empty() || get_debug_draw_mode() != RSE::VIEWPORT_DEBUG_DRAW_DISABLED) {
		return false;
	}
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	if (!rb->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		return false;
	}
	if (avboit_device_features < 0) {
		avboit_device_features = AVBOITPolicy::device_features_met(RendererRD::AVBOIT::query_device_features(RD::get_singleton())) ? 1 : 0;
	}
	AVBOITPolicy::ViewInputs inputs;
	inputs.method = method;
	inputs.renderer = AVBOITPolicy::RENDERER_FORWARD_PLUS;
	// The resolve writes the view's color buffer as an RGBA16F storage image, which Forward+ always makes.
	inputs.device_features = avboit_device_features == 1 && rb->get_can_be_storage() && rb->get_base_data_format() == RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	inputs.msaa = p_use_msaa;
	inputs.view_count = rb->get_view_count();
	const AVBOITPolicy::Reason reason = AVBOITPolicy::evaluate(inputs);
	const String line = avboit_log.note(method, reason);
	if (!line.is_empty()) {
		print_line(line);
	}
	return reason == AVBOITPolicy::REASON_NONE;
}

// GodotNext: rd_readonly_depth (#301). Pass the transparent list's depth/stencil write scan so
// AVBOIT composite and remainder passes keep the depth buffer read-only whenever possible.
bool RenderForwardClustered::_render_avboit(RenderDataRD *p_render_data, RID p_alpha_framebuffer, uint32_t p_color_pass_flags, uint32_t p_uniform_buffer_index, const Size2i &p_screen_size, const Color &p_default_bg_color, bool p_reverse_cull, RID p_radiance_texture, const RendererRD::MaterialStorage::Samplers &p_samplers, SceneShaderForwardClustered::ShaderSpecialization p_base_specialization, const ForwardTransparentDepthScan &p_alpha_depth_scan) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND_V(rb.is_null(), false);
	RendererRD::AVBOIT::Targets targets;
	if (!RendererRD::AVBOIT::ensure_targets(rb.ptr(), rb->get_depth_texture(), targets)) {
		return false;
	}
	scene_shader.enable_avboit_shader_group();

	RenderList &alpha = render_list[RENDER_LIST_ALPHA];
	const float lod_distance_multiplier = p_render_data->scene_data->lod_distance_multiplier;
	const float screen_mesh_lod_threshold = p_render_data->scene_data->screen_mesh_lod_threshold;

	// 1 and 2: the bounds and occlusion pre-passes, at the grid's resolution. Each binds only the grid
	// it reads, never the one it draws into.
	RENDER_TIMESTAMP("AVBOIT Occlusion Pre-Pass");
	RD::get_singleton()->draw_command_begin_label("AVBOIT Occlusion Pre-Pass");
	const uint32_t grid_uniform_buffer_index = _setup_environment(p_render_data, false, targets.grid, targets.grid, Color(), false);
	avboit_bind_bounds = RID();
	avboit_bind_occlusion = RID();
	RID uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_ALPHA, p_render_data, false, p_radiance_texture, p_samplers, grid_uniform_buffer_index, true);
	{
		RenderListParameters params(alpha.elements.ptr(), alpha.element_info.ptr(), alpha.elements.size(), p_reverse_cull, PASS_MODE_DEPTH_MATERIAL, 0, false, p_render_data->directional_light_soft_shadows, uniform_set, false, Vector2(), lod_distance_multiplier, screen_mesh_lod_threshold, 1, 0, p_base_specialization, false);
		params.avboit_pass = RendererRD::AVBOIT::PASS_BOUNDS;
		Vector<Color> clear;
		clear.push_back(RendererRD::AVBOIT::bounds_clear_color());
		_render_list_with_draw_list(&params, targets.bounds_framebuffer, RD::DRAW_CLEAR_ALL, clear);
	}
	avboit_bind_bounds = targets.bounds;
	uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_ALPHA, p_render_data, false, p_radiance_texture, p_samplers, grid_uniform_buffer_index, true);
	{
		RenderListParameters params(alpha.elements.ptr(), alpha.element_info.ptr(), alpha.elements.size(), p_reverse_cull, PASS_MODE_DEPTH_MATERIAL, 0, false, p_render_data->directional_light_soft_shadows, uniform_set, false, Vector2(), lod_distance_multiplier, screen_mesh_lod_threshold, 1, 0, p_base_specialization, false);
		params.avboit_pass = RendererRD::AVBOIT::PASS_OCCLUSION;
		Vector<Color> clear;
		for (uint32_t i = 0; i < RendererRD::AVBOIT::LAYERS; i++) {
			clear.push_back(Color(0, 0, 0, 0));
		}
		_render_list_with_draw_list(&params, targets.occlusion_framebuffer, RD::DRAW_CLEAR_ALL, clear);
	}
	// The grid-sized scene data must not be what the frame is left with: POST_TRANSPARENT compositor
	// effects read the last one set up, through RenderSceneData::get_uniform_buffer(). Set up the
	// transparent pass's again, with its arguments (a view with AVBOIT is never a reflection probe).
	_setup_environment(p_render_data, false, p_screen_size, p_screen_size, p_default_bg_color, false);
	RD::get_singleton()->draw_command_end_label();

	// 3: the composite into the accumulation target, depth-tested against the view's depth.
	RENDER_TIMESTAMP("AVBOIT Composite");
	RD::get_singleton()->draw_command_begin_label("AVBOIT Composite");
	avboit_bind_occlusion = targets.occlusion;
	uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_ALPHA, p_render_data, false, p_radiance_texture, p_samplers, p_uniform_buffer_index, true);
	avboit_bind_bounds = RID();
	avboit_bind_occlusion = RID();
	{
		RenderListParameters params(alpha.elements.ptr(), alpha.element_info.ptr(), alpha.elements.size(), p_reverse_cull, PASS_MODE_COLOR, p_color_pass_flags, false, p_render_data->directional_light_soft_shadows, uniform_set, false, Vector2(), lod_distance_multiplier, screen_mesh_lod_threshold, 1, 0, p_base_specialization, true);
		params.avboit_pass = RendererRD::AVBOIT::PASS_COMPOSITE;
		Vector<Color> clear;
		clear.push_back(Color(0, 0, 0, 0));
		// GodotNext: rd_readonly_depth (#301). AVBOIT composite never writes depth; keep depth read-only unless a surface writes stencil.
		BitField<RD::DrawFlags> composite_flags = RD::DRAW_CLEAR_COLOR_0;
		if (p_alpha_depth_scan.avboit_composite_read_only_depth()) {
			composite_flags.set_flag(RD::DRAW_READ_ONLY_DEPTH);
		}
		_render_list_with_draw_list(&params, targets.composite_framebuffer, composite_flags, clear, 0.0f, 0u, p_render_data->render_region);
	}

	// 4: the resolve, over the region the composite drew.
	Rect2i rect(Point2i(), rb->get_internal_size());
	if (p_render_data->render_region.has_area()) {
		rect = rect.intersection(p_render_data->render_region);
	}
	if (rect.has_area()) {
		avboit.resolve(targets.accum, rb->get_internal_texture(), rect);
	}
	RD::get_singleton()->draw_command_end_label();

	// 5: blend modes that cannot be summed in any order (mul, blending disabled), in the sorted order.
	bool has_remainder = false;
	for (uint32_t i = 0; i < alpha.elements.size(); i++) {
		if (RendererRD::AVBOIT::surface_class(alpha.elements[i]->shader->blend_mode, alpha.elements[i]->shader->uses_shadow_to_opacity) == RendererRD::AVBOIT::SURFACE_REMAINDER) {
			has_remainder = true;
			break;
		}
	}
	if (has_remainder) {
		RenderListParameters params(alpha.elements.ptr(), alpha.element_info.ptr(), alpha.elements.size(), p_reverse_cull, PASS_MODE_COLOR, p_color_pass_flags, false, p_render_data->directional_light_soft_shadows, uniform_set, false, Vector2(), lod_distance_multiplier, screen_mesh_lod_threshold, 1, 0, p_base_specialization, true);
		params.avboit_pass = RendererRD::AVBOIT::PASS_REMAINDER;
		// GodotNext: rd_readonly_depth (#301).
		const BitField<RD::DrawFlags> remainder_flags = p_alpha_depth_scan.transparent_pass_read_only_depth() ? BitField<RD::DrawFlags>(RD::DRAW_READ_ONLY_DEPTH) : BitField<RD::DrawFlags>(RD::DRAW_DEFAULT_ALL);
		_render_list_with_draw_list(&params, p_alpha_framebuffer, remainder_flags, Vector<Color>(), 0.0f, 0u, p_render_data->render_region);
	}
	return true;
}

void RenderForwardClustered::_render_nextgi_front_layer(RenderDataRD *p_render_data, bool p_reverse_cull, const Size2i &p_screen_size, RID p_radiance_texture, const RendererRD::MaterialStorage::Samplers &p_samplers, SceneShaderForwardClustered::ShaderSpecialization p_base_specialization) {
	if (!bool(GLOBAL_GET("rendering/global_illumination/nextgi/translucency_reflections_front_layer_allow")) || p_render_data->environment.is_null() || !environment_get_nextgi_translucency_front_layer_reflections(p_render_data->environment) || next_gi.get_tier_result().tier != RendererRD::NextGI::TIER_HWRT) {
		return;
	}
	if (p_render_data->scene_data->view_count != 1 || render_list[RENDER_LIST_ALPHA].elements.is_empty()) {
		return;
	}
	if (!front_layer.ensure_targets(p_screen_size)) {
		return;
	}
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	const RSE::ViewportMSAA msaa = rb->get_msaa_3d();
	if (msaa != RSE::VIEWPORT_MSAA_DISABLED) {
		resolve_effects->resolve_depth(rb->get_depth_msaa(0), rb->get_depth_texture(0), p_screen_size, 1 << int(msaa));
	}
	if (!front_layer.copy_opaque_depth(rb->get_depth_texture(0), copy_effects)) {
		return;
	}

	RENDER_TIMESTAMP("NextGI Front Layer");
	RD::get_singleton()->draw_command_begin_label("NextGI Front Layer");
	p_render_data->scene_data->nextgi_front_layer = true;
	const uint32_t uniform_buffer_index = _setup_environment(p_render_data, false, p_screen_size, p_screen_size, Color(), false);
	const RID uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_ALPHA, p_render_data, false, p_radiance_texture, p_samplers, uniform_buffer_index, true, true);
	RenderListParameters params(render_list[RENDER_LIST_ALPHA].elements.ptr(), render_list[RENDER_LIST_ALPHA].element_info.ptr(), render_list[RENDER_LIST_ALPHA].elements.size(), p_reverse_cull, PASS_MODE_DEPTH_NORMAL_ROUGHNESS_FRONT_LAYER, 0, false, p_render_data->directional_light_soft_shadows, uniform_set, false, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, 1, 0, p_base_specialization, false);
	Vector<Color> clear;
	clear.push_back(Color(0, 0, 0, 0));
	_render_list_with_draw_list(&params, front_layer.get_framebuffer(), RD::DRAW_CLEAR_ALL, clear, 0.0f, 0u, p_render_data->render_region);
	p_render_data->scene_data->nextgi_front_layer = false;
	front_layer.trace(p_render_data);
	RD::get_singleton()->draw_command_end_label();
}

void RenderForwardClustered::_render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) {
	scene_state.used_uniform_buffer_count = 0;
	const bool visibility_only = bool(GLOBAL_GET("rendering/nextgeo/visibility_only"));
	if (RSG::virtual_mesh_storage != nullptr) {
		RSG::virtual_mesh_storage->set_nextgeo_draw_ready(!visibility_only);
	}
	_update_dirty_geometry_instances();
	if (RSG::virtual_mesh_storage != nullptr) {
		// GodotNext: nextgeo_exclusive_dispatch (#79). These must stay 0.
		if (p_render_data == nullptr || !p_render_data->reflection_probe.is_valid()) {
			RSG::virtual_mesh_storage->nextgeo_reset_counters();
		}
		// GodotNext: nextgeo_instance_records (#75). Upload dirty records
		// before this frame's views inspect record buffers; idempotent after
		// the first view.
		RSG::virtual_mesh_storage->flush();
		nextgeo_cut.prepare_gpu_records(RSG::virtual_mesh_storage);
	}

	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	ERR_FAIL_NULL(p_render_data);

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());
	Ref<RenderBufferDataForwardClustered> rb_data;
	if (rb->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		// Our forward clustered custom data buffer will only be available when we're rendering our normal view.
		// This will not be available when rendering reflection probes.
		rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	}
	bool is_reflection_probe = p_render_data->reflection_probe.is_valid();
	bool is_multiview = rb->get_view_count() > 1;

	static const int texture_multisamples[RSE::VIEWPORT_MSAA_MAX] = { 1, 2, 4, 8 };

	//first of all, make a new render pass
	//fill up ubo

	RENDER_TIMESTAMP("Prepare 3D Scene");

	// get info about our rendering effects
	bool ce_needs_motion_vectors = _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_MOTION_VECTORS);
	bool ce_needs_normal_roughness = _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_ROUGHNESS);
	bool ce_needs_separate_specular = _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_SEPARATE_SPECULAR);

	// GodotNext: nextgi_forward — a NextGI view does not run SDFGI. VoxelGI
	// supplies diffuse lighting only in reflections-only mode.
	// GodotNext: ps_swrt_trace (#186). The software tier enters the same frame block (one view at a time).
	const bool nextgi_view = p_render_data->environment.is_valid() && RendererRD::NextGI::view_uses_frame_block(next_gi.get_tier_result().tier, is_reflection_probe, environment_get_nextgi_enabled(p_render_data->environment), get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_UNSHADED, p_render_data->scene_data->view_count);
	// GodotNext: nextgi_xr_off (#255). Count XR views that NextGI leaves to the non-NextGI ambient.
	if (p_render_data->environment.is_valid() && RendererRD::NextGI::view_skipped_for_multiview(next_gi.get_tier_result().tier, is_reflection_probe, environment_get_nextgi_enabled(p_render_data->environment), p_render_data->scene_data->view_count)) {
		RendererRD::NextGI::note_multiview_view_skipped();
	}
	const bool nextgi_voxel_gi = nextgi_view && RendererRD::NextGI::is_reflections_only();

	// sdfgi first
	if (!nextgi_view) {
		_update_sdfgi(p_render_data);
	}

	// assign render indices to voxel_gi_instances
	for (uint32_t i = 0; i < (uint32_t)p_render_data->voxel_gi_instances->size(); i++) {
		RID voxel_gi_instance = (*p_render_data->voxel_gi_instances)[i];
		gi.voxel_gi_instance_set_render_index(voxel_gi_instance, i);
	}

	// obtain cluster builder
	if (light_storage->owns_reflection_probe_instance(p_render_data->reflection_probe)) {
		current_cluster_builder = light_storage->reflection_probe_instance_get_cluster_builder(p_render_data->reflection_probe, &cluster_builder_shared);

		if (p_render_data->camera_attributes.is_valid()) {
			light_storage->reflection_probe_set_baked_exposure(light_storage->reflection_probe_instance_get_probe(p_render_data->reflection_probe), RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes));
		}
	} else if (rb_data.is_valid()) {
		current_cluster_builder = rb_data->cluster_builder;

		p_render_data->voxel_gi_count = 0;

		if (!nextgi_view && rb->has_custom_data(RB_SCOPE_SDFGI)) {
			Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
			if (sdfgi.is_valid()) {
				sdfgi->update_cascades();
				sdfgi->pre_process_gi(p_render_data->scene_data->cam_transform, p_render_data);
				sdfgi->update_light();
			}
		}

		if (!nextgi_view || nextgi_voxel_gi) {
			gi.setup_voxel_gi_instances(p_render_data, p_render_data->render_buffers, p_render_data->scene_data->cam_transform, *p_render_data->voxel_gi_instances, p_render_data->voxel_gi_count);
		}
	} else {
		ERR_PRINT("No render buffer nor reflection atlas, bug"); // Should never happen!
		current_cluster_builder = nullptr;
		return; // No point in continuing, we'll just crash.
	}

	ERR_FAIL_NULL(current_cluster_builder);

	p_render_data->cluster_buffer = current_cluster_builder->get_cluster_buffer();
	p_render_data->cluster_size = current_cluster_builder->get_cluster_size();
	p_render_data->cluster_max_elements = current_cluster_builder->get_max_cluster_elements();

	_update_vrs(rb);

	RENDER_TIMESTAMP("Setup 3D Scene");

	bool using_debug_mvs = get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_MOTION_VECTORS;
	bool using_taa = rb->get_use_taa();
	const bool reset_temporal_history = !is_reflection_probe && rb_data.is_valid() && rb->consume_temporal_history_reset();
	TemporalUpscaler *external_upscaler = nullptr;

	enum {
		SCALE_NONE,
		SCALE_FSR2,
		SCALE_MFX,
		SCALE_EXTERNAL,
		SCALE_TSR,
	} scale_type = SCALE_NONE;

	switch (rb->get_scaling_3d_mode()) {
		case RSE::VIEWPORT_SCALING_3D_MODE_FSR2:
			scale_type = SCALE_FSR2;
			break;
		case RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL:
#ifdef METAL_MFXTEMPORAL_ENABLED
			scale_type = SCALE_MFX;
#else
			scale_type = SCALE_NONE;
#endif
			break;
		case RSE::VIEWPORT_SCALING_3D_MODE_DLSS:
		// GodotNext: pt_temporal_upscaler_extension (#192).
		case RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM:
			external_upscaler = TemporalUpscalerRegistry::get(rb->get_scaling_3d_mode());
			if (external_upscaler != nullptr) {
				scale_type = SCALE_EXTERNAL;
			}
			break;
		case RSE::VIEWPORT_SCALING_3D_MODE_TSR:
			// GodotNext: pt_tsr_d3d12_metal (#195). Fall back to plain 3D scaling (SCALE_NONE)
			// when the device lacks a required RB_SCOPE_TSR storage format.
			if (tsr_effect != nullptr) {
				if (tsr_effect->get_format_selection().supported) {
					scale_type = SCALE_TSR;
				} else if (!is_reflection_probe) {
					tsr_effect->warn_unsupported_once();
				}
			}
			break;
		default:
			break;
	}

	bool using_upscaling = scale_type != SCALE_NONE;
	if (reset_temporal_history) {
		print_verbose(vformat("GODOTNEXT_TEMPORAL_RESET mode=%d", scale_type));
	}

	// check if we need motion vectors
	bool motion_vectors_required;
	if (using_debug_mvs) {
		motion_vectors_required = true;
	} else if (ce_needs_motion_vectors) {
		motion_vectors_required = true;
	} else if (!is_reflection_probe && using_taa) {
		motion_vectors_required = true;
	} else if (!is_reflection_probe && using_upscaling) {
		motion_vectors_required = true;
	} else {
		motion_vectors_required = false;
	}

	//p_render_data->scene_data->subsurface_scatter_width = subsurface_scatter_size;
	p_render_data->scene_data->calculate_motion_vectors = motion_vectors_required;
	p_render_data->scene_data->directional_light_count = 0;
	p_render_data->scene_data->opaque_prepass_threshold = 0.99f;

	Size2i screen_size;
	RID color_framebuffer;
	RID color_only_framebuffer;
	RID depth_framebuffer;
	RendererRD::MaterialStorage::Samplers samplers;

	PassMode depth_pass_mode = PASS_MODE_DEPTH;
	uint32_t color_pass_flags = 0;
	Vector<Color> depth_pass_clear;
	bool using_separate_specular = false;
	bool using_ssao = !is_reflection_probe && p_render_data->environment.is_valid() && environment_get_ssao_enabled(p_render_data->environment);
	bool using_ssr = false;
	bool using_sscs = false;
	bool using_sdfgi = false;
	bool using_voxelgi = false;
	// GodotNext: nextgi_frame_block
	bool using_nextgi = false;
	// GodotNext: rt_shadows (#141). Standalone HWRT tile-sparse ray-traced shadows.
	bool using_rt_shadows = false;
	bool prepass_velocity_consumer = false;
	// GodotNext: nextgeo_visibility_raster (#83)
	bool using_nextgeo = false;
	bool nextgeo_drew = false;
	// GodotNext: rr_guides (#205). True only when this view writes the DLSS-RR guides.
	bool rr_guides_active = false;
	NextGIVelocityMode velocity_mode = NEXTGI_VELOCITY_NONE;
	NextGIVelocityScale velocity_scale = NEXTGI_VELOCITY_SCALE_NONE;
	NextGIVelocityPlan velocity_plan;
	bool reverse_cull = p_render_data->scene_data->cam_transform.basis.determinant() < 0;
	bool using_ssil = !is_reflection_probe && p_render_data->environment.is_valid() && environment_get_ssil_enabled(p_render_data->environment);
	bool using_motion_pass = rb_data.is_valid() && using_upscaling;
	NextGeoBoolScope nextgeo_scene_proxy_scope(nextgeo_proxy_active, is_reflection_probe);

	if (is_reflection_probe) {
		uint32_t resolution = light_storage->reflection_probe_instance_get_resolution(p_render_data->reflection_probe);
		screen_size.x = resolution;
		screen_size.y = resolution;

		color_framebuffer = light_storage->reflection_probe_instance_get_framebuffer(p_render_data->reflection_probe, p_render_data->reflection_probe_pass);
		color_only_framebuffer = color_framebuffer;
		depth_framebuffer = light_storage->reflection_probe_instance_get_depth_framebuffer(p_render_data->reflection_probe, p_render_data->reflection_probe_pass);

		if (light_storage->reflection_probe_is_interior(light_storage->reflection_probe_instance_get_probe(p_render_data->reflection_probe))) {
			p_render_data->environment = RID(); //no environment on interiors
		}

		reverse_cull = true; // for some reason our views are inverted
		samplers = RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default();

		// Indicate pipelines for reflection probes are required.
		global_pipeline_data_required.use_reflection_probes = true;
	} else {
		screen_size = rb->get_internal_size();

		if (rb_data.is_valid() && p_render_data->scene_data != nullptr && RSG::virtual_mesh_storage != nullptr && RSG::virtual_mesh_storage->get_live_record_count() > 0 && RSG::virtual_mesh_storage->get_record_count() > 0 && RSG::virtual_mesh_storage->get_record_buffer().is_valid() && RSG::virtual_mesh_storage->get_max_bvh_levels() > 0) {
			const bool unsupported_debug_draw = RendererRD::nextgeo_is_unsupported_debug_draw(get_debug_draw_mode());
			const bool view_msaa = rb->get_texture_samples() != RD::TEXTURE_SAMPLES_1;
			using_nextgeo = RendererRD::nextgeo_view_fallback(view_msaa, p_render_data->scene_data->view_count > 1, unsupported_debug_draw) == RendererRD::NEXTGEO_FALLBACK_NONE;
			if (using_nextgeo && !visibility_only) {
				scene_shader.enable_nextgeo_shader_group();
			}
		}

		if (p_render_data->scene_data->calculate_motion_vectors) {
			color_pass_flags |= COLOR_PASS_FLAG_MOTION_VECTORS;
			scene_shader.enable_advanced_shader_group();

			// Indicate pipelines for motion vectors are required.
			global_pipeline_data_required.use_motion_vectors = true;
		}

		if (p_render_data->voxel_gi_instances->size() > 0) {
			using_voxelgi = true;
		}

		if (p_render_data->environment.is_valid()) {
			if (environment_get_sdfgi_enabled(p_render_data->environment) && get_debug_draw_mode() != RSE::VIEWPORT_DEBUG_DRAW_UNSHADED) {
				using_sdfgi = true;
			}
			if (environment_get_ssr_enabled(p_render_data->environment)) {
				if (!p_render_data->transparent_bg) {
					using_ssr = true;
				} else {
					WARN_PRINT_ONCE("Screen-space reflections are not supported in viewports with a transparent background. Disabling SSR in transparent viewport.");
				}
			}
			// GodotNext: nextgi_frame_block
			using_nextgi = nextgi_view;
			nextgi_active = using_nextgi;
			if (using_nextgi) {
				using_sdfgi = false;
				using_voxelgi = nextgi_voxel_gi;
				if (using_ssil) {
					WARN_PRINT_ONCE("Screen-space indirect lighting is disabled for views that use NextGI.");
				}
				using_ssil = false;
			}
		}

		if (scale_type == SCALE_FSR2) {
			velocity_mode = NEXTGI_VELOCITY_SENTINEL;
			velocity_scale = NEXTGI_VELOCITY_SCALE_FSR2;
		} else if (scale_type == SCALE_MFX) {
			velocity_mode = NEXTGI_VELOCITY_DENSE;
			velocity_scale = NEXTGI_VELOCITY_SCALE_MFX;
		} else if (scale_type == SCALE_EXTERNAL) {
			if (external_upscaler == nullptr || external_upscaler->velocity_mode() == TemporalUpscaler::VELOCITY_DENSE) {
				velocity_mode = NEXTGI_VELOCITY_DENSE;
			}
			velocity_scale = NEXTGI_VELOCITY_SCALE_EXTERNAL;
		} else if (scale_type == SCALE_TSR) {
			velocity_mode = NEXTGI_VELOCITY_DENSE;
			velocity_scale = NEXTGI_VELOCITY_SCALE_TSR;
		} else if (using_taa || ce_needs_motion_vectors) {
			velocity_mode = NEXTGI_VELOCITY_TAA;
		}
		// GodotNext: rt_shadows (#141, plan §21.3 contract M9). Standalone HWRT ray-traced shadows
		// reuse #37's prepass velocity plan alongside depth and normal-roughness.
		using_rt_shadows = _rt_view_has_lights(p_render_data);
		prepass_velocity_consumer = using_nextgi || using_rt_shadows;
		velocity_plan = RendererRD::nextgeo_velocity_plan(velocity_scale, velocity_mode, using_taa, prepass_velocity_consumer, using_nextgeo);
		if (velocity_plan.prepass_writes) {
			if (!p_render_data->scene_data->calculate_motion_vectors) {
				color_pass_flags |= COLOR_PASS_FLAG_MOTION_VECTORS;
			}
			scene_shader.enable_advanced_shader_group(is_multiview);
			global_pipeline_data_required.use_motion_vectors = true;
			rb->ensure_velocity();
			p_render_data->scene_data->calculate_motion_vectors = true;
		}

		// GodotNext: light_source_parity (#140). Collect visible lights with active per-light contact shadows
		// (directional first, then local lights up to MAX_SSCS_LIGHTS_PER_VIEW). When none are active
		// (e.g. contact_shadow_length == 0), using_sscs remains false so rendering is byte-identical to off.
		p_render_data->contact_shadow_lights.clear();
		p_render_data->contact_shadow_light_indices.clear();
		if (rb_data.is_valid() && ss_effects && p_render_data->lights != nullptr) {
			const bool project_cs_enabled = GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/contact_shadow/enabled");
			const int project_cs_length_enum = GLOBAL_GET_CACHED(int, "rendering/lights_and_shadows/contact_shadow/shadow_length");
			auto try_add_cs_light = [&](RID p_li, bool p_want_directional) {
				if (p_render_data->contact_shadow_lights.size() >= RendererRD::LightSourceParity::MAX_SSCS_LIGHTS_PER_VIEW) {
					return;
				}
				if (!light_storage->owns_light_instance(p_li)) {
					return;
				}
				const RID base = light_storage->light_instance_get_base_light(p_li);
				const RSE::LightType ltype = light_storage->light_get_type(base);
				if ((ltype == RSE::LIGHT_DIRECTIONAL) != p_want_directional) {
					return;
				}
				if (ltype == RSE::LIGHT_DIRECTIONAL && light_storage->light_directional_get_sky_mode(base) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
					return;
				}
				const RendererRD::LightSourceParity::ResolvedContactShadow resolved = RendererRD::LightSourceParity::resolve_contact_shadow(
						light_storage->light_get_param(base, RSE::LIGHT_PARAM_CONTACT_SHADOW_LENGTH),
						light_storage->light_get_contact_shadow_length_world_space(base),
						light_storage->light_get_allow_contact_shadows(base),
						light_storage->light_get_param(base, RSE::LIGHT_PARAM_CONTACT_SHADOW_CASTING_INTENSITY),
						light_storage->light_get_param(base, RSE::LIGHT_PARAM_CONTACT_SHADOW_NON_CASTING_INTENSITY),
						project_cs_enabled,
						project_cs_length_enum);
				if (!resolved.active) {
					return;
				}
				const uint32_t slot = p_render_data->contact_shadow_lights.size();
				RenderDataRD::ContactShadowLight entry;
				entry.light_instance = p_li;
				entry.type = ltype;
				entry.length = resolved.length;
				entry.world_space = resolved.world_space;
				entry.blur = light_storage->light_get_param(base, RSE::LIGHT_PARAM_CONTACT_SHADOW_BLUR);
				p_render_data->contact_shadow_lights.push_back(entry);
				p_render_data->contact_shadow_light_indices.insert(p_li, slot);
			};
			for (uint32_t i = 0; i < p_render_data->lights->size(); i++) {
				try_add_cs_light((*p_render_data->lights)[i], true);
			}
			for (uint32_t i = 0; i < p_render_data->lights->size(); i++) {
				try_add_cs_light((*p_render_data->lights)[i], false);
			}
			using_sscs = !p_render_data->contact_shadow_lights.is_empty();
		}

		if (p_render_data->scene_data->view_count > 1) {
			color_pass_flags |= COLOR_PASS_FLAG_MULTIVIEW;
			// Try enabling here in case is_xr_enabled() returns false.
			scene_shader.shader.enable_group(SceneShaderForwardClustered::SHADER_GROUP_MULTIVIEW);

			// Indicate pipelines for multiview are required.
			global_pipeline_data_required.use_multiview = true;
		}

		// GodotNext: rr_guides (#205). The opaque passes of this view write the two albedo guides.
		// enable_view_shader_groups() is the only place the RR shader groups are enabled; the
		// doctest checks its gate.
		rr_guides_active = RendererRD::RRGuides::enable_view_shader_groups(scene_shader, RendererRD::RRGuides::is_requested(), false, p_render_data->scene_data->view_count, rb->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED, using_nextgeo && !visibility_only);
		if (rr_guides_active) {
			rb->ensure_rr_guides();
			color_pass_flags |= COLOR_PASS_FLAG_RR_GUIDES;
		} else {
			rb->free_rr_guides();
		}

		color_framebuffer = rb_data->get_color_pass_fb(color_pass_flags);
		color_only_framebuffer = rb_data->get_color_only_fb();
		samplers = rb->get_samplers();
	}

	p_render_data->scene_data->emissive_exposure_normalization = -1.0;

	RD::get_singleton()->draw_command_begin_label("Render Setup");

	_setup_lightmaps(p_render_data, *p_render_data->lightmaps, p_render_data->scene_data->cam_transform);
	_setup_voxelgis(*p_render_data->voxel_gi_instances);
	uint32_t depth_prepass_uniform_buffer_index = _setup_environment(p_render_data, is_reflection_probe, screen_size, screen_size, p_default_bg_color, false);

	// May have changed due to the above (light buffer enlarged, as an example).
	_update_render_base_uniform_set();

	nextgeo_virtual_active = using_nextgeo;
	nextgeo_visibility_only_active = visibility_only;
	// GodotNext: nextgeo_tessellation (#163). Re-bin displaced virtual surfaces immediately when
	// experimental/tessellation toggles at runtime.
	{
		const bool tess_enabled_now = bool(GLOBAL_GET("rendering/nextgeo/experimental/tessellation"));
		if (tess_enabled_now != nextgeo_last_tessellation_enabled) {
			nextgeo_last_tessellation_enabled = tess_enabled_now;
			bool marked_any = false;
			if (p_render_data->instances != nullptr) {
				for (uint32_t i = 0; i < p_render_data->instances->size(); i++) {
					GeometryInstanceForwardClustered *inst = static_cast<GeometryInstanceForwardClustered *>((*p_render_data->instances)[i]);
					if (inst == nullptr) {
						continue;
					}
					for (GeometryInstanceSurfaceDataCache *surf = inst->surface_caches; surf != nullptr; surf = surf->next) {
						if (surf->material != nullptr && (surf->material->get_raster_features() & RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) != 0u) {
							inst->_mark_dirty();
							marked_any = true;
							break;
						}
					}
				}
			}
			for (GeometryInstanceSurfaceDataCache &surf : geometry_surface_compilation_all_list) {
				if (surf.owner != nullptr && surf.material != nullptr && (surf.material->get_raster_features() & RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) != 0u) {
					surf.owner->_mark_dirty();
					marked_any = true;
				}
			}
			if (marked_any) {
				_update_dirty_geometry_instances();
			}
		}
	}
	// GodotNext: nextgeo_raster_bins (#155). Programmable raster bins draw on the hybrid main view only;
	// elsewhere their surfaces draw on legacy raster.
	{
		const bool main_view = !is_reflection_probe && rb.is_valid() && (RSG::viewport == nullptr || RSG::viewport->get_main_3d_render_target().is_null() || rb->get_render_target() == RSG::viewport->get_main_3d_render_target());
		nextgeo_programmable_view_active = using_nextgeo && main_view && RSG::virtual_mesh_storage != nullptr && RSG::virtual_mesh_storage->get_raster_bins().get_active_count() > 0u && _nextgeo_view_runs_hybrid_raster();
	}
	// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7). Compute shading bins when
	// rendering/nextgeo/shading_path asks for them; auto keeps the tile quads through the build (§22).
	nextgeo_shading_compute_active = using_nextgeo && !visibility_only && RendererRD::nextgeo_shading_path_wants_compute(RendererRD::NextGeoShadingPath(int(GLOBAL_GET("rendering/nextgeo/shading_path"))));
	nextgeo_shading_stage_a_reason = RendererRD::NEXTGEO_SHADING_REASON_SETTING;
	nextgeo_shading_stage_b_reason = RendererRD::NEXTGEO_SHADING_REASON_SETTING;
	nextgeo_shading_stage_a_variant = -1;
	nextgeo_shading_stage_b_variant = -1;

	// GodotNext: megalights_inputs (#136). #143 takes over MegaLightsInputs::is_requested().
	// Reflection probes leave the main view's targets alone. enable_view_shader_groups() is
	// the only place the MegaLights shader groups are enabled; the doctest checks its gate.
	if (!is_reflection_probe && rb.is_valid()) {
		megalights_inputs_active = RendererRD::MegaLightsInputs::enable_view_shader_groups(scene_shader, rb->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED, p_render_data->scene_data->view_count > 1, using_nextgeo);
		if (!megalights_inputs_active && RendererRD::MegaLights::has_pending_check() && rb->get_msaa_3d() == RSE::VIEWPORT_MSAA_DISABLED && p_render_data->scene_data->view_count == 1) {
			scene_shader.enable_megalights_shader_group(false);
			if (using_nextgeo) {
				scene_shader.enable_nextgeo_megalights_shader_group();
			}
			megalights_inputs_active = true;
		}
		if (megalights_inputs_active) {
			rb->ensure_megalights_inputs();
		} else {
			rb->free_megalights_inputs();
		}
	} else {
		megalights_inputs_active = false;
	}
	RendererRD::MegaLightsInputs::begin_layer_groups();

	// GodotNext: nextgi_frame_block / rt_shadows (#141)
	// NextGI instances get INSTANCE_DATA_FLAG_USE_GI_BUFFERS, so the opaque pass reads RB_TEX_AMBIENT.
	_fill_render_list(RENDER_LIST_OPAQUE, p_render_data, PASS_MODE_COLOR, using_sdfgi, using_sdfgi || using_voxelgi || using_nextgi, using_motion_pass, false, prepass_velocity_consumer && velocity_plan.prepass_writes, using_nextgi);
	render_list[RENDER_LIST_OPAQUE].sort_by_key();
	render_list[RENDER_LIST_MOTION].sort_by_key();
	render_list[RENDER_LIST_ALPHA].sort_by_reverse_depth_and_priority();

	int *render_info = p_render_data->render_info ? p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE] : (int *)nullptr;
	_fill_instance_data(RENDER_LIST_OPAQUE, render_info);
	_fill_instance_data(RENDER_LIST_MOTION, render_info);
	_fill_instance_data(RENDER_LIST_ALPHA, render_info);

	RD::get_singleton()->draw_command_end_label();

	// GodotNext: df_shadows (#214, plan §21.3 contract S7). Need depth + normal-roughness prepass when DF shadows run.
	const bool using_df_shadows = !is_reflection_probe && _df_view_has_lights(p_render_data);

	if (!is_reflection_probe) {
		if (using_voxelgi) {
			depth_pass_mode = PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI;
		} else if (p_render_data->environment.is_valid()) {
			if (using_ssr ||
					using_sdfgi ||
					// GodotNext: nextgi_frame_block
					using_nextgi ||
					// GodotNext: nextgeo_visibility_raster (#83)
					using_nextgeo ||
					environment_get_ssao_enabled(p_render_data->environment) ||
					using_ssil ||
					using_sscs ||
					ce_needs_normal_roughness ||
					get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_NORMAL_BUFFER ||
					scene_state.used_normal_texture) {
				depth_pass_mode = PASS_MODE_DEPTH_NORMAL_ROUGHNESS;
			}
		} else if (using_nextgeo || get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_NORMAL_BUFFER || scene_state.used_normal_texture) {
			depth_pass_mode = PASS_MODE_DEPTH_NORMAL_ROUGHNESS;
		}

		// GodotNext: megalights_inputs (#136). The input targets and the ML_ALBEDO / ML_MATERIAL
		// debug draws need the normal-roughness prepass. Do not downgrade a voxel GI prepass.
		// GodotNext: rr_guides (#205). So does the RR guide pack.
		// GodotNext: df_shadows (#214) / rt_shadows (#141). DF and standalone RT shadows read prepass normal-roughness.
		const bool ml_debug_draw = get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_ML_ALBEDO || get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_ML_MATERIAL;
		if ((megalights_inputs_active || ml_debug_draw || rr_guides_active || using_df_shadows || using_rt_shadows) && depth_pass_mode == PASS_MODE_DEPTH) {
			depth_pass_mode = PASS_MODE_DEPTH_NORMAL_ROUGHNESS;
		}

		switch (depth_pass_mode) {
			case PASS_MODE_DEPTH: {
				depth_framebuffer = rb_data->get_depth_fb();
			} break;
			case PASS_MODE_DEPTH_NORMAL_ROUGHNESS: {
				// GodotNext: megalights_inputs (#136). Two clears sit between normal-roughness and velocity.
				if (megalights_inputs_active) {
					if (prepass_velocity_consumer && velocity_plan.prepass_writes) {
						depth_framebuffer = rb_data->get_depth_fb(RenderBufferDataForwardClustered::DEPTH_FB_ROUGHNESS_VELOCITY_ML);
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
						depth_pass_clear.push_back(Color(velocity_plan.pre_clear_r, velocity_plan.pre_clear_g, 0, 0));
					} else {
						depth_framebuffer = rb_data->get_depth_fb(RenderBufferDataForwardClustered::DEPTH_FB_ROUGHNESS_ML);
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
					}
				} else if (prepass_velocity_consumer && velocity_plan.prepass_writes) {
					depth_framebuffer = rb_data->get_depth_fb(RenderBufferDataForwardClustered::DEPTH_FB_ROUGHNESS_VELOCITY);
					depth_pass_clear.push_back(Color(0, 0, 0, 0));
					depth_pass_clear.push_back(Color(velocity_plan.pre_clear_r, velocity_plan.pre_clear_g, 0, 0));
				} else {
					depth_framebuffer = rb_data->get_depth_fb(RenderBufferDataForwardClustered::DEPTH_FB_ROUGHNESS);
					depth_pass_clear.push_back(Color(0, 0, 0, 0));
				}
			} break;
			case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI: {
				// GodotNext: megalights_inputs (#136)
				if (megalights_inputs_active) {
					if (prepass_velocity_consumer && velocity_plan.prepass_writes) {
						depth_framebuffer = rb_data->get_depth_fb(RenderBufferDataForwardClustered::DEPTH_FB_ROUGHNESS_VOXELGI_VELOCITY_ML);
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
						depth_pass_clear.push_back(Color(velocity_plan.pre_clear_r, velocity_plan.pre_clear_g, 0, 0));
					} else {
						depth_framebuffer = rb_data->get_depth_fb(RenderBufferDataForwardClustered::DEPTH_FB_ROUGHNESS_VOXELGI_ML);
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
						depth_pass_clear.push_back(Color(0, 0, 0, 0));
					}
				} else if (prepass_velocity_consumer && velocity_plan.prepass_writes) {
					depth_framebuffer = rb_data->get_depth_fb(RenderBufferDataForwardClustered::DEPTH_FB_ROUGHNESS_VOXELGI_VELOCITY);
					depth_pass_clear.push_back(Color(0, 0, 0, 0));
					depth_pass_clear.push_back(Color(0, 0, 0, 0));
					depth_pass_clear.push_back(Color(velocity_plan.pre_clear_r, velocity_plan.pre_clear_g, 0, 0));
				} else {
					depth_framebuffer = rb_data->get_depth_fb(RenderBufferDataForwardClustered::DEPTH_FB_ROUGHNESS_VOXELGI);
					depth_pass_clear.push_back(Color(0, 0, 0, 0));
					depth_pass_clear.push_back(Color(0, 0, 0, 0));
				}
			} break;
			default: {
			};
		}
	}

	bool using_sss = rb_data.is_valid() && !is_reflection_probe && scene_state.used_sss && ss_effects->sss_get_quality() != RSE::SUB_SURFACE_SCATTERING_QUALITY_DISABLED;

	if (using_sss && p_render_data->transparent_bg) {
		WARN_PRINT_ONCE("Sub-surface scattering is not supported in viewports with a transparent background. Disabling SSS in transparent viewport.");
		using_sss = false;
	}

	if ((using_sss || ce_needs_separate_specular) && !using_separate_specular) {
		using_separate_specular = true;
		color_pass_flags |= COLOR_PASS_FLAG_SEPARATE_SPECULAR;
		color_framebuffer = rb_data->get_color_pass_fb(color_pass_flags);
	}

	// Ensure this is allocated so we don't get a stutter the first time an object with SSS appears on screen.
	if (global_surface_data.sss_used && !is_reflection_probe) {
		rb_data->ensure_specular();
	}

	if (global_surface_data.normal_texture_used && !is_reflection_probe) {
		rb_data->ensure_normal_roughness_texture();
	}

	if (using_sss || using_separate_specular || scene_state.used_lightmap || scene_state.lightmaps_used > 0 || using_voxelgi || global_surface_data.sss_used) {
		scene_shader.enable_advanced_shader_group(p_render_data->scene_data->view_count > 1);
	}

	// Update the global pipeline requirements with all the features found to be in use in this scene.
	if (depth_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS || global_surface_data.normal_texture_used) {
		global_pipeline_data_required.use_normal_and_roughness = true;
	}

	if (scene_state.used_lightmap || scene_state.lightmaps_used > 0) {
		global_pipeline_data_required.use_lightmaps = true;
	}

	if (using_voxelgi) {
		global_pipeline_data_required.use_voxelgi = true;
	}

	if (using_separate_specular || global_surface_data.sss_used) {
		global_pipeline_data_required.use_separate_specular = true;
	}

	// Update the compiled pipelines if any of the requirements have changed.
	_update_dirty_geometry_pipelines();

	RID radiance_texture;
	bool draw_sky = false;
	bool draw_sky_fog_only = false;
	// We invert luminance_multiplier for sky so that we can combine it with exposure value.
	float sky_luminance_multiplier = 1.0 / rb->get_luminance_multiplier();
	float sky_brightness_multiplier = 1.0;

	Color clear_color;
	bool load_color = false;

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW) {
		clear_color = Color(0, 0, 0, 1); //in overdraw mode, BG should always be black
	} else if (is_environment(p_render_data->environment)) {
		RSE::EnvironmentBG bg_mode = environment_get_background(p_render_data->environment);
		float bg_energy_multiplier = environment_get_bg_energy_multiplier(p_render_data->environment);
		bg_energy_multiplier *= environment_get_bg_intensity(p_render_data->environment);
		RSE::EnvironmentReflectionSource reflection_source = environment_get_reflection_source(p_render_data->environment);

		if (p_render_data->camera_attributes.is_valid()) {
			bg_energy_multiplier *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
		}

		switch (bg_mode) {
			case RSE::ENV_BG_CLEAR_COLOR:
			case RSE::ENV_BG_COLOR: {
				clear_color = bg_mode == RSE::ENV_BG_CLEAR_COLOR ? p_default_bg_color : environment_get_bg_color(p_render_data->environment);

				if (!p_render_data->transparent_bg && (rb->has_custom_data(RB_SCOPE_FOG) || environment_get_fog_enabled(p_render_data->environment))) {
					draw_sky_fog_only = true;
					RendererRD::MaterialStorage::get_singleton()->material_set_param(sky.sky_scene_state.fog_material, "clear_color", Variant(clear_color));
				}

				clear_color = clear_color.srgb_to_linear();
				clear_color.r *= bg_energy_multiplier;
				clear_color.g *= bg_energy_multiplier;
				clear_color.b *= bg_energy_multiplier;
			} break;
			case RSE::ENV_BG_SKY: {
				draw_sky = !p_render_data->transparent_bg;
			} break;
			case RSE::ENV_BG_CANVAS: {
				if (!is_reflection_probe) {
					RID texture = RendererRD::TextureStorage::get_singleton()->render_target_get_rd_texture(rb->get_render_target());
					bool convert_to_linear = !RendererRD::TextureStorage::get_singleton()->render_target_is_using_hdr(rb->get_render_target());
					copy_effects->copy_to_fb_rect(texture, color_only_framebuffer, Rect2i(), false, false, false, false, RID(), false, false, convert_to_linear);
				}
				load_color = true;
			} break;
			case RSE::ENV_BG_KEEP: {
				load_color = true;
			} break;
			case RSE::ENV_BG_CAMERA_FEED: {
			} break;
			default: {
			}
		}

		// setup sky if used for ambient, reflections, or background
		if (draw_sky || draw_sky_fog_only || (reflection_source == RSE::ENV_REFLECTION_SOURCE_BG && bg_mode == RSE::ENV_BG_SKY) || reflection_source == RSE::ENV_REFLECTION_SOURCE_SKY || environment_get_ambient_source(p_render_data->environment) == RSE::ENV_AMBIENT_SOURCE_SKY) {
			RENDER_TIMESTAMP("Setup Sky");
			RD::get_singleton()->draw_command_begin_label("Setup Sky");

			// Setup our sky render information for this frame/viewport
			sky.setup_sky(p_render_data, screen_size);

			sky_brightness_multiplier *= bg_energy_multiplier;

			RID sky_rid = environment_get_sky(p_render_data->environment);
			if (sky_rid.is_valid()) {
				sky.update_radiance_buffers(rb, p_render_data->environment, p_render_data->scene_data->cam_transform.origin, time, sky_luminance_multiplier, sky_brightness_multiplier);
				radiance_texture = sky.sky_get_radiance_texture_rd(sky_rid);
			} else {
				// do not try to draw sky if invalid
				draw_sky = false;
			}

			if (draw_sky || draw_sky_fog_only) {
				// update sky half/quarter res buffers (if required)
				sky.update_res_buffers(rb, p_render_data->environment, time, sky_luminance_multiplier, sky_brightness_multiplier);
			}

			RD::get_singleton()->draw_command_end_label();
		}

		if (bg_mode != RSE::ENV_BG_CLEAR_COLOR && bg_mode != RSE::ENV_BG_COLOR) {
			clear_color = clear_color.srgb_to_linear();
		}
	} else {
		clear_color = p_default_bg_color.srgb_to_linear();
	}

	// After this point clear_color has linear encoding.

	RSE::ViewportMSAA msaa = rb->get_msaa_3d();
	bool use_msaa = msaa != RSE::VIEWPORT_MSAA_DISABLED;

	bool ce_pre_opaque_resolved_color = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_COLOR, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE);
	bool ce_post_opaque_resolved_color = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_COLOR, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE);
	bool ce_pre_transparent_resolved_color = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_COLOR, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT);

	bool ce_pre_opaque_resolved_depth = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE);
	bool ce_post_opaque_resolved_depth = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE);
	bool ce_pre_transparent_resolved_depth = use_msaa && _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT);

	bool debug_voxelgis = get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_VOXEL_GI_ALBEDO || get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_VOXEL_GI_LIGHTING || get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_VOXEL_GI_EMISSION;
	bool debug_sdfgi_probes = get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_SDFGI_PROBES;
	bool force_depth_pre_pass = scene_state.used_opaque_stencil || using_ssao || using_ssr || using_sscs || using_ssil;
	// GodotNext: nextgi_frame_block / nextgeo_visibility_raster (#83) / nextgeo_residency (#78)
	// GodotNext: megalights_inputs (#136) / rr_guides (#205)
	force_depth_pre_pass = force_depth_pre_pass || using_nextgi || using_nextgeo || megalights_inputs_active || rr_guides_active;
	// GodotNext: vsm_page_marking (#219) / df_shadows (#214) / rt_shadows (#141). Page marking, DF and RT shadow masks read prepass depth.
	const bool using_vsm = _vsm_marks_view(p_render_data);
	force_depth_pre_pass = force_depth_pre_pass || using_vsm || using_df_shadows || using_rt_shadows;
	// GodotNext: light_grid (#139, plan §21.3 contract M2). Activate packed light grid only when
	// MegaLights is enabled for this view or positional light count exceeds the legacy bitmask cap.
	uint32_t packed_positional_light_hint = 0u;
	if (!megalights_inputs_active && current_cluster_builder != nullptr && p_render_data->lights != nullptr && p_render_data->lights->size() > current_cluster_builder->get_max_cluster_elements()) {
		for (uint32_t i = 0; i < (uint32_t)p_render_data->lights->size(); i++) {
			RID li = (*p_render_data->lights)[i];
			if (!light_storage->owns_light_instance(li)) {
				continue;
			}
			RID base_light = light_storage->light_instance_get_base_light(li);
			if (light_storage->light_get_type(base_light) != RSE::LIGHT_DIRECTIONAL) {
				packed_positional_light_hint++;
			}
		}
	}
	const bool using_packed_light_grid = current_cluster_builder != nullptr &&
			RendererRD::LightGrid::should_use_packed_grid(megalights_inputs_active, packed_positional_light_hint, current_cluster_builder->get_max_cluster_elements());
	force_depth_pre_pass = force_depth_pre_pass || using_packed_light_grid;
	bool depth_pre_pass = (force_depth_pre_pass || scene_shader.depth_prepass_enabled) && depth_framebuffer.is_valid();
	packed_light_grid_active = using_packed_light_grid;
	packed_light_grid_has_depth_prepass = depth_pre_pass;
	vsm_mark_view = using_vsm && depth_pre_pass;
	df_shadows_view = using_df_shadows && depth_pre_pass;
	df_bind_mask = RID();
	df_bind_guide = RID();
	rt_shadows_view = using_rt_shadows && depth_pre_pass;
	rt_bind_mask = RID();
	rt_bind_lookup = RID();
	rt_bind_hit_dist = RID();
	// GodotNext: vsm_settings_debug (#221). Set again by this view's marking and non-Nanite/Nanite cull.
	vsm_debug_view_marked = false;
	vsm_debug_view_pairs = false;
	vsm_debug_view_nanite_pairs = false;
	// GodotNext: vsm_projection (#222) / vsm_nanite (#217). Sample VSM when this view runs page marking
	// and rasterizes both non-Nanite and any live NextGeo virtual-mesh casters.
	vsm_sample_view = vsm_mark_view && current_cluster_builder != nullptr && (!_vsm_nextgeo_casters_live() || nextgeo_cut.is_vsm_available());
	const bool is_nextgeo_main_view = !is_reflection_probe && rb.is_valid() && (RSG::viewport == nullptr || RSG::viewport->get_main_3d_render_target().is_null() || rb->get_render_target() == RSG::viewport->get_main_3d_render_target());

	SceneShaderForwardClustered::ShaderSpecialization base_specialization = scene_shader.default_specialization;
	base_specialization.use_depth_fog = p_render_data->environment.is_valid() && environment_get_fog_mode(p_render_data->environment) == RSE::EnvironmentFogMode::ENV_FOG_MODE_DEPTH;
	base_specialization.use_vsm = vsm_sample_view; // GodotNext: vsm_projection (#222)
	base_specialization.use_df_shadows = df_shadows_view; // GodotNext: df_shadows (#214)
	base_specialization.use_rt_shadows = rt_shadows_view; // GodotNext: rt_shadows (#141)

	if (depth_pre_pass) { //depth pre pass
		// GodotNext: megalights_decals (#136). The MegaLights input prepass runs the decal loop, but
		// the light cluster is baked in _pre_opaque_render, after this pass. Bake a decal-only
		// cluster now and bind it to the prepass; _pre_opaque_render rebuilds the full cluster.
		const bool ml_prepass = megalights_inputs_active && (depth_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS || depth_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI);
		RID ml_prepass_cluster_buffer;
		if (ml_prepass && current_cluster_builder != nullptr) {
			RENDER_TIMESTAMP("MegaLights Decal Cluster");
			current_cluster_builder->begin(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection, !p_render_data->reflection_probe.is_valid());
			RendererRD::TextureStorage::get_singleton()->update_decal_buffer(*p_render_data->decals, p_render_data->scene_data->cam_transform);
			current_cluster_builder->bake_cluster();
			ml_prepass_cluster_buffer = p_render_data->cluster_buffer;
		}

		bool needs_pre_resolve = _needs_post_prepass_render(p_render_data, using_sdfgi || using_voxelgi);
		if (needs_pre_resolve) {
			RENDER_TIMESTAMP("GI + Render Depth Pre-Pass (Parallel)");
		} else {
			RENDER_TIMESTAMP("Render Depth Pre-Pass");
		}
		if (needs_pre_resolve) {
			//pre clear the depth framebuffer, as AMD (and maybe others?) use compute for it, and barrier other compute shaders.
			RD::get_singleton()->draw_list_begin(depth_framebuffer, RD::DRAW_CLEAR_ALL, depth_pass_clear, 0.0f);
			RD::get_singleton()->draw_list_end();
			//start compute processes here, so they run at the same time as depth pre-pass
			_post_prepass_render(p_render_data, using_sdfgi || using_voxelgi);
		}

		RD::get_singleton()->draw_command_begin_label("Render Depth Pre-Pass");

		// GodotNext: megalights_decals (#136). Only the MegaLights prepass binds the cluster.
		// GodotNext: nextgeo_shading_bins (#158). The set's uniforms also make Stage A's compute bins' set 1.
		RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_OPAQUE, nullptr, is_multiview, RID(), samplers, depth_prepass_uniform_buffer_index, false, false, ml_prepass_cluster_buffer, nextgeo_shading_compute_active ? &nextgeo_stage_a_pass_uniforms : nullptr);

		bool finish_depth = using_ssao || using_ssil || using_sdfgi || using_voxelgi || ce_pre_opaque_resolved_depth || ce_post_opaque_resolved_depth;
		// GodotNext: nextgi_frame_block / nextgeo_visibility_raster (#83)
		finish_depth = finish_depth || using_nextgi || using_nextgeo;
		// GodotNext: vsm_page_marking (#219) / df_shadows (#214) / light_grid (#139) / rt_shadows (#141).
		// With MSAA, marking, DF/RT shadow masks, and packed light-grid Hi-Z cell culling read the resolved prepass depth.
		finish_depth = finish_depth || using_vsm || using_df_shadows || using_packed_light_grid || using_rt_shadows;
		RenderListParameters render_list_params(render_list[RENDER_LIST_OPAQUE].elements.ptr(), render_list[RENDER_LIST_OPAQUE].element_info.ptr(), render_list[RENDER_LIST_OPAQUE].elements.size(), reverse_cull, depth_pass_mode, 0, rb_data.is_null(), p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, 0, base_specialization, !is_reflection_probe);
		render_list_params.use_nextgeo_proxy = is_reflection_probe;
		render_list_params.prepass_motion = prepass_velocity_consumer && velocity_plan.prepass_writes && (depth_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS || depth_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI);
		// GodotNext: megalights_inputs (#136)
		render_list_params.megalights_inputs = ml_prepass;
		_render_list_with_draw_list(&render_list_params, depth_framebuffer, RD::DrawFlags(needs_pre_resolve ? RD::DRAW_DEFAULT_ALL : RD::DRAW_CLEAR_ALL), depth_pass_clear, 0.0f, 0u, p_render_data->render_region);

		RD::get_singleton()->draw_command_end_label();

		// GodotNext: nextgeo_gpu_cull (#76) / nextgeo_visibility_raster (#83).
		// Two-phase cull + draw path (c) visibility raster after the pre-pass
		// draw, before MSAA resolve and PRE_OPAQUE.
		if (using_nextgeo) {
			rb->ensure_nextgeo_targets();
			const RenderSceneDataRD *scene_data = p_render_data->scene_data;
			const Transform3D cur_view = scene_data->cam_transform.affine_inverse();
			const Transform3D prev_view = scene_data->prev_cam_transform.affine_inverse();
			const Projection view_projection = scene_data->get_view_projection(0) * cur_view;
			Projection prev_correction;
			prev_correction.set_depth_correction(scene_data->flip_y);
			const Projection prev_view_projection = (prev_correction * scene_data->prev_view_projection[0]) * prev_view;

			Projection prev_proj_jittered;
			prev_proj_jittered.set_depth_correction(scene_data->flip_y);
			prev_proj_jittered.add_jitter_offset(scene_data->prev_taa_jitter);
			prev_proj_jittered = prev_proj_jittered * scene_data->prev_cam_projection;

			NextGeoRasterInputs raster_inputs;
			raster_inputs.visibility = rb->get_nextgeo_visibility();
			raster_inputs.instance_id = rb->get_nextgeo_instance_id();
			raster_inputs.velocity = velocity_plan.prepass_writes ? rb->get_velocity_buffer(false) : RID();
			raster_inputs.depth = rb->get_depth_texture();
			raster_inputs.proj = scene_data->get_cam_projection();
			raster_inputs.view = cur_view;
			raster_inputs.prev_proj = prev_proj_jittered;
			raster_inputs.prev_view = prev_view;
			raster_inputs.taa_jitter = scene_data->taa_jitter;
			raster_inputs.prev_taa_jitter = scene_data->prev_taa_jitter;
			raster_inputs.velocity_clear = Color(velocity_plan.pre_clear_r, velocity_plan.pre_clear_g, 0.0f, 0.0f);
			raster_inputs.clear_velocity = !prepass_velocity_consumer && velocity_plan.prepass_writes;
			raster_inputs.reset_velocity = reset_temporal_history;
			raster_inputs.reverse_cull = reverse_cull;
			raster_inputs.debug_backface = bool(GLOBAL_GET("rendering/nextgeo/debug_backface")) || get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_CRACKS;
			if (!visibility_only || nextgeo_programmable_view_active) {
				nextgeo_resolve.upload_tables(
						nextgeo_bucket_table.ptr(),
						nextgeo_bucket_table.size(),
						nextgeo_instance_data.ptr(),
						nextgeo_instance_data.size() * sizeof(SceneState::InstanceData));
				raster_inputs.bucket_table = nextgeo_resolve.get_bucket_table_buffer();
				raster_inputs.bucket_table_words = nextgeo_bucket_table.size();
			}
			// GodotNext: nextgeo_sw_raster (#162). The hybrid HW/SW raster on the Nanite device tier (N1);
			// below it, or with raster_mode = hardware, the v2.4 hardware raster.
			if (RendererRD::NextGeoRasterMode(int(GLOBAL_GET("rendering/nextgeo/raster_mode"))) == RendererRD::NEXTGEO_RASTER_MODE_HYBRID && nextgeo_cut.is_hybrid_raster_available()) {
				const RendererRD::NextGeoVis64Storage vis64_storage = nextgeo_cut.get_vis64_storage(bool(GLOBAL_GET("rendering/nextgeo/debug_vis64_buffer")));
				if (vis64_storage != RendererRD::NEXTGEO_VIS64_NONE) {
					rb->ensure_nextgeo_vis64(vis64_storage == RendererRD::NEXTGEO_VIS64_BUFFER);
					raster_inputs.vis64 = rb->get_nextgeo_vis64();
					raster_inputs.vis64_storage = vis64_storage;
					raster_inputs.camera_forward = -scene_data->cam_transform.basis.get_column(2);
					raster_inputs.z_near = scene_data->z_near;
					raster_inputs.orthographic = scene_data->cam_orthogonal;
					raster_inputs.sw_raster_max_edge_px = float(GLOBAL_GET("rendering/nextgeo/sw_raster_max_edge_px"));
					// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9, GN-10). Select mesh vs VS HW path.
					const RendererRD::NextGeoHWRasterPath hw_raster_setting = RendererRD::nextgeo_hw_raster_path_from_variant(GLOBAL_GET("rendering/nextgeo/hw_raster_path"));
					const RendererRD::NextGeoHWRasterSelection hw_raster_sel = nextgeo_cut.select_hw_raster_path(hw_raster_setting, scene_data->view_count > 1);
					raster_inputs.use_mesh_hw_raster = hw_raster_sel.use_mesh;
					raster_inputs.hw_raster_reason = hw_raster_sel.reason;
					// GodotNext: nextgeo_tessellation (#163, plan §21.4 contract N11). Runtime displacement tessellation.
					raster_inputs.tessellation_enabled = bool(GLOBAL_GET("rendering/nextgeo/experimental/tessellation"));
					raster_inputs.tessellation_dicing_rate = float(GLOBAL_GET("rendering/nextgeo/tessellation/dicing_rate"));
					raster_inputs.tessellation_debug_max_patches = uint32_t(MAX(int(GLOBAL_GET("rendering/nextgeo/tessellation/debug_max_patches")), 0));
					// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N3, N4). This frame's programmable bins:
					// each bin's raster target and material parameters, and the scene environment.
					if (nextgeo_programmable_view_active && nextgeo_raster_bins_used != 0u && !nextgeo_raster_bin_table.is_empty()) {
						const bool buffer = vis64_storage == RendererRD::NEXTGEO_VIS64_BUFFER;
						for (uint32_t bin = 1; bin < RendererVirtualMeshStorage::RASTER_BIN_LIMIT; bin++) {
							const NextGeoRasterBinOwner &owner = nextgeo_raster_bin_owners[bin];
							if ((nextgeo_raster_bins_used & (uint64_t(1) << (bin - 1u))) == 0u || owner.shader == nullptr || owner.material == nullptr) {
								continue;
							}
							NextGeoRasterBinProgram program;
							program.bin = bin;
							// GodotNext: nextgeo_masked_two_sided (#153), nextgeo_vertex_offset (#164).
							program.features = owner.material->get_raster_features();
							program.vertex_offset_max = owner.material->get_effective_vertex_offset_max(float(GLOBAL_GET("rendering/nextgeo/vertex_offset_max_default")));
							program.disp_magnitude = owner.material->displacement_magnitude;
							program.disp_center = owner.material->displacement_center;
							program.sw_shader[buffer ? 1 : 0] = owner.shader->get_raster_target_shader(false, buffer);
							program.hw_shader[buffer ? 1 : 0] = owner.shader->get_raster_target_shader(true, buffer);
							if (hw_raster_sel.use_mesh) {
								program.hw_mesh_shader[buffer ? 1 : 0] = owner.shader->get_raster_target_mesh_shader(buffer);
							}
							if (raster_inputs.tessellation_enabled &&
									(program.features & RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) != 0u &&
									owner.shader->has_displacement && owner.shader->ensure_tessellation_target()) {
								program.tess_dice_shader[buffer ? 1 : 0] = owner.shader->get_tessellation_dice_shader(buffer);
								program.tess_hw_shader[buffer ? 1 : 0] = owner.shader->get_tessellation_hw_shader(buffer);
							}
							Vector<RD::Uniform> material_uniforms;
							// GodotNext: nextgeo_texture_cache_refresh (#305). Streaming swaps refresh texture_cache in
							// place; this guards against a texture freed mid-frame before update_dirty_resources().
							bool uniforms_valid = owner.material->get_parameter_uniforms(material_uniforms, owner.shader->texture_uniforms, owner.shader->ubo_size, true);
							RenderingDevice *rd = RD::get_singleton();
							for (const RD::Uniform &uniform : material_uniforms) {
								if (uniform.uniform_type == RD::UNIFORM_TYPE_TEXTURE) {
									uniforms_valid = uniforms_valid && uniform.get_id_count() > 0;
									for (uint32_t j = 0; j < uniform.get_id_count(); j++) {
										uniforms_valid = uniforms_valid && rd->texture_is_valid(uniform.get_id(j));
									}
								}
							}
							if (!uniforms_valid) {
								RendererRD::MaterialStorage::get_singleton()->material_queue_texture_update(owner.material->get_self());
								continue;
							}
							for (const RD::Uniform &uniform : material_uniforms) {
								program.material_uniforms.push_back(uniform);
							}
							raster_inputs.raster_bin_programs.push_back(program);
						}
						raster_inputs.raster_bin_table = nextgeo_raster_bin_table.ptr();
						raster_inputs.raster_bin_table_words = nextgeo_raster_bin_table.size();
						raster_inputs.environment_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, scene_data->get_uniform_buffer()));
						raster_inputs.environment_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, nextgeo_resolve.get_instance_data_buffer()));
						raster_inputs.environment_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, RendererRD::MaterialStorage::get_singleton()->global_shader_uniforms_get_storage_buffer()));
						rb->get_samplers().append_uniforms(raster_inputs.environment_uniforms, 3);
					}
				}
			}

			nextgeo_drew = nextgeo_cut.dispatch(RSG::virtual_mesh_storage, rb->get_depth_texture(), view_projection, prev_view_projection, scene_data->cam_transform.origin, rb->get_internal_size(), scene_data->camera_visible_layers, float(GLOBAL_GET("rendering/nextgeo/max_pixel_error")), 1.0f, bool(GLOBAL_GET("rendering/nextgeo/occlusion")), bool(GLOBAL_GET("rendering/nextgeo/debug_stats")), &raster_inputs, is_nextgeo_main_view);
			// GodotNext: nextgeo_tessellation (#163). Bind diced patches buffer (invalid -> dummy when tessellation is off).
			nextgeo_resolve.set_diced_patches_buffer(nextgeo_cut.get_diced_patches_buffer());
			if (nextgeo_drew && !visibility_only && nextgeo_resolve.get_bucket_count() > 0) {
				RENDER_TIMESTAMP("NextGeo Resolve Classify");
				RD::get_singleton()->draw_command_begin_label("NextGeo Resolve Classify");
				const bool nextgeo_classified = nextgeo_resolve.classify(RSG::virtual_mesh_storage, rb->get_nextgeo_visibility(), nextgeo_cut.get_clusters_buffer(), rb->get_internal_size());
				RD::get_singleton()->draw_command_end_label();
				// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7). Per-pixel shading bins for the buckets
				// whose material has a compute form; classify() still lists every bucket's tiles for the tile quads.
				if (nextgeo_classified && nextgeo_shading_compute_active && nextgeo_resolve.get_compute_bucket_count() > 0) {
					RENDER_TIMESTAMP("NextGeo Shading Bins");
					RD::get_singleton()->draw_command_begin_label("NextGeo Shading Bins");
					nextgeo_resolve.bin(RSG::virtual_mesh_storage, rb->get_nextgeo_visibility(), nextgeo_cut.get_clusters_buffer(), rb->get_internal_size(), p_render_data->render_region);
					RD::get_singleton()->draw_command_end_label();
				}
				if (nextgeo_classified) {
					RENDER_TIMESTAMP("NextGeo Stage A");
					RD::get_singleton()->draw_command_begin_label("NextGeo Stage A");
					bool stage_a_redraw = false;
					// GodotNext: nextgeo_vertex_offset (#164). When the view writes prepass velocity and has
					// VERTEX_OFFSET buckets, Stage A writes WPO motion vectors (prev TIME + prev model xform)
					// before NextGI and motion-pass upscalers while leaving non-WPO rigid velocity untouched.
					const float vertex_offset_max_default = float(GLOBAL_GET("rendering/nextgeo/vertex_offset_max_default"));
					const bool stage_a_velocity = velocity_plan.prepass_writes && rb->get_velocity_buffer(false).is_valid() && nextgeo_resolve.has_vertex_offset_buckets();
					if (stage_a_velocity) {
						scene_shader.enable_nextgeo_motion_shader_group(megalights_inputs_active);
					}
					// GodotNext: nextgeo_shading_bins (#158). Stage A's compute bins, when its targets are storable.
					NextGeoResolve::ComputeStage stage_a_compute;
					nextgeo_shading_stage_a_reason = _nextgeo_shading_stage(false, rb, rb_data, using_voxelgi, megalights_inputs_active, 0u, false, nextgeo_stage_a_pass_uniforms, stage_a_compute, stage_a_velocity);
					nextgeo_shading_stage_a_variant = nextgeo_shading_stage_a_reason == RendererRD::NEXTGEO_SHADING_REASON_NONE ? int32_t(stage_a_compute.variant) : -1;
					nextgeo_resolve.draw_stage_a(
							RSG::virtual_mesh_storage,
							rb->get_nextgeo_visibility(),
							nextgeo_cut.get_clusters_buffer(),
							// GodotNext: megalights_inputs (#136), nextgeo_vertex_offset (#164)
							rb_data->get_nextgeo_stage_a_fb(using_voxelgi, megalights_inputs_active, stage_a_velocity),
							render_base_uniform_set,
							rp_uniform_set,
							base_specialization,
							using_voxelgi,
							reverse_cull,
							p_render_data->render_region,
							// GodotNext: megalights_inputs (#136)
							megalights_inputs_active,
							stage_a_redraw,
							// GodotNext: nextgeo_shading_bins (#158)
							nextgeo_shading_stage_a_reason == RendererRD::NEXTGEO_SHADING_REASON_NONE ? &stage_a_compute : nullptr,
							stage_a_velocity,
							reset_temporal_history,
							vertex_offset_max_default);
					if (stage_a_redraw) {
						RenderingServerDefault::redraw_request();
					}
					RD::get_singleton()->draw_command_end_label();
				}
			}
		}

		if (use_msaa) {
			RENDER_TIMESTAMP("Resolve Depth Pre-Pass (MSAA)");
			RD::get_singleton()->draw_command_begin_label("Resolve Depth Pre-Pass (MSAA)");
			if (depth_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS || depth_pass_mode == PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI) {
				for (uint32_t v = 0; v < rb->get_view_count(); v++) {
					resolve_effects->resolve_gi(rb->get_depth_msaa(v), rb_data->get_normal_roughness_msaa(v), using_voxelgi ? rb_data->get_voxelgi_msaa(v) : RID(), rb->get_depth_texture(v), rb_data->get_normal_roughness(v), using_voxelgi ? rb_data->get_voxelgi(v) : RID(), rb->get_internal_size(), texture_multisamples[msaa]);
					if (prepass_velocity_consumer && velocity_plan.prepass_writes) {
						RD::get_singleton()->texture_resolve_multisample(rb->get_velocity_buffer(true, v), rb->get_velocity_buffer(false, v));
					}
				}
			} else if (finish_depth) {
				for (uint32_t v = 0; v < rb->get_view_count(); v++) {
					resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
				}
			}
			RD::get_singleton()->draw_command_end_label();
		}
	}
	velocity_plan = RendererRD::nextgeo_velocity_plan(velocity_scale, velocity_mode, using_taa, prepass_velocity_consumer, nextgeo_drew);

	{
		if (ce_pre_opaque_resolved_color) {
			// We haven't rendered color data yet so...
			WARN_PRINT_ONCE("Pre opaque rendering effects can't access resolved color buffers.");
		}

		if (ce_pre_opaque_resolved_depth && !depth_pre_pass) {
			// We haven't rendered depth data yet so...
			WARN_PRINT_ONCE("Pre opaque rendering effects can't access resolved depth buffers.");
		}

		RENDER_TIMESTAMP("Process Pre Opaque Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE, p_render_data);
	}

	RID normal_roughness_views[RendererSceneRender::MAX_RENDER_VIEWS];
	RID voxel_gi_views[RendererSceneRender::MAX_RENDER_VIEWS];

	if (rb_data.is_valid()) {
		if (rb_data->has_normal_roughness()) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				normal_roughness_views[v] = rb_data->get_normal_roughness(v);
			}
		}
		if (rb_data->has_voxelgi()) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				voxel_gi_views[v] = rb_data->get_voxelgi(v);
			}
		}
	}
	// GodotNext: nextgi_frame_block
	_pre_opaque_render(p_render_data, using_ssao, using_ssil, using_ssr, using_sscs, using_sdfgi || using_voxelgi, normal_roughness_views, voxel_gi_views, using_nextgi);
	// GodotNext: light_function_atlas (#138, plan §21.3 contract M8). Only enable atlas sampling in
	// Forward+ shaders when this view has at least one active light function.
	base_specialization.use_light_function = light_function_atlas.has_baked_atlas() && light_function_atlas.get_view_entry_count() > 0;
	if (using_nextgi) {
		_render_nextgi_front_layer(p_render_data, reverse_cull, screen_size, radiance_texture, samplers, base_specialization);
	}

	if (current_cluster_builder) {
		base_specialization.cluster_has_area_light = current_cluster_builder->get_cluster_count_by_type(ClusterBuilderRD::ELEMENT_TYPE_AREA_LIGHT) != 0 ||
				current_cluster_builder->get_packed_area_light_count() != 0;
	}

	RENDER_TIMESTAMP("Render Opaque Pass");

	RD::get_singleton()->draw_command_begin_label("Render Opaque Pass");

	p_render_data->scene_data->directional_light_count = p_render_data->directional_light_count;
	p_render_data->scene_data->opaque_prepass_threshold = 0.0f;

	// Shadow pass can change the base uniform set samplers.
	_update_render_base_uniform_set();

	uint32_t opaque_pass_uniform_buffer_index = _setup_environment(p_render_data, is_reflection_probe, screen_size, screen_size, p_default_bg_color, true, using_motion_pass);

	// GodotNext: nextgeo_shading_bins (#158). The set's uniforms also make Stage B's compute bins' set 1.
	RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_OPAQUE, p_render_data, is_multiview, radiance_texture, samplers, opaque_pass_uniform_buffer_index, true, false, RID(), nextgeo_shading_compute_active ? &nextgeo_stage_b_pass_uniforms : nullptr);

	{
		bool render_motion_pass = !render_list[RENDER_LIST_MOTION].elements.is_empty();

		{
			// GodotNext: nextgeo_visibility_raster (#83) / nextgeo_resolve (#80).
			bool stage_b_wrote = false;
			uint32_t stage_b_draws = 0;
			uint32_t opaque_color_pass_flags = using_motion_pass ? (color_pass_flags & ~uint32_t(COLOR_PASS_FLAG_MOTION_VECTORS)) : color_pass_flags;
			RID opaque_framebuffer = using_motion_pass ? rb_data->get_color_pass_fb(opaque_color_pass_flags) : color_framebuffer;

			// GodotNext: rr_guides (#205, #275). Stage B clears the two guides when it opens the
			// framebuffer, otherwise the opaque pass does; after Stage B the opaque pass loads
			// everything.
			const bool rr_has_specular = (opaque_color_pass_flags & COLOR_PASS_FLAG_SEPARATE_SPECULAR) != 0;
			const bool rr_has_velocity = (opaque_color_pass_flags & COLOR_PASS_FLAG_MOTION_VECTORS) != 0;
			bool rr_guides_cleared = false;

			auto build_opaque_clear_colors = [&](bool p_load_color) {
				Vector<Color> colors;
				if (!p_load_color) {
					Color cc = clear_color;
					if (using_separate_specular || rb_data.is_valid()) {
						// Effects that rely on separate specular, like subsurface scattering, must clear the alpha to zero.
						cc.a = 0;
					}
					colors.push_back(cc);
					if (rb_data.is_valid()) {
						colors.push_back(Color(0, 0, 0, 0)); // Separate specular.
						colors.push_back(Color(0, 0, 0, 0)); // Motion vector. Pushed to the clear color vector even if the framebuffer isn't bound.
					}
				}
				return colors;
			};

			if (nextgeo_drew && !visibility_only && nextgeo_resolve.get_bucket_count() > 0) {
				RENDER_TIMESTAMP("NextGeo Stage B");
				RD::get_singleton()->draw_command_begin_label("NextGeo Stage B");
				uint32_t stage_b_pipeline_flags = 0;
				if (opaque_color_pass_flags & COLOR_PASS_FLAG_SEPARATE_SPECULAR) {
					stage_b_pipeline_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_SEPARATE_SPECULAR;
				}
				if (opaque_color_pass_flags & COLOR_PASS_FLAG_MOTION_VECTORS) {
					stage_b_pipeline_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS;
				}
				// GodotNext: rr_guides (#205)
				if (opaque_color_pass_flags & COLOR_PASS_FLAG_RR_GUIDES) {
					stage_b_pipeline_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_RR_GUIDES;
				}
				const bool zero_vel = velocity_plan.prepass_writes && (stage_b_pipeline_flags & SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS);
				BitField<RD::DrawFlags> stage_b_draw_flags = RendererRD::nextgeo_color_pass_draw_flags(load_color, velocity_plan.color_pass_loads, using_separate_specular, depth_pre_pass);
				Vector<Color> stage_b_clear = build_opaque_clear_colors(load_color);
				if (rr_guides_active) {
					RendererRD::RRGuides::clear_guides(stage_b_draw_flags, stage_b_clear, rr_has_specular, rr_has_velocity);
				}
				bool stage_b_redraw = false;
				// GodotNext: nextgeo_shading_bins (#158). Stage B's compute bins, when its targets are storable.
				NextGeoResolve::ComputeStage stage_b_compute;
				nextgeo_shading_stage_b_reason = _nextgeo_shading_stage(true, rb, rb_data, using_voxelgi, megalights_inputs_active, opaque_color_pass_flags, zero_vel, nextgeo_stage_b_pass_uniforms, stage_b_compute);
				nextgeo_shading_stage_b_variant = nextgeo_shading_stage_b_reason == RendererRD::NEXTGEO_SHADING_REASON_NONE ? int32_t(stage_b_compute.variant) : -1;
				const float vertex_offset_max_default = float(GLOBAL_GET("rendering/nextgeo/vertex_offset_max_default"));
				const bool stage_b_opened = nextgeo_resolve.draw_stage_b(
						RSG::virtual_mesh_storage,
						rb->get_nextgeo_visibility(),
						nextgeo_cut.get_clusters_buffer(),
						opaque_framebuffer,
						stage_b_draw_flags,
						stage_b_clear,
						render_base_uniform_set,
						rp_uniform_set,
						stage_b_pipeline_flags,
						zero_vel,
						p_render_data->directional_light_soft_shadows,
						base_specialization,
						reverse_cull,
						p_render_data->render_region,
						stage_b_redraw,
						stage_b_draws,
						// GodotNext: nextgeo_shading_bins (#158)
						nextgeo_shading_stage_b_reason == RendererRD::NEXTGEO_SHADING_REASON_NONE ? &stage_b_compute : nullptr,
						reset_temporal_history,
						vertex_offset_max_default);
				if (stage_b_redraw) {
					RenderingServerDefault::redraw_request();
				}
				stage_b_wrote = stage_b_opened && stage_b_draws > 0;
				if (stage_b_opened) {
					load_color = true;
					rr_guides_cleared = true; // GodotNext: rr_guides (#275)
				}
				RD::get_singleton()->draw_command_end_label();
			}

			load_color = RendererRD::nextgeo_should_load_color(load_color, nextgeo_drew, stage_b_wrote);
			Vector<Color> c = build_opaque_clear_colors(load_color);
			RenderListParameters render_list_params(render_list[RENDER_LIST_OPAQUE].elements.ptr(), render_list[RENDER_LIST_OPAQUE].element_info.ptr(), render_list[RENDER_LIST_OPAQUE].elements.size(), reverse_cull, PASS_MODE_COLOR, opaque_color_pass_flags, rb_data.is_null(), p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, 0, base_specialization, !is_reflection_probe);
			render_list_params.use_nextgeo_proxy = is_reflection_probe;
			// Load the velocity attachment on TAA when prepass/NextGeo wrote it; never DRAW_IGNORE_ALL (§6.1).
			BitField<RD::DrawFlags> opaque_draw_flags = RendererRD::nextgeo_color_pass_draw_flags(load_color, velocity_plan.color_pass_loads, using_separate_specular, depth_pre_pass);
			if (rr_guides_active && !rr_guides_cleared) {
				RendererRD::RRGuides::clear_guides(opaque_draw_flags, c, rr_has_specular, rr_has_velocity);
			}
			if (!stage_b_wrote || !render_list[RENDER_LIST_OPAQUE].elements.is_empty()) {
				_render_list_with_draw_list(&render_list_params, opaque_framebuffer, opaque_draw_flags, c, 0.0f, 0u, p_render_data->render_region);
			}
			if (is_nextgeo_main_view) {
				NextGeoCut::set_resolve_stats(
						(!visibility_only && nextgeo_drew) ? nextgeo_resolve.get_last_published_bucket_count() : 0,
						(!visibility_only && nextgeo_drew) ? nextgeo_resolve.get_last_published_stage_a_draws() : 0,
						stage_b_draws,
						stage_b_wrote);
			}
			// GodotNext: nextgeo_shading_bins (#158). With rendering/nextgeo/debug_stats, as the cut's counters:
			// which path shaded Stage A/B, and why the tile quads stayed.
			if (is_nextgeo_main_view && bool(GLOBAL_GET("rendering/nextgeo/debug_stats"))) {
				Dictionary shading;
				const bool resolved = !visibility_only && nextgeo_drew && nextgeo_resolve.get_bucket_count() > 0;
				const uint32_t compute_buckets = resolved ? nextgeo_resolve.get_compute_bucket_count() : 0u;
				const uint32_t stage_a_compute = resolved ? nextgeo_resolve.get_last_published_stage_a_compute() : 0u;
				const uint32_t stage_b_compute = resolved ? nextgeo_resolve.get_last_published_stage_b_compute() : 0u;
				shading["shading_path"] = nextgeo_shading_compute_active ? "compute_bins" : "tile_quads";
				shading["shading_binned"] = resolved && nextgeo_resolve.is_binned();
				shading["shading_compute_buckets"] = int64_t(compute_buckets);
				shading["shading_rejected_buckets"] = int64_t(resolved ? nextgeo_resolve.get_bucket_count() - compute_buckets : 0u);
				shading["shading_stage_a_compute"] = int64_t(stage_a_compute);
				shading["shading_stage_b_compute"] = int64_t(stage_b_compute);
				shading["shading_stage_a_tile_quads"] = int64_t(resolved ? nextgeo_resolve.get_last_published_stage_a_draws() - MIN(stage_a_compute, nextgeo_resolve.get_last_published_stage_a_draws()) : 0u);
				shading["shading_stage_b_tile_quads"] = int64_t(stage_b_draws - MIN(stage_b_compute, stage_b_draws));
				shading["shading_stage_a_reason"] = RendererRD::nextgeo_shading_reason_name(nextgeo_shading_stage_a_reason);
				shading["shading_stage_b_reason"] = RendererRD::nextgeo_shading_reason_name(nextgeo_shading_stage_b_reason);
				// SceneShaderForwardClustered::ResolveComputeVariant of each stage's compute bins, -1 for none.
				shading["shading_stage_a_variant"] = int64_t(resolved ? nextgeo_shading_stage_a_variant : -1);
				shading["shading_stage_b_variant"] = int64_t(resolved ? nextgeo_shading_stage_b_variant : -1);
				NextGeoCut::set_shading_stats(shading);
			}
		}

		RD::get_singleton()->draw_command_end_label();

		if (using_motion_pass) {
			if (velocity_mode == NEXTGI_VELOCITY_DENSE) {
				motion_vectors_store->process(rb,
						p_render_data->scene_data->cam_projection, p_render_data->scene_data->cam_transform,
						p_render_data->scene_data->prev_cam_projection, p_render_data->scene_data->prev_cam_transform,
						velocity_plan.mfx_preserve, reset_temporal_history);
			} else if (!velocity_plan.skip_clear_pass) {
				Vector<Color> motion_vector_clear_colors;
				motion_vector_clear_colors.push_back(Color(-1, -1, 0, 0));
				RD::get_singleton()->draw_list_begin(rb_data->get_velocity_only_fb(), RD::DRAW_CLEAR_ALL, motion_vector_clear_colors);
				RD::get_singleton()->draw_list_end();
			}
		}

		if (render_motion_pass) {
			RD::get_singleton()->draw_command_begin_label("Render Motion Pass");

			RENDER_TIMESTAMP("Render Motion Pass");

			rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_MOTION, p_render_data, is_multiview, radiance_texture, samplers, opaque_pass_uniform_buffer_index, true);

			RenderListParameters render_list_params(render_list[RENDER_LIST_MOTION].elements.ptr(), render_list[RENDER_LIST_MOTION].element_info.ptr(), render_list[RENDER_LIST_MOTION].elements.size(), reverse_cull, PASS_MODE_COLOR, color_pass_flags, rb_data.is_null(), p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, 0, base_specialization, !is_reflection_probe);
			render_list_params.use_nextgeo_proxy = is_reflection_probe;
			_render_list_with_draw_list(&render_list_params, color_framebuffer);

			RD::get_singleton()->draw_command_end_label();
		}
	}

	if (!is_reflection_probe && !visibility_only && rb_data.is_valid() && RendererRD::nextgeo_is_debug_view(get_debug_draw_mode())) {
		_render_nextgeo_debug_scene_passes(p_render_data, using_nextgeo, nextgeo_drew, use_msaa, msaa, reverse_cull, screen_size, depth_prepass_uniform_buffer_index, samplers, base_specialization);
	}

	// GodotNext: rd_readonly_depth (#301). Scan the transparent list once to decide both whether
	// TSR level 2 can split right after the motion-vector pass (no depth_draw_always surface) and
	// whether the transparent pass can attach the depth buffer read-only (also no stencil-writing
	// surface).
	ForwardTransparentDepthScan alpha_depth_scan;
	for (uint32_t i = 0; i < render_list[RENDER_LIST_ALPHA].elements.size(); i++) {
		const GeometryInstanceSurfaceDataCache *surf = render_list[RENDER_LIST_ALPHA].elements[i];
		const bool writes_depth = surf->shader->depth_draw == SceneShaderForwardClustered::ShaderData::DEPTH_DRAW_ALWAYS;
		const bool writes_stencil = surf->shader->stencil_enabled && (surf->shader->stencil_flags & (SceneShaderForwardClustered::ShaderData::STENCIL_FLAG_WRITE | SceneShaderForwardClustered::ShaderData::STENCIL_FLAG_WRITE_DEPTH_FAIL)) != 0;
		alpha_depth_scan.add_surface(writes_depth, writes_stencil);
	}

	// GodotNext: pt_tsr_async_compute (#193) & rd_readonly_depth (#301). TSR's async schedule for
	// this frame, from rendering/anti_aliasing/tsr/async_compute (UE's r.TSR.AsyncCompute levels).
	// Without async compute (RD::is_async_compute_active()), in a multiview (XR) viewport, and in
	// every TSR viewport of an RD frame after the first one that used it
	// (nextgi_tsr_async_available()), every level records as level 0. At level 2, velocity dilation
	// and parallax disocclusion record here on the async queue, as soon as the depth and velocity
	// buffers are final, and the passes that read the scene color record at the upscaler's place
	// below. The split needs the depth and velocity to stay as they are until then: no MSAA
	// resolve, no transparent surface that writes depth, no compositor effect and no VoxelGI or
	// SDFGI debug draw in between. Otherwise level 2 records as level 0. With read-only depth
	// attachments (#301), the sky pass, SSS, specular merge, depth copy, and transparent pass (when
	// no depth_draw_always or stencil-writing surface draws) share the depth buffer's read-only
	// layout with the async passes, so the fork-join window spans them and closes at TSR's shading
	// rejection pass.
	TSRAsyncSchedule tsr_async_schedule;
	if (rb_data.is_valid() && scale_type == SCALE_TSR) {
		const int64_t tsr_async_level = GLOBAL_GET_CACHED(int64_t, "rendering/anti_aliasing/tsr/async_compute");
		const uint64_t rd_frame = RD::get_singleton()->get_frames_drawn();
		const bool tsr_async_available = nextgi_tsr_async_available(RD::get_singleton()->is_async_compute_active(), rb->get_view_count(), rd_frame, tsr_async_frame);
		bool tsr_split_allowed = false;
		if (tsr_async_available && tsr_async_level == TSR_ASYNC_DEPTH_VELOCITY) {
			tsr_split_allowed = !use_msaa && !debug_voxelgis && !debug_sdfgi_probes && alpha_depth_scan.tsr_split_depth_unchanged();
			const RSE::CompositorEffectCallbackType tsr_split_callbacks[] = {
				RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE,
				RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_SKY,
				RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT,
				RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT,
			};
			for (RSE::CompositorEffectCallbackType callback : tsr_split_callbacks) {
				tsr_split_allowed = tsr_split_allowed && !_has_compositor_effect(callback, p_render_data);
			}
		}
		tsr_async_schedule = nextgi_tsr_async_schedule(tsr_async_level, tsr_async_available, tsr_split_allowed);
		if (tsr_async_schedule.uses_async()) {
			tsr_async_frame = rd_frame;
		}
	}

	// GodotNext: pt_tsr_async_compute (#193). One view's TSR parameters, the same at both record
	// points; only the upscaler's place passes the color, the reactive mask and the output.
	auto tsr_parameters = [&](uint32_t p_view, TSRRecordPoint p_record_point) {
		Projection correction;
		correction.set_depth_correction(true, true, false);
		const Projection &prev_proj = p_render_data->scene_data->prev_cam_projection;
		const Projection &cur_proj = p_render_data->scene_data->cam_projection;
		const Transform3D &prev_transform = p_render_data->scene_data->prev_cam_transform;
		const Transform3D &cur_transform = p_render_data->scene_data->cam_transform;

		RendererRD::TSR::Parameters params;
		params.render_buffers = rb;
		params.context = rb_data->get_tsr_context(p_view);
		params.view = p_view;
		if (p_record_point == TSR_RECORD_UPSCALE) {
			params.color = rb->get_internal_texture(p_view);
			params.reactive = rb->get_internal_texture_reactive(p_view);
			params.output = rb->get_upscaled_texture(p_view);
		}
		params.depth = rb->get_depth_texture(p_view);
		params.velocity = rb->get_velocity_buffer(false, p_view);
		params.internal_size = rb->get_internal_size();
		params.output_size = rb->get_target_size();
		params.quality = rb->get_tsr_quality();
		params.jitter_pixels = p_render_data->scene_data->taa_jitter * Vector2(rb->get_internal_size()) * 0.5f;
		params.reprojection = (correction * prev_proj) * prev_transform.affine_inverse() * cur_transform * (correction * cur_proj).inverse();
		params.z_near = p_render_data->scene_data->z_near;
		params.z_far = p_render_data->scene_data->z_far;
		params.is_orthogonal = p_render_data->scene_data->cam_orthogonal;
		params.reset = reset_temporal_history;
		// GodotNext: pt_tsr_stability (#198).
		params.max_internal_size = rb->get_max_internal_size();
		params.camera_projection = correction * cur_proj;
		params.camera_transform = cur_transform;
		// Wall-clock, not time_step: the flicker clock must advance while Engine.time_scale is 0.
		params.delta_time = float(RSG::frame_step_unscaled);
		// Thin-geometry detection is opt-in and read live.
		params.thin_geometry = GLOBAL_GET_CACHED(bool, "rendering/anti_aliasing/tsr/thin_geometry_detection");
		// The pass visualizer draws while the viewport's debug draw is DEBUG_DRAW_TSR; the setting picks the pass.
		params.debug_view = get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_TSR ? int32_t(nextgi_tsr_debug_view_from_setting(GLOBAL_GET_CACHED(int64_t, "rendering/anti_aliasing/tsr/debug_view"))) : -1;
		params.async_schedule = tsr_async_schedule;
		params.record_point = p_record_point;
		return params;
	};

	if (tsr_async_schedule.split) {
		// While timestamps are captured, the render graph orders every command after the previous
		// timestamp, so the two passes run alone, up to the next timestamp. A split frame records
		// "TSR depth and velocity" here and "TSR critical path" at the upscaler's place, and no "TSR":
		// "TSR" always covers all of TSR.
		RD::get_singleton()->draw_command_begin_label("TSR Depth and Velocity");
		RENDER_TIMESTAMP("TSR depth and velocity");
		for (uint32_t v = 0; v < rb->get_view_count(); v++) {
			rb_data->ensure_tsr(tsr_effect, v);
			tsr_effect->process(tsr_parameters(v, TSR_RECORD_AFTER_MOTION_VECTORS));
		}
		RD::get_singleton()->draw_command_end_label();
	}

	// GodotNext: rr_guides (#205). The opaque passes wrote the two albedo guides; pack the
	// normal and roughness guide from the prepass buffer where they drew a surface.
	if (rr_guides_active) {
		RENDER_TIMESTAMP("DLSS-RR guides");
		RD::get_singleton()->draw_command_begin_label("DLSS-RR guides");
		if (rr_guides == nullptr) {
			rr_guides = memnew(RendererRD::RRGuides);
		}
		for (uint32_t v = 0; v < rb->get_view_count(); v++) {
			rr_guides->pack(rb_data->get_normal_roughness(v), rb->get_texture_slice(RB_SCOPE_RR_GUIDES, RB_TEX_RR_DIFFUSE_ALBEDO, v, 0), rb->get_texture_slice(RB_SCOPE_RR_GUIDES, RB_TEX_RR_NORMAL_ROUGHNESS, v, 0), rb->get_internal_size(), p_render_data->scene_data->cam_transform.basis);
		}
		RD::get_singleton()->draw_command_end_label();
	}

	{
		if (ce_post_opaque_resolved_color) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				RD::get_singleton()->texture_resolve_multisample(rb->get_color_msaa(v), rb->get_internal_texture(v));
			}
		}

		if (ce_post_opaque_resolved_depth) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
			}
		}

		RENDER_TIMESTAMP("Process Post Opaque Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE, p_render_data);
	}

	if (debug_voxelgis) {
		Projection dc;
		dc.set_depth_correction(true);
		Projection cm = (dc * p_render_data->scene_data->cam_projection) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(color_only_framebuffer);
		RD::get_singleton()->draw_command_begin_label("Debug VoxelGIs");
		for (int i = 0; i < (int)p_render_data->voxel_gi_instances->size(); i++) {
			gi.debug_voxel_gi((*p_render_data->voxel_gi_instances)[i], draw_list, color_only_framebuffer, cm, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_VOXEL_GI_LIGHTING, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_VOXEL_GI_EMISSION, 1.0);
		}
		RD::get_singleton()->draw_command_end_label();
		RD::get_singleton()->draw_list_end();
	}

	if (debug_sdfgi_probes) {
		Projection dc;
		dc.set_depth_correction(true);
		Projection cms[RendererSceneRender::MAX_RENDER_VIEWS];
		for (uint32_t v = 0; v < p_render_data->scene_data->view_count; v++) {
			cms[v] = (dc * p_render_data->scene_data->view_projection[v]) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
		}
		_debug_sdfgi_probes(rb, color_only_framebuffer, p_render_data->scene_data->view_count, cms);
	}

	if (draw_sky || draw_sky_fog_only) {
		RENDER_TIMESTAMP("Render Sky");

		RD::get_singleton()->draw_command_begin_label("Draw Sky");
		// GodotNext: rd_readonly_depth (#301). Sky only depth-tests without writes; attach depth
		// read-only so async depth readers (TSR level 2) can overlap the sky pass.
		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(color_only_framebuffer, RD::DRAW_READ_ONLY_DEPTH, Vector<Color>(), 1.0f, 0u, p_render_data->render_region);

		sky.draw_sky(draw_list, rb, p_render_data->environment, color_only_framebuffer, time, sky_luminance_multiplier, sky_brightness_multiplier);

		RD::get_singleton()->draw_list_end();
		RD::get_singleton()->draw_command_end_label();
	}

	if (use_msaa) {
		RENDER_TIMESTAMP("Resolve MSAA");

		if (scene_state.used_screen_texture || using_separate_specular || ce_pre_transparent_resolved_color) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				RD::get_singleton()->texture_resolve_multisample(rb->get_color_msaa(v), rb->get_internal_texture(v));
			}
			if (using_separate_specular) {
				for (uint32_t v = 0; v < rb->get_view_count(); v++) {
					RD::get_singleton()->texture_resolve_multisample(rb_data->get_specular_msaa(v), rb_data->get_specular(v));
				}
			}
		}

		if (scene_state.used_depth_texture || scene_state.used_normal_texture || using_separate_specular || ce_needs_normal_roughness || ce_pre_transparent_resolved_depth) {
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
			}
		}
	}

	{
		RENDER_TIMESTAMP("Process Post Sky Compositor Effects");
		// Don't need to check for depth or color resolve here, we've already triggered it.
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_SKY, p_render_data);
	}

	if (using_separate_specular) {
		if (using_sss) {
			RENDER_TIMESTAMP("Sub-Surface Scattering");
			RD::get_singleton()->draw_command_begin_label("Process Sub-Surface Scattering");
			_process_sss(rb, p_render_data->scene_data->cam_projection);
			RD::get_singleton()->draw_command_end_label();
		}

		{
			//just mix specular back
			RENDER_TIMESTAMP("Merge Specular");
			copy_effects->merge_specular(color_only_framebuffer, rb_data->get_specular(), !use_msaa ? RID() : rb->get_internal_texture(), RID(), p_render_data->scene_data->view_count);
		}
	}

	if (using_separate_specular && is_environment(p_render_data->environment) && (environment_get_background(p_render_data->environment) == RSE::ENV_BG_CANVAS)) {
		// Canvas background mode does not clear the color buffer, but copies over it. If screen-space specular effects are enabled and the background is blank,
		// this results in ghosting due to the separate specular buffer copy. Need to explicitly clear the specular buffer once we're done with it to fix it.
		RENDER_TIMESTAMP("Clear Separate Specular (Canvas Background Mode)");
		Vector<Color> blank_clear_color;
		blank_clear_color.push_back(Color(0.0, 0.0, 0.0));
		RD::get_singleton()->draw_list_begin(rb_data->get_specular_only_fb(), RD::DRAW_CLEAR_ALL, blank_clear_color);
		RD::get_singleton()->draw_list_end();
	}

	if (rb_data.is_valid() && using_upscaling) {
		// Make sure the upscaled texture is initialized, but not necessarily filled, before running screen copies
		// so it properly detect if a dedicated copy texture should be used.
		rb->ensure_upscaled();
	}

	if (scene_state.used_screen_texture || global_surface_data.screen_texture_used) {
		RENDER_TIMESTAMP("Copy Screen Texture");

		_render_buffers_ensure_screen_texture(p_render_data);

		if (scene_state.used_screen_texture) {
			// Copy screen texture to backbuffer so we can read from it
			_render_buffers_copy_screen_texture(p_render_data);
		}
	}

	if (scene_state.used_depth_texture || global_surface_data.depth_texture_used) {
		RENDER_TIMESTAMP("Copy Depth Texture");

		_render_buffers_ensure_depth_texture(p_render_data);

		if (scene_state.used_depth_texture) {
			// Copy depth texture to backbuffer so we can read from it
			_render_buffers_copy_depth_texture(p_render_data);
		}
	}

	{
		if (using_separate_specular) {
			// Our specular will be combined back in (and effects, subsurface scattering and/or ssr applied),
			// so if we've requested this, we need another copy.
			// Fairly unlikely scenario though.

			if (ce_pre_transparent_resolved_color) {
				for (uint32_t v = 0; v < rb->get_view_count(); v++) {
					RD::get_singleton()->texture_resolve_multisample(rb->get_color_msaa(v), rb->get_internal_texture(v));
				}
			}

			if (ce_pre_transparent_resolved_depth) {
				for (uint32_t v = 0; v < rb->get_view_count(); v++) {
					resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
				}
			}
		}

		RENDER_TIMESTAMP("Process Pre Transparent Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT, p_render_data);
	}

	RENDER_TIMESTAMP("Render 3D Transparent Pass");

	RD::get_singleton()->draw_command_begin_label("Render 3D Transparent Pass");

	p_render_data->scene_data->nextgi_front_layer = using_nextgi && front_layer.get_reflection().is_valid() && p_render_data->environment.is_valid() && environment_get_nextgi_translucency_front_layer_reflections(p_render_data->environment) && bool(GLOBAL_GET("rendering/global_illumination/nextgi/translucency_reflections_front_layer_allow"));
	uint32_t transparent_pass_uniform_buffer_index = _setup_environment(p_render_data, is_reflection_probe, screen_size, screen_size, p_default_bg_color, false);

	rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_ALPHA, p_render_data, is_multiview, radiance_texture, samplers, transparent_pass_uniform_buffer_index, true);

	{
		uint32_t transparent_color_pass_flags = (color_pass_flags | uint32_t(COLOR_PASS_FLAG_TRANSPARENT)) & ~uint32_t(COLOR_PASS_FLAG_SEPARATE_SPECULAR);
		// Motion vectors should not be overwritten by transparent objects.
		transparent_color_pass_flags &= ~uint32_t(COLOR_PASS_FLAG_MOTION_VECTORS);
		// GodotNext: rr_guides (#205). Transparent surfaces write no guides.
		transparent_color_pass_flags &= ~uint32_t(COLOR_PASS_FLAG_RR_GUIDES);

		RID alpha_framebuffer = rb_data.is_valid() ? rb_data->get_color_pass_fb(transparent_color_pass_flags) : color_only_framebuffer;
		// GodotNext: vsm_fog_translucency (#215). Forward transparents take one coarse-page tap when
		// rendering/lights_and_shadows/virtual_shadows/translucent_quality == 0.
		SceneShaderForwardClustered::ShaderSpecialization transparent_specialization = base_specialization;
		transparent_specialization.use_vsm_coarse = vsm_sample_view && vsm_translucent_coarse;
		// GodotNext: light_function_atlas (#138). Forward transparents ignore the Light Function Atlas
		// by default unless rendering/lights_and_shadows/light_function_atlas/translucent is enabled.
		transparent_specialization.use_light_function = base_specialization.use_light_function && RendererRD::LightFunctionAtlas::read_translucent_enabled();
		// GodotNext: avboit (#295, plan §21.2.1) & rd_readonly_depth (#301). With AVBOIT the alpha
		// list is drawn in any order. Keep the depth attachment read-only when no transparent
		// surface writes depth or stencil so TSR level 2 overlaps the transparent pass.
		if (!(_avboit_view_active(p_render_data, use_msaa, is_reflection_probe) && _render_avboit(p_render_data, alpha_framebuffer, transparent_color_pass_flags, transparent_pass_uniform_buffer_index, screen_size, p_default_bg_color, reverse_cull, radiance_texture, samplers, transparent_specialization, alpha_depth_scan))) {
			RenderListParameters render_list_params(render_list[RENDER_LIST_ALPHA].elements.ptr(), render_list[RENDER_LIST_ALPHA].element_info.ptr(), render_list[RENDER_LIST_ALPHA].elements.size(), reverse_cull, PASS_MODE_COLOR, transparent_color_pass_flags, rb_data.is_null(), p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, 0, transparent_specialization, !is_reflection_probe);
			render_list_params.use_nextgeo_proxy = is_reflection_probe;
			const BitField<RD::DrawFlags> transparent_draw_flags = alpha_depth_scan.transparent_pass_read_only_depth() ? BitField<RD::DrawFlags>(RD::DRAW_READ_ONLY_DEPTH) : BitField<RD::DrawFlags>(RD::DRAW_DEFAULT_ALL);
			_render_list_with_draw_list(&render_list_params, alpha_framebuffer, transparent_draw_flags, Vector<Color>(), 0.0f, 0u, p_render_data->render_region);
		}
	}
	p_render_data->scene_data->nextgi_front_layer = false;

	RD::get_singleton()->draw_command_end_label();

	if (rb_data.is_valid() && use_msaa) {
		RENDER_TIMESTAMP("> Resolve MSAA (Post-Transparent)");
		RD::get_singleton()->draw_command_begin_label("Resolve MSAA (Post-Transparent)");

		bool compositor_needs_resolved_depth = _compositor_effects_has_flag(p_render_data, RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH, RSE::CompositorEffectCallbackType::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
		bool using_dof = RSG::camera_attributes->camera_attributes_uses_dof(p_render_data->camera_attributes);
		bool resolve_transparent_depth = (using_taa || using_upscaling || using_dof || compositor_needs_resolved_depth);
		bool resolve_velocity_buffer = (using_taa || using_upscaling || ce_needs_motion_vectors) && rb->has_velocity_buffer(true);

		RENDER_TIMESTAMP("Resolve Color");
		for (uint32_t v = 0; v < rb->get_view_count(); v++) {
			RD::get_singleton()->texture_resolve_multisample(rb->get_color_msaa(v), rb->get_internal_texture(v));
		}
		if (resolve_transparent_depth) {
			RENDER_TIMESTAMP("Resolve Depth");
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				resolve_effects->resolve_depth(rb->get_depth_msaa(v), rb->get_depth_texture(v), rb->get_internal_size(), texture_multisamples[msaa]);
			}
		}
		if (resolve_velocity_buffer) {
			RENDER_TIMESTAMP("Resolve Velocity Buffer");
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				RD::get_singleton()->texture_resolve_multisample(rb->get_velocity_buffer(true, v), rb->get_velocity_buffer(false, v));
			}
		}

		RENDER_TIMESTAMP("< Resolve MSAA (Post-Transparent)");
		RD::get_singleton()->draw_command_end_label();
	}

	RD::get_singleton()->draw_command_begin_label("Copy Framebuffer for SSIL/SSR");
	// GodotNext: nextgi_frame_block
	if (using_ssil || using_ssr || using_nextgi) {
		RENDER_TIMESTAMP("Copy Final Framebuffer (SSIL/SSR)");
		_copy_framebuffer_to_ss_effects(rb, using_ssil, using_ssr);
	}
	RD::get_singleton()->draw_command_end_label();

	{
		RENDER_TIMESTAMP("Process Post Transparent Compositor Effects");
		_process_compositor_effects(RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT, p_render_data);
	}

	if (rb_data.is_valid() && (using_upscaling || using_taa)) {
		if (scale_type == SCALE_FSR2) {
			rb_data->ensure_fsr2(fsr2_effect);

			RID exposure;
			if (RSG::camera_attributes->camera_attributes_uses_auto_exposure(p_render_data->camera_attributes)) {
				exposure = luminance->get_current_luminance_buffer(rb);
			}

			RD::get_singleton()->draw_command_begin_label("FSR2");
			RENDER_TIMESTAMP("FSR2");

			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				real_t fov = p_render_data->scene_data->cam_projection.get_fov();
				real_t aspect = p_render_data->scene_data->cam_projection.get_aspect();
				real_t fovy = p_render_data->scene_data->cam_projection.get_fovy(fov, 1.0 / aspect);
				Vector2 jitter = p_render_data->scene_data->taa_jitter * Vector2(rb->get_internal_size()) * 0.5f;
				RendererRD::FSR2Effect::Parameters params;
				params.context = rb_data->get_fsr2_context();
				params.internal_size = rb->get_internal_size();
				params.sharpness = CLAMP(1.0f - (rb->get_fsr_sharpness() / 2.0f), 0.0f, 1.0f);
				params.color = rb->get_internal_texture(v);
				params.depth = rb->get_depth_texture(v);
				params.velocity = rb->get_velocity_buffer(false, v);
				params.reactive = rb->get_internal_texture_reactive(v);
				params.exposure = exposure;
				params.output = rb->get_upscaled_texture(v);
				params.z_near = p_render_data->scene_data->z_near;
				params.z_far = p_render_data->scene_data->z_far;
				params.fovy = fovy;
				params.jitter = jitter;
				params.delta_time = float(time_step);
				params.reset_accumulation = reset_temporal_history;

				Projection correction;
				correction.set_depth_correction(true, true, false);

				const Projection &prev_proj = p_render_data->scene_data->prev_cam_projection;
				const Projection &cur_proj = p_render_data->scene_data->cam_projection;
				const Transform3D &prev_transform = p_render_data->scene_data->prev_cam_transform;
				const Transform3D &cur_transform = p_render_data->scene_data->cam_transform;
				params.reprojection = (correction * prev_proj) * prev_transform.affine_inverse() * cur_transform * (correction * cur_proj).inverse();

				fsr2_effect->upscale(params);
			}

			RD::get_singleton()->draw_command_end_label();
		} else if (scale_type == SCALE_MFX) {
#ifdef METAL_MFXTEMPORAL_ENABLED
			bool reset = rb_data->ensure_mfx_temporal(mfx_temporal_effect);

			RID exposure;
			if (RSG::camera_attributes->camera_attributes_uses_auto_exposure(p_render_data->camera_attributes)) {
				exposure = luminance->get_current_luminance_buffer(rb);
			}

			RD::get_singleton()->draw_command_begin_label("MetalFX Temporal");
			// Scale to ±0.5.
			Vector2 jitter = p_render_data->scene_data->taa_jitter * 0.5f;
			jitter *= Vector2(1.0, -1.0); // Flip y-axis as bottom left is origin.

			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				RendererRD::MFXTemporalEffect::Params params;
				params.src = rb->get_internal_texture(v);
				params.depth = rb->get_depth_texture(v);
				params.motion = rb->get_velocity_buffer(false, v);
				params.exposure = exposure;
				params.dst = rb->get_upscaled_texture(v);
				params.jitter_offset = jitter;
				params.content_size = rb->get_internal_size();
				params.reset = reset || reset_temporal_history;

				mfx_temporal_effect->process(rb_data->get_mfx_temporal_context(), params);
			}

			RD::get_singleton()->draw_command_end_label();
#endif
		} else if (scale_type == SCALE_EXTERNAL) {
			// GodotNext: pt_temporal_upscaler_extension (#192). Read the label with
			// its span size rather than up to a NUL.
			const Span<char> upscaler_label = external_upscaler->get_label();
			const String label = String::utf8(upscaler_label.ptr(), upscaler_label.size());
			RD::get_singleton()->draw_command_begin_label(upscaler_label);
			RENDER_TIMESTAMP(label);
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				const RSE::ViewportScaling3DReason reason = external_upscaler->ensure_context(rb.ptr(), v);
				if (reason != RSE::VIEWPORT_SCALING_3D_REASON_NONE) {
					external_upscaler->latch_context_error(rb.ptr(), reason);
					break;
				}
				const real_t fov = p_render_data->scene_data->cam_projection.get_fov();
				const real_t aspect = p_render_data->scene_data->cam_projection.get_aspect();
				TemporalUpscaler::Inputs inputs;
				inputs.color = rb->get_internal_texture(v);
				inputs.depth = rb->get_depth_texture(v);
				inputs.velocity = rb->get_velocity_buffer(false, v);
				// GodotNext: pt_temporal_upscaler_extension (#192). Reactive mask and
				// sizes for TemporalUpscalerInputs.
				inputs.reactive = rb->get_internal_texture_reactive(v);
				inputs.output = rb->get_upscaled_texture(v);
				inputs.internal_size = rb->get_internal_size();
				inputs.max_internal_size = rb->get_max_internal_size();
				inputs.output_size = rb->get_target_size();
				inputs.motion_vector_scale = rb->get_internal_size();
				inputs.jitter = p_render_data->scene_data->taa_jitter * Vector2(rb->get_internal_size()) * 0.5f;
				inputs.world_to_view = p_render_data->scene_data->cam_transform.affine_inverse();
				inputs.view_to_clip = p_render_data->scene_data->cam_projection;
				inputs.z_near = p_render_data->scene_data->z_near;
				inputs.z_far = p_render_data->scene_data->z_far;
				inputs.fovy = p_render_data->scene_data->cam_projection.get_fovy(fov, 1.0 / aspect);
				inputs.delta_time = float(time_step);
				inputs.reset = reset_temporal_history;
				external_upscaler->process(rb, v, inputs);
			}
			RD::get_singleton()->draw_command_end_label();
		} else if (scale_type == SCALE_TSR) {
			RD::get_singleton()->draw_command_begin_label("TSR");
			// GodotNext: pt_tsr_async_compute (#193). After the async depth and velocity passes,
			// this is TSR's main-queue part, from the first pass that waits for them to the resolve
			// ("TSR critical path", beside "TSR depth and velocity"). Otherwise "TSR" starts here as
			// before.
			if (tsr_async_schedule.split) {
				RENDER_TIMESTAMP("TSR critical path");
			} else {
				RENDER_TIMESTAMP("TSR");
			}

			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				rb_data->ensure_tsr(tsr_effect, v);
				tsr_effect->process(tsr_parameters(v, TSR_RECORD_UPSCALE));
			}

			RD::get_singleton()->draw_command_end_label();
		} else if (using_taa) {
			RD::get_singleton()->draw_command_begin_label("TAA");
			RENDER_TIMESTAMP("TAA");
			taa->process(rb, rb->get_base_data_format(), p_render_data->scene_data->z_near, p_render_data->scene_data->z_far);
			RD::get_singleton()->draw_command_end_label();
		}
	}

	if (rb_data.is_valid()) {
		_debug_draw_cluster(rb);

		RENDER_TIMESTAMP("Tonemap");

		// GodotNext: path_tracer (#167). The view's path-traced mean replaces the HDR color before tonemapping.
		next_gi.composite_path_tracer(rb.ptr(), rb->get_internal_texture(), using_nextgi);

		// GodotNext: nextgi_reference
		// The path-traced mean replaces the HDR color before tonemapping.
		next_gi.composite_reference(rb->get_internal_texture());

		_render_buffers_post_process_and_tonemap(p_render_data);
	}

	if (rb_data.is_valid()) {
		_render_buffers_debug_draw(p_render_data);

		if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_SDFGI && rb->has_custom_data(RB_SCOPE_SDFGI)) {
			Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
			Vector<RID> view_rids;

			// SDFGI renders at internal resolution, need to check if our debug correctly supports outputting upscaled.
			Size2i size = rb->get_internal_size();
			RID source_texture = rb->get_internal_texture();
			for (uint32_t v = 0; v < rb->get_view_count(); v++) {
				view_rids.push_back(rb->get_internal_texture(v));
			}

			sdfgi->debug_draw(p_render_data->scene_data->view_count, p_render_data->scene_data->view_projection, p_render_data->scene_data->cam_transform, size.x, size.y, rb->get_render_target(), source_texture, view_rids);
		}
	}

	nextgeo_virtual_active = false;
	nextgeo_visibility_only_active = false;
	nextgeo_programmable_view_active = false;
	nextgeo_shadow_pass_active = false;
	// GodotNext: vsm_nanite (#217). Release per-view shader/material pointers at end of view.
	nextgeo_raster_bins_used = 0;
	for (uint32_t bin = 0; bin < RendererVirtualMeshStorage::RASTER_BIN_LIMIT; bin++) {
		nextgeo_raster_bin_owners[bin] = {};
	}
	// GodotNext: megalights_inputs (#136)
	megalights_inputs_active = false;
}

void RenderForwardClustered::_render_nextgeo_debug_scene_passes(RenderDataRD *p_render_data, bool p_using_nextgeo, bool p_nextgeo_drew, bool p_use_msaa, RSE::ViewportMSAA p_msaa, bool p_reverse_cull, const Size2i &p_screen_size, uint32_t p_depth_ubo_index, const RendererRD::MaterialStorage::Samplers &p_samplers, SceneShaderForwardClustered::ShaderSpecialization p_base_specialization) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	if (rb.is_null() || p_render_data->scene_data == nullptr) {
		return;
	}

	const RSE::ViewportDebugDraw mode = get_debug_draw_mode();
	const RenderSceneDataRD *scene_data = p_render_data->scene_data;
	const Transform3D cur_view = scene_data->cam_transform.affine_inverse();

	RendererRD::NextGeoDebugSceneParams scene_params;
	scene_params.proj = scene_data->get_cam_projection();
	scene_params.view = cur_view;
	scene_params.view_proj = scene_data->get_view_projection(0) * cur_view;
	scene_params.cam_pos = scene_data->cam_transform.origin;
	scene_params.internal_size = p_screen_size;
	scene_params.max_pixel_error = float(GLOBAL_GET("rendering/nextgeo/max_pixel_error"));
	scene_params.lod_bias = 1.0f;
	scene_params.cull_sign = p_reverse_cull ? -1.0f : 1.0f;
	scene_params.using_nextgeo = p_nextgeo_drew;
	scene_params.visible_clusters_buf = nextgeo_cut.get_clusters_buffer();
	scene_params.diced_patches_buf = nextgeo_cut.get_diced_patches_buffer();
	scene_params.visible_capacity = NextGeoCut::CLUSTER_CAPACITY;
	scene_params.bucket_table_buf = nextgeo_resolve.get_bucket_table_buffer();
	scene_params.bucket_table_words = nextgeo_bucket_table.size();
	nextgeo_debug.record_scene_params(scene_params);

	if (mode == RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_RT_PROXY) {
		if (p_use_msaa) {
			resolve_effects->resolve_depth(rb->get_depth_msaa(0), rb->get_depth_texture(0), rb->get_internal_size(), 1 << int(p_msaa));
		}
		RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
		for (uint32_t i = 0; i < p_render_data->instances->size(); i++) {
			GeometryInstanceForwardClustered *inst = static_cast<GeometryInstanceForwardClustered *>((*p_render_data->instances)[i]);
			if (inst->data->base_type != RSE::INSTANCE_MESH || inst->data->skeleton.is_valid() || inst->mesh_instance.is_valid()) {
				continue;
			}
			const RID mesh = inst->data->base;
			for (GeometryInstanceSurfaceDataCache *s = inst->surface_caches; s != nullptr; s = s->next) {
				if (s->mesh_has_virtual_geometry) {
					s->surface_rt_proxy = mesh_storage->mesh_get_virtual_rt_proxy_surface(mesh, s->surface_index);
				}
			}
		}

		NextGeoBoolScope rt_proxy_scope(nextgeo_rt_proxy_active, true);
		_fill_render_list(RENDER_LIST_SECONDARY, p_render_data, PASS_MODE_DEPTH);
		render_list[RENDER_LIST_SECONDARY].sort_by_key();
		_fill_instance_data(RENDER_LIST_SECONDARY);

		const RID rp_uset = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, false, RID(), p_samplers, p_depth_ubo_index, false);
		const RID rt_proxy_fb = nextgeo_debug.prepare_rt_proxy_depth_fb(p_screen_size);
		if (rt_proxy_fb.is_valid()) {
			RenderListParameters params(render_list[RENDER_LIST_SECONDARY].elements.ptr(), render_list[RENDER_LIST_SECONDARY].element_info.ptr(), render_list[RENDER_LIST_SECONDARY].elements.size(), p_reverse_cull, PASS_MODE_DEPTH, 0, true, false, rp_uset, false, Vector2(), scene_data->lod_distance_multiplier, scene_data->screen_mesh_lod_threshold, 1, 0, p_base_specialization, false);
			params.use_nextgeo_rt_proxy = true;
			_render_list_with_draw_list(&params, rt_proxy_fb, RD::DRAW_CLEAR_DEPTH, Vector<Color>(), 0.0f);
		}
		return;
	}

	const RID rp_uset = _setup_render_pass_uniform_set(RENDER_LIST_OPAQUE, nullptr, false, RID(), p_samplers, p_depth_ubo_index, false);
	RID depth_attach = rb->get_depth_texture(0);
	if (p_use_msaa) {
		const RID msaa_1x_depth_fb = nextgeo_debug.prepare_rt_proxy_depth_fb(p_screen_size);
		if (msaa_1x_depth_fb.is_valid()) {
			RenderListParameters depth_params(render_list[RENDER_LIST_OPAQUE].elements.ptr(), render_list[RENDER_LIST_OPAQUE].element_info.ptr(), render_list[RENDER_LIST_OPAQUE].elements.size(), p_reverse_cull, PASS_MODE_DEPTH, 0, true, false, rp_uset, false, Vector2(), scene_data->lod_distance_multiplier, scene_data->screen_mesh_lod_threshold, 1, 0, p_base_specialization, false);
			_render_list_with_draw_list(&depth_params, msaa_1x_depth_fb, RD::DRAW_CLEAR_DEPTH, Vector<Color>(), 0.0f);
			depth_attach = nextgeo_debug.get_rt_proxy_depth();
		}
	}

	const RID fallback_fb = nextgeo_debug.prepare_fallback_fb(p_screen_size, depth_attach);
	if (!fallback_fb.is_valid()) {
		return;
	}
	nextgeo_debug.clear_fallback_texture();

	const uint32_t elem_count = render_list[RENDER_LIST_OPAQUE].elements.size();
	if (elem_count > 0) {
		LocalVector<uint32_t> codes;
		codes.resize(elem_count);
		if (mode == RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_CRACKS) {
			for (uint32_t k = 0; k < elem_count; k++) {
				const GeometryInstanceSurfaceDataCache *surf = render_list[RENDER_LIST_OPAQUE].elements[k];
				codes[k] = ((surf->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA) == 0) ? 2u : 0u;
			}
			RenderListParameters pass1_params(render_list[RENDER_LIST_OPAQUE].elements.ptr(), render_list[RENDER_LIST_OPAQUE].element_info.ptr(), elem_count, p_reverse_cull, PASS_MODE_DEPTH_MATERIAL, 0, true, false, rp_uset, false, Vector2(), scene_data->lod_distance_multiplier, scene_data->screen_mesh_lod_threshold, 1, 0, p_base_specialization, false);
			pass1_params.nextgeo_fallback_mark = true;
			pass1_params.nextgeo_fallback_codes = codes.ptr();
			_render_list_with_draw_list(&pass1_params, fallback_fb, RD::DRAW_DEFAULT_ALL);

			for (uint32_t k = 0; k < elem_count; k++) {
				const GeometryInstanceSurfaceDataCache *surf = render_list[RENDER_LIST_OPAQUE].elements[k];
				const bool closed_opaque = ((surf->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA) == 0) && surf->shader != nullptr && surf->shader->cull_mode != RSE::CULL_MODE_DISABLED;
				codes[k] = closed_opaque ? 1u : 0u;
			}
			RenderListParameters pass2_params(render_list[RENDER_LIST_OPAQUE].elements.ptr(), render_list[RENDER_LIST_OPAQUE].element_info.ptr(), elem_count, p_reverse_cull, PASS_MODE_DEPTH_MATERIAL, 0, true, false, rp_uset, false, Vector2(), scene_data->lod_distance_multiplier, scene_data->screen_mesh_lod_threshold, 1, 0, p_base_specialization, false);
			pass2_params.nextgeo_fallback_mark = true;
			pass2_params.nextgeo_cracks_backface = true;
			pass2_params.nextgeo_fallback_codes = codes.ptr();
			_render_list_with_draw_list(&pass2_params, fallback_fb, RD::DRAW_DEFAULT_ALL);
		} else {
			const uint32_t view_fb = uint32_t(RendererRD::nextgeo_view_fallback(
					rb->get_texture_samples() != RD::TEXTURE_SAMPLES_1,
					scene_data->view_count > 1,
					RendererRD::nextgeo_is_unsupported_debug_draw(mode)));
			for (uint32_t k = 0; k < elem_count; k++) {
				const GeometryInstanceSurfaceDataCache *surf = render_list[RENDER_LIST_OPAQUE].elements[k];
				const RSE::InstanceType base_type = surf->owner->data->base_type;
				const bool vgd_candidate = surf->mesh_has_virtual_geometry || surf->nextgeo_fallback_reason == RendererRD::NEXTGEO_FALLBACK_MUTATED_MESH;
				const uint32_t cls = vgd_candidate ? 1u : 2u;
				uint32_t reason = uint32_t(surf->nextgeo_fallback_reason);
				if (base_type == RSE::INSTANCE_MULTIMESH || base_type == RSE::INSTANCE_PARTICLES) {
					reason = 14u;
				} else if (!p_using_nextgeo && reason == 0u) {
					reason = view_fb;
				}
				const bool rt_excl = base_type != RSE::INSTANCE_MESH ||
						surf->shader == nullptr ||
						surf->shader->get_nextgi_rt_class() != RendererRD::MaterialStorage::ShaderData::NEXTGI_RT_OPAQUE ||
						(surf->owner->nextgeo_record_index != UINT32_MAX && RSG::virtual_mesh_storage != nullptr &&
								surf->owner->nextgeo_record_index < RSG::virtual_mesh_storage->get_record_count() &&
								(RSG::virtual_mesh_storage->get_record(surf->owner->nextgeo_record_index).flags & RendererVirtualMeshStorage::RECORD_RT_FAR) != 0u);
				codes[k] = (cls & 3u) | ((reason & 0xFu) << 2u) | ((rt_excl ? 1u : 0u) << 6u);
			}
			RenderListParameters mark_params(render_list[RENDER_LIST_OPAQUE].elements.ptr(), render_list[RENDER_LIST_OPAQUE].element_info.ptr(), elem_count, p_reverse_cull, PASS_MODE_DEPTH_MATERIAL, 0, true, false, rp_uset, false, Vector2(), scene_data->lod_distance_multiplier, scene_data->screen_mesh_lod_threshold, 1, 0, p_base_specialization, false);
			mark_params.nextgeo_fallback_mark = true;
			mark_params.nextgeo_fallback_codes = codes.ptr();
			_render_list_with_draw_list(&mark_params, fallback_fb, RD::DRAW_DEFAULT_ALL);
		}
	}

	if (mode == RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_FALLBACK) {
		nextgeo_debug.dispatch_q14_reduce(p_nextgeo_drew ? rb->get_nextgeo_visibility() : RID());
	}
}

void RenderForwardClustered::_render_buffers_debug_draw(const RenderDataRD *p_render_data) {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	Ref<RenderBufferDataForwardClustered> rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	ERR_FAIL_COND(rb_data.is_null());

	RendererSceneRenderRD::_render_buffers_debug_draw(p_render_data);

	RID render_target = rb->get_render_target();

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_SSAO && rb->has_texture(RB_SCOPE_SSAO, RB_FINAL)) {
		RID final = rb->get_texture_slice(RB_SCOPE_SSAO, RB_FINAL, 0, 0);
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		copy_effects->copy_to_fb_rect(final, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize), false, true);
	}

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_SSIL && rb->has_texture(RB_SCOPE_SSIL, RB_FINAL)) {
		RID final = rb->get_texture_slice(RB_SCOPE_SSIL, RB_FINAL, 0, 0);
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		copy_effects->copy_to_fb_rect(final, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize), false, false);
	}

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_GI_BUFFER && rb->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)) {
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		RID ambient_texture = rb->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT);
		RID reflection_texture = rb->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION);
		copy_effects->copy_to_fb_rect(ambient_texture, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize), false, false, false, true, reflection_texture, rb->get_view_count() > 1);
	}

	// GodotNext: pt_tsr_stability (#198). The TSR pass visualizer draws into RB_SCOPE_TSR_DEBUG while TSR
	// is the active scaling mode; otherwise the texture does not exist and the view shows the normal image.
	// Nearest copy, like the MegaLights views, so the legend's colors are not filtered.
	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_TSR && rb->has_texture(RB_SCOPE_TSR_DEBUG, SNAME("debug_view"))) {
		RID tsr_debug = rb->get_texture_slice(RB_SCOPE_TSR_DEBUG, SNAME("debug_view"), 0, 0);
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		copy_effects->copy_to_fb_rect(tsr_debug, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize), false, false, false, false, RID(), false, false, false, false, Rect2(), 1.0f, false);
	}

	// GodotNext: vsm_settings_debug (#221). Without virtual shadow maps the view shows the normal image.
	_vsm_debug_draw(p_render_data);

	// GodotNext: nextgi_user_surface
	// Denoise views (#53) copy their producer texture. A view with no merged producer clears the
	// framebuffer to its labeled tint.
	const RendererRD::NextGI::DebugViewId nextgi_view = RendererRD::NextGI::debug_view_from_viewport(get_debug_draw_mode());
	if (nextgi_view != RendererRD::NextGI::DEBUG_VIEW_MAX) {
		RID nextgi_source;
		if (nextgi_view == RendererRD::NextGI::DEBUG_VIEW_DENOISE_DENOISED) {
			if (rb->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)) {
				nextgi_source = rb->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT);
			}
		} else if (nextgi_view == RendererRD::NextGI::DEBUG_VIEW_REFLECTION) {
			if (rb->has_texture(RB_SCOPE_GI, RB_TEX_REFLECTION)) {
				nextgi_source = rb->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION);
			}
		} else if (nextgi_view == RendererRD::NextGI::DEBUG_VIEW_SPECULAR_HIT) {
			nextgi_source = next_gi.get_specular_debug_texture();
		} else if (nextgi_view >= RendererRD::NextGI::DEBUG_VIEW_SPECULAR_RAW && nextgi_view <= RendererRD::NextGI::DEBUG_VIEW_SPECULAR_DENOISED) {
			nextgi_source = next_gi.get_specular_filter_debug_texture(nextgi_view);
		} else if (nextgi_view == RendererRD::NextGI::DEBUG_VIEW_LITE_PROBES) {
			nextgi_source = next_gi.get_lite_debug_texture(rb.ptr());
		} else {
			nextgi_source = next_gi.get_denoise_debug_texture(nextgi_view);
		}
		if (nextgi_source.is_valid()) {
			const Size2i rtsize = texture_storage->render_target_get_size(render_target);
			copy_effects->copy_to_fb_rect(nextgi_source, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize), false, false);
		} else {
			const RendererRD::NextGI::DebugView view = RendererRD::NextGI::resolve_debug_view(nextgi_view, next_gi.get_tier_result());
			Vector<Color> clear_colors;
			clear_colors.push_back(view.tint);
			RD::get_singleton()->draw_list_begin(texture_storage->render_target_get_rd_framebuffer(render_target), RD::DRAW_CLEAR_COLOR_0, clear_colors);
			RD::get_singleton()->draw_list_end();
		}
	}

	// GodotNext: nextgeo_exclusive_dispatch (#79).
	if (RendererRD::nextgeo_is_debug_view(get_debug_draw_mode()) && p_render_data->scene_data != nullptr) {
		const Size2i rtsize = texture_storage->render_target_get_size(render_target);
		const RID dest_fb = texture_storage->render_target_get_rd_framebuffer(render_target);
		if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_RT_PROXY) {
			const RID raster_depth = rb->get_depth_texture(0);
			const RID proxy_depth = nextgeo_debug.get_rt_proxy_depth();
			if (debug_effects != nullptr && raster_depth.is_valid() && proxy_depth.is_valid() && dest_fb.is_valid()) {
				debug_effects->draw_nextgeo_rt_proxy(raster_depth, proxy_depth, dest_fb, p_render_data->scene_data->get_cam_projection(), rb->get_internal_size(), rtsize);
			}
		} else {
			const RID depth_tex = rb->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED ? nextgeo_debug.get_rt_proxy_depth() : rb->get_depth_texture(0);
			nextgeo_debug.draw_debug_view(get_debug_draw_mode(), rb->get_nextgeo_visibility(), depth_tex, dest_fb, rb->get_internal_size(), rtsize);
		}
	}
}

void RenderForwardClustered::_render_shadow_pass(RID p_light, RID p_shadow_atlas, int p_pass, const PagedArray<RenderGeometryInstance *> &p_instances, float p_lod_distance_multiplier, float p_screen_mesh_lod_threshold, bool p_open_pass, bool p_close_pass, bool p_clear_region, RenderingServerTypes::RenderInfo *p_render_info, const Size2i &p_viewport_size, const Transform3D &p_main_cam_transform, int32_t p_nextgeo_shadow_view) {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	ERR_FAIL_COND(!light_storage->owns_light_instance(p_light));

	RID base = light_storage->light_instance_get_base_light(p_light);

	Rect2i atlas_rect;
	uint32_t atlas_size = 1;
	RID atlas_fb;

	bool reverse_cull_face = light_storage->light_get_reverse_cull_face_mode(base);
	bool using_dual_paraboloid = false;
	bool using_dual_paraboloid_flip = false;
	Vector2i dual_paraboloid_offset;
	RID render_fb;
	RID render_texture;
	float zfar;

	bool use_pancake = false;
	bool render_cubemap = false;
	bool finalize_cubemap = false;

	bool flip_y = false;

	Projection light_projection;
	Transform3D light_transform;

	if (light_storage->light_get_type(base) == RSE::LIGHT_DIRECTIONAL) {
		//set pssm stuff
		uint64_t last_scene_shadow_pass = light_storage->light_instance_get_shadow_pass(p_light);
		if (last_scene_shadow_pass != get_scene_pass()) {
			light_storage->light_instance_set_directional_rect(p_light, light_storage->get_directional_shadow_rect());
			light_storage->directional_shadow_increase_current_light();
			light_storage->light_instance_set_shadow_pass(p_light, get_scene_pass());
		}

		use_pancake = light_storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_PANCAKE_SIZE) > 0;
		light_projection = light_storage->light_instance_get_shadow_camera(p_light, p_pass);
		light_transform = light_storage->light_instance_get_shadow_transform(p_light, p_pass);

		atlas_rect = light_storage->light_instance_get_directional_rect(p_light);

		if (light_storage->light_directional_get_shadow_mode(base) == RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_4_SPLITS) {
			atlas_rect.size.width /= 2;
			atlas_rect.size.height /= 2;

			if (p_pass == 1) {
				atlas_rect.position.x += atlas_rect.size.width;
			} else if (p_pass == 2) {
				atlas_rect.position.y += atlas_rect.size.height;
			} else if (p_pass == 3) {
				atlas_rect.position += atlas_rect.size;
			}
		} else if (light_storage->light_directional_get_shadow_mode(base) == RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS) {
			atlas_rect.size.height /= 2;

			if (p_pass == 0) {
			} else {
				atlas_rect.position.y += atlas_rect.size.height;
			}
		}

		float directional_shadow_size = light_storage->directional_shadow_get_size();
		Rect2 atlas_rect_norm = atlas_rect;
		atlas_rect_norm.position /= directional_shadow_size;
		atlas_rect_norm.size /= directional_shadow_size;
		light_storage->light_instance_set_directional_shadow_atlas_rect(p_light, p_pass, atlas_rect_norm);

		zfar = RSG::light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE);

		render_fb = light_storage->direction_shadow_get_fb();
		render_texture = RID();
		flip_y = true;

	} else {
		//set from shadow atlas

		ERR_FAIL_COND(!light_storage->owns_shadow_atlas(p_shadow_atlas));
		ERR_FAIL_COND(!light_storage->shadow_atlas_owns_light_instance(p_shadow_atlas, p_light));

		RSG::light_storage->shadow_atlas_update(p_shadow_atlas);

		uint32_t key = light_storage->shadow_atlas_get_light_instance_key(p_shadow_atlas, p_light);

		uint32_t quadrant = (key >> RendererRD::LightStorage::QUADRANT_SHIFT) & 0x3;
		uint32_t shadow = key & RendererRD::LightStorage::SHADOW_INDEX_MASK;
		uint32_t subdivision = light_storage->shadow_atlas_get_quadrant_subdivision(p_shadow_atlas, quadrant);

		ERR_FAIL_INDEX((int)shadow, light_storage->shadow_atlas_get_quadrant_shadow_size(p_shadow_atlas, quadrant));

		uint32_t shadow_atlas_size = light_storage->shadow_atlas_get_size(p_shadow_atlas);
		uint32_t quadrant_size = shadow_atlas_size >> 1;

		atlas_rect.position.x = (quadrant & 1) * quadrant_size;
		atlas_rect.position.y = (quadrant >> 1) * quadrant_size;

		uint32_t shadow_size = (quadrant_size / subdivision);
		atlas_rect.position.x += (shadow % subdivision) * shadow_size;
		atlas_rect.position.y += (shadow / subdivision) * shadow_size;

		atlas_rect.size.width = shadow_size;
		atlas_rect.size.height = shadow_size;

		zfar = light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE);

		if (light_storage->light_get_type(base) == RSE::LIGHT_OMNI) {
			bool wrap = (shadow + 1) % subdivision == 0;
			dual_paraboloid_offset = wrap ? Vector2i(1 - subdivision, 1) : Vector2i(1, 0);

			if (light_storage->light_omni_get_shadow_mode(base) == RSE::LIGHT_OMNI_SHADOW_CUBE) {
				render_texture = light_storage->get_cubemap(shadow_size / 2);
				render_fb = light_storage->get_cubemap_fb(shadow_size / 2, p_pass);

				light_projection = light_storage->light_instance_get_shadow_camera(p_light, p_pass);
				light_transform = light_storage->light_instance_get_shadow_transform(p_light, p_pass);
				render_cubemap = true;
				finalize_cubemap = p_pass == 5;
				atlas_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);

				atlas_size = shadow_atlas_size;

				if (p_pass == 0) {
					_render_shadow_begin();
				}

			} else {
				atlas_rect.position.x += 1;
				atlas_rect.position.y += 1;
				atlas_rect.size.x -= 2;
				atlas_rect.size.y -= 2;

				atlas_rect.position += p_pass * atlas_rect.size * dual_paraboloid_offset;

				light_projection = light_storage->light_instance_get_shadow_camera(p_light, 0);
				light_transform = light_storage->light_instance_get_shadow_transform(p_light, 0);

				using_dual_paraboloid = true;
				using_dual_paraboloid_flip = p_pass == 1;
				render_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);
				flip_y = true;
			}

		} else if (light_storage->light_get_type(base) == RSE::LIGHT_SPOT) {
			light_projection = light_storage->light_instance_get_shadow_camera(p_light, 0);
			light_transform = light_storage->light_instance_get_shadow_transform(p_light, 0);

			render_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);

			flip_y = true;
		} else if (light_storage->light_get_type(base) == RSE::LIGHT_AREA) {
			Vector2 area_size = light_storage->light_area_get_size(base);

			zfar = light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE) + area_size.length() / 2.0;

			light_transform = light_storage->light_instance_get_shadow_transform(p_light, 0);

			light_projection = light_storage->light_instance_get_shadow_camera(p_light, 0);

			render_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);

			flip_y = true;

			using_dual_paraboloid = true;
		}
	}

	if (render_cubemap) {
		//rendering to cubemap
		_render_shadow_append(render_fb, p_instances, light_projection, light_transform, zfar, 0, 0, reverse_cull_face, false, false, use_pancake, p_lod_distance_multiplier, p_screen_mesh_lod_threshold, Rect2(), false, true, true, true, p_render_info, p_viewport_size, p_main_cam_transform, p_nextgeo_shadow_view);
		if (finalize_cubemap) {
			_render_shadow_process();
			_render_shadow_end();
			//reblit
			Rect2 atlas_rect_norm = atlas_rect;
			atlas_rect_norm.position /= float(atlas_size);
			atlas_rect_norm.size /= float(atlas_size);
			copy_effects->copy_cubemap_to_dp(render_texture, atlas_fb, atlas_rect_norm, atlas_rect.size, light_projection.get_z_near(), zfar, false);
			atlas_rect_norm.position += Vector2(dual_paraboloid_offset) * atlas_rect_norm.size;
			copy_effects->copy_cubemap_to_dp(render_texture, atlas_fb, atlas_rect_norm, atlas_rect.size, light_projection.get_z_near(), zfar, true);

			//restore transform so it can be properly used
			light_storage->light_instance_set_shadow_transform(p_light, Projection(), light_storage->light_instance_get_base_transform(p_light), zfar, 0, 0, 0);
		}

	} else {
		//render shadow
		_render_shadow_append(render_fb, p_instances, light_projection, light_transform, zfar, 0, 0, reverse_cull_face, using_dual_paraboloid, using_dual_paraboloid_flip, use_pancake, p_lod_distance_multiplier, p_screen_mesh_lod_threshold, atlas_rect, flip_y, p_clear_region, p_open_pass, p_close_pass, p_render_info, p_viewport_size, p_main_cam_transform, p_nextgeo_shadow_view);
	}
}

void RenderForwardClustered::_render_shadow_begin() {
	// GodotNext: nextgi_frame_block
	next_gi.notify_shadow_batch_begin();
	scene_state.shadow_passes.clear();
	RD::get_singleton()->draw_command_begin_label("Shadow Setup");
	_update_render_base_uniform_set();

	render_list[RENDER_LIST_SECONDARY].clear();
	// No need to reset scene_state.curr_gpu_ptr or scene_state.instance_buffer[RENDER_LIST_SECONDARY]
	// because _fill_instance_data will do that if it detects p_offset == 0u.
}

void RenderForwardClustered::_render_shadow_append(RID p_framebuffer, const PagedArray<RenderGeometryInstance *> &p_instances, const Projection &p_projection, const Transform3D &p_transform, float p_zfar, float p_bias, float p_normal_bias, bool p_reverse_cull_face, bool p_use_dp, bool p_use_dp_flip, bool p_use_pancake, float p_lod_distance_multiplier, float p_screen_mesh_lod_threshold, const Rect2i &p_rect, bool p_flip_y, bool p_clear_region, bool p_begin, bool p_end, RenderingServerTypes::RenderInfo *p_render_info, const Size2i &p_viewport_size, const Transform3D &p_main_cam_transform, int32_t p_nextgeo_shadow_view) {
	SceneState::ShadowPass shadow_pass;

	RenderSceneDataRD scene_data;
	scene_data.flip_y = !p_flip_y; // Q: Why is this inverted? Do we assume flip in shadow logic?
	scene_data.cam_projection = p_projection;
	scene_data.cam_transform = p_transform;
	scene_data.view_projection[0] = p_projection;
	scene_data.z_far = p_zfar;
	scene_data.z_near = 0.0;
	scene_data.lod_distance_multiplier = p_lod_distance_multiplier;
	scene_data.dual_paraboloid_side = p_use_dp_flip ? -1 : 1;
	scene_data.opaque_prepass_threshold = 0.1f;
	scene_data.time = time;
	scene_data.time_step = time_step;
	scene_data.main_cam_transform = p_main_cam_transform;
	scene_data.shadow_pass = true;

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;
	render_data.render_info = p_render_info;

	Size2i screen_size = RD::get_singleton()->framebuffer_get_size(p_framebuffer);
	Size2i viewport_size = p_rect.size;
	if (viewport_size == Size2()) {
		viewport_size = screen_size;
	}
	uint32_t uniform_buffer_index = _setup_environment(&render_data, true, screen_size, viewport_size, Color(), false, false, p_use_pancake);

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_DISABLE_LOD) {
		scene_data.screen_mesh_lod_threshold = 0.0;
	} else {
		scene_data.screen_mesh_lod_threshold = p_screen_mesh_lod_threshold;
	}

	PassMode pass_mode = p_use_dp ? PASS_MODE_SHADOW_DP : PASS_MODE_SHADOW;

	uint32_t render_list_from = render_list[RENDER_LIST_SECONDARY].elements.size();
	nextgeo_shadow_pass_active = p_nextgeo_shadow_view >= 0;
	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode, false, false, false, true);
	nextgeo_shadow_pass_active = false;
	uint32_t render_list_size = render_list[RENDER_LIST_SECONDARY].elements.size() - render_list_from;
	render_list[RENDER_LIST_SECONDARY].sort_by_key_range(render_list_from, render_list_size);
	_fill_instance_data(RENDER_LIST_SECONDARY, p_render_info ? p_render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_SHADOW] : (int *)nullptr, render_list_from, render_list_size, false);

	{
		//regular forward for now
		bool flip_cull = p_use_dp_flip;
		if (p_flip_y) {
			flip_cull = !flip_cull;
		}

		if (p_reverse_cull_face) {
			flip_cull = !flip_cull;
		}

		shadow_pass.element_from = render_list_from;
		shadow_pass.element_count = render_list_size;
		shadow_pass.flip_cull = flip_cull;
		shadow_pass.pass_mode = pass_mode;

		shadow_pass.rp_uniform_set = RID(); //will be filled later when instance buffer is complete
		shadow_pass.screen_mesh_lod_threshold = scene_data.screen_mesh_lod_threshold;
		shadow_pass.lod_distance_multiplier = scene_data.lod_distance_multiplier;

		shadow_pass.framebuffer = p_framebuffer;
		shadow_pass.clear_depth = p_begin || p_clear_region;
		shadow_pass.rect = p_rect;

		shadow_pass.uniform_buffer_index = uniform_buffer_index;
		shadow_pass.nextgeo_shadow_view = p_nextgeo_shadow_view;
		shadow_pass.use_nextgeo_proxy = nextgeo_proxy_active && p_nextgeo_shadow_view < 0;

		scene_state.shadow_passes.push_back(shadow_pass);
	}
}

void RenderForwardClustered::_render_shadow_process() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (scene_state.instance_buffer[RENDER_LIST_SECONDARY].get_size(0u) > 0u) {
		rd->buffer_flush(scene_state.instance_buffer[RENDER_LIST_SECONDARY]._get(0u));
	}

	//render shadows one after the other, so this can be done un-barriered and the driver can optimize (as well as allow us to run compute at the same time)

	for (uint32_t i = 0; i < scene_state.shadow_passes.size(); i++) {
		//render passes need to be configured after instance buffer is done, since they need the latest version
		SceneState::ShadowPass &shadow_pass = scene_state.shadow_passes[i];
		shadow_pass.rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, false, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), shadow_pass.uniform_buffer_index, false);
	}

	RD::get_singleton()->draw_command_end_label();
}
void RenderForwardClustered::_render_shadow_end() {
	RD::get_singleton()->draw_command_begin_label("Shadow Render");

	bool has_nextgeo_shadows = false;
	for (SceneState::ShadowPass &shadow_pass : scene_state.shadow_passes) {
		RenderListParameters render_list_parameters(render_list[RENDER_LIST_SECONDARY].elements.ptr() + shadow_pass.element_from, render_list[RENDER_LIST_SECONDARY].element_info.ptr() + shadow_pass.element_from, shadow_pass.element_count, shadow_pass.flip_cull, shadow_pass.pass_mode, 0, true, false, shadow_pass.rp_uniform_set, false, Vector2(), shadow_pass.lod_distance_multiplier, shadow_pass.screen_mesh_lod_threshold, 1, shadow_pass.element_from);
		render_list_parameters.use_nextgeo_proxy = shadow_pass.use_nextgeo_proxy;
		_render_list_with_draw_list(&render_list_parameters, shadow_pass.framebuffer, shadow_pass.clear_depth ? RD::DRAW_CLEAR_DEPTH : RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0.0f, 0, shadow_pass.rect);
		if (shadow_pass.nextgeo_shadow_view >= 0) {
			has_nextgeo_shadows = true;
		}
	}

	if (has_nextgeo_shadows) {
		RD::get_singleton()->draw_command_begin_label("NextGeo Shadow Raster");
		nextgeo_cut.begin_shadow_raster_timestamps();
		for (SceneState::ShadowPass &shadow_pass : scene_state.shadow_passes) {
			if (shadow_pass.nextgeo_shadow_view >= 0) {
				nextgeo_cut.draw_shadow_view(uint32_t(shadow_pass.nextgeo_shadow_view), shadow_pass.framebuffer, shadow_pass.rect, shadow_pass.pass_mode == PASS_MODE_SHADOW_DP);
			}
		}
		nextgeo_cut.end_shadow_raster_timestamps();
		RD::get_singleton()->draw_command_end_label();
	}

	RD::get_singleton()->draw_command_end_label();
	// GodotNext: nextgi_frame_block
	next_gi.notify_shadow_batch_end();
}

void RenderForwardClustered::_render_particle_collider_heightfield(RID p_fb, const Transform3D &p_cam_transform, const Projection &p_cam_projection, const PagedArray<RenderGeometryInstance *> &p_instances) {
	RENDER_TIMESTAMP("Setup GPUParticlesCollisionHeightField3D");

	RD::get_singleton()->draw_command_begin_label("Render Collider Heightfield");
	NextGeoBoolScope proxy_scope(nextgeo_proxy_active, true);

	RenderSceneDataRD scene_data;
	scene_data.flip_y = true;
	scene_data.cam_projection = p_cam_projection;
	scene_data.cam_transform = p_cam_transform;
	scene_data.view_projection[0] = p_cam_projection;
	scene_data.z_near = 0.0;
	scene_data.z_far = p_cam_projection.get_z_far();
	scene_data.dual_paraboloid_side = 0;
	scene_data.opaque_prepass_threshold = 0.0;
	scene_data.time = time;
	scene_data.time_step = time_step;
	scene_data.main_cam_transform = p_cam_transform;
	scene_data.shadow_pass = true; // Not a shadow pass, but should be treated like one.

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;

	_update_render_base_uniform_set();

	Size2i screen_size = RD::get_singleton()->framebuffer_get_size(p_fb);
	uint32_t uniform_buffer_index = _setup_environment(&render_data, true, screen_size, screen_size, Color(), false, false, false);

	PassMode pass_mode = PASS_MODE_SHADOW;

	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode);
	render_list[RENDER_LIST_SECONDARY].sort_by_key();
	_fill_instance_data(RENDER_LIST_SECONDARY);

	RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, false, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), uniform_buffer_index);

	RENDER_TIMESTAMP("Render Collider Heightfield");

	{
		//regular forward for now
		RenderListParameters render_list_params(render_list[RENDER_LIST_SECONDARY].elements.ptr(), render_list[RENDER_LIST_SECONDARY].element_info.ptr(), render_list[RENDER_LIST_SECONDARY].elements.size(), false, pass_mode, 0, true, false, rp_uniform_set);
		render_list_params.use_nextgeo_proxy = true;
		_render_list_with_draw_list(&render_list_params, p_fb, RD::DRAW_CLEAR_ALL);
	}
	RD::get_singleton()->draw_command_end_label();
}

uint32_t RenderForwardClustered::_render_nextgi_card_batch(const NextGICardCapture *p_cards, uint32_t p_count, RID p_framebuffer) {
	if (p_cards == nullptr || p_count == 0 || !p_framebuffer.is_valid()) {
		return 0;
	}
	NextGeoBoolScope proxy_scope(nextgeo_proxy_active, true);
	p_count = MIN(p_count, 64u);
	RenderingDevice *rd = RD::get_singleton();
	_update_render_base_uniform_set();
	render_list[RENDER_LIST_SECONDARY].clear();
	if (cull_argument.size() == 0) {
		cull_argument.push_back(nullptr);
	}

	struct CardPass {
		uint32_t element_from = 0;
		uint32_t element_count = 0;
		uint32_t uniform_buffer_index = 0;
		Rect2i rect;
		RID rp_uniform_set;
	};
	LocalVector<CardPass> passes;
	const Size2i screen_size = rd->framebuffer_get_size(p_framebuffer);
	for (uint32_t i = 0; i < p_count; i++) {
		RenderSceneDataRD scene_data;
		scene_data.cam_projection = p_cards[i].projection;
		scene_data.cam_transform = p_cards[i].cam_transform;
		scene_data.view_projection[0] = p_cards[i].projection;
		scene_data.dual_paraboloid_side = 0;
		scene_data.material_uv2_mode = false;
		scene_data.opaque_prepass_threshold = 0.0f;
		scene_data.emissive_exposure_normalization = 1.0f;
		scene_data.time = time;
		scene_data.time_step = time_step;
		scene_data.main_cam_transform = p_cards[i].cam_transform;

		cull_argument[0] = p_cards[i].instance;
		RenderDataRD render_data;
		render_data.scene_data = &scene_data;
		render_data.cluster_size = 1;
		render_data.cluster_max_elements = 32;
		render_data.instances = &cull_argument;

		Size2i viewport_size = p_cards[i].rect.size;
		if (viewport_size == Size2i()) {
			viewport_size = screen_size;
		}
		CardPass pass;
		pass.uniform_buffer_index = _setup_environment(&render_data, true, screen_size, viewport_size, Color());
		pass.element_from = render_list[RENDER_LIST_SECONDARY].elements.size();
		_fill_render_list(RENDER_LIST_SECONDARY, &render_data, PASS_MODE_DEPTH_MATERIAL, false, false, false, true);
		pass.element_count = render_list[RENDER_LIST_SECONDARY].elements.size() - pass.element_from;
		render_list[RENDER_LIST_SECONDARY].sort_by_key_range(pass.element_from, pass.element_count);
		_fill_instance_data(RENDER_LIST_SECONDARY, nullptr, pass.element_from, pass.element_count, false);
		pass.rect = p_cards[i].rect;
		passes.push_back(pass);
	}
	if (scene_state.instance_buffer[RENDER_LIST_SECONDARY].get_size(0u) > 0u) {
		rd->buffer_flush(scene_state.instance_buffer[RENDER_LIST_SECONDARY]._get(0u));
	}
	for (CardPass &pass : passes) {
		pass.rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, false, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), pass.uniform_buffer_index, false);
	}

	const RD::FramebufferFormatID fb_format = rd->framebuffer_get_format(p_framebuffer);
	// Depth of new rects must start at 0. Color stays: earlier cards already live in this atlas.
	const RD::DrawListID draw_list = rd->draw_list_begin(p_framebuffer, RD::DRAW_CLEAR_DEPTH, Vector<Color>(), 0.0f);
	for (const CardPass &pass : passes) {
		if (pass.element_count == 0) {
			continue;
		}
		rd->draw_list_set_viewport(draw_list, pass.rect);
		rd->draw_list_enable_scissor(draw_list, Rect2(Point2(), pass.rect.size));
		RenderListParameters params(render_list[RENDER_LIST_SECONDARY].elements.ptr() + pass.element_from, render_list[RENDER_LIST_SECONDARY].element_info.ptr() + pass.element_from, pass.element_count, true, PASS_MODE_DEPTH_MATERIAL, 0, true, false, pass.rp_uniform_set, false, Vector2(), 0.0f, 0.0f, 1, pass.element_from);
		params.framebuffer_format = fb_format;
		params.use_nextgeo_proxy = true;
		_render_list(draw_list, fb_format, &params, 0, pass.element_count);
	}
	rd->draw_list_end();
	return 1;
}

void RenderForwardClustered::_render_material(const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region, float p_exposure_normalization) {
	RENDER_TIMESTAMP("Setup Rendering 3D Material");

	RD::get_singleton()->draw_command_begin_label("Render 3D Material");
	NextGeoBoolScope proxy_scope(nextgeo_proxy_active, true);

	RenderSceneDataRD scene_data;
	scene_data.cam_projection = p_cam_projection;
	scene_data.cam_transform = p_cam_transform;
	scene_data.view_projection[0] = p_cam_projection;
	scene_data.dual_paraboloid_side = 0;
	scene_data.material_uv2_mode = false;
	scene_data.opaque_prepass_threshold = 0.0f;
	scene_data.emissive_exposure_normalization = p_exposure_normalization;
	scene_data.time = time;
	scene_data.time_step = time_step;
	scene_data.main_cam_transform = p_cam_transform;

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;

	scene_shader.enable_advanced_shader_group();

	_update_render_base_uniform_set();

	Size2i screen_size = RD::get_singleton()->framebuffer_get_size(p_framebuffer);
	Size2i viewport_size = p_region.size;
	if (viewport_size == Size2()) {
		viewport_size = screen_size;
	}
	uint32_t uniform_buffer_index = _setup_environment(&render_data, true, screen_size, viewport_size, Color());

	PassMode pass_mode = PASS_MODE_DEPTH_MATERIAL;
	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode);
	render_list[RENDER_LIST_SECONDARY].sort_by_key();
	_fill_instance_data(RENDER_LIST_SECONDARY);

	RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, false, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), uniform_buffer_index);

	RENDER_TIMESTAMP("Render 3D Material");

	{
		RenderListParameters render_list_params(render_list[RENDER_LIST_SECONDARY].elements.ptr(), render_list[RENDER_LIST_SECONDARY].element_info.ptr(), render_list[RENDER_LIST_SECONDARY].elements.size(), true, pass_mode, 0, true, false, rp_uniform_set);
		render_list_params.use_nextgeo_proxy = true;
		//regular forward for now
		Vector<Color> clear = {
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0)
		};

		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, RD::DRAW_CLEAR_ALL, clear, 0.0f, 0, p_region);
		_render_list(draw_list, RD::get_singleton()->framebuffer_get_format(p_framebuffer), &render_list_params, 0, render_list_params.element_count);
		RD::get_singleton()->draw_list_end();
	}

	RD::get_singleton()->draw_command_end_label();
}

void RenderForwardClustered::_render_uv2(const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region) {
	RENDER_TIMESTAMP("Setup Rendering UV2");

	RD::get_singleton()->draw_command_begin_label("Render UV2");
	// GodotNext: nextgeo_exclusive_dispatch (#79, plan §7.5). LightmapGI UV2
	// baking always draws authored legacy surfaces, never the NextGeo proxy.
	NextGeoBoolScope proxy_scope(nextgeo_proxy_active, false);

	RenderSceneDataRD scene_data;
	scene_data.dual_paraboloid_side = 0;
	scene_data.material_uv2_mode = true;
	scene_data.opaque_prepass_threshold = 0.0;
	scene_data.emissive_exposure_normalization = -1.0;

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;

	scene_shader.enable_advanced_shader_group();

	_update_render_base_uniform_set();

	Size2i screen_size = RD::get_singleton()->framebuffer_get_size(p_framebuffer);
	Size2i viewport_size = p_region.size;
	if (viewport_size == Size2()) {
		viewport_size = screen_size;
	}
	uint32_t uniform_buffer_index = _setup_environment(&render_data, true, screen_size, viewport_size, Color());

	PassMode pass_mode = PASS_MODE_DEPTH_MATERIAL;
	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode);
	render_list[RENDER_LIST_SECONDARY].sort_by_key();
	_fill_instance_data(RENDER_LIST_SECONDARY);

	RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_SECONDARY, nullptr, false, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), uniform_buffer_index);

	RENDER_TIMESTAMP("Render 3D Material");

	{
		RenderListParameters render_list_params(render_list[RENDER_LIST_SECONDARY].elements.ptr(), render_list[RENDER_LIST_SECONDARY].element_info.ptr(), render_list[RENDER_LIST_SECONDARY].elements.size(), true, pass_mode, 0, true, false, rp_uniform_set, true);
		render_list_params.is_uv2_bake = true;
		//regular forward for now
		Vector<Color> clear = {
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0),
			Color(0, 0, 0, 0)
		};
		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, RD::DRAW_CLEAR_ALL, clear, 0.0f, 0, p_region);

		const int uv_offset_count = 9;
		static const Vector2 uv_offsets[uv_offset_count] = {
			Vector2(-1, 1),
			Vector2(1, 1),
			Vector2(1, -1),
			Vector2(-1, -1),
			Vector2(-1, 0),
			Vector2(1, 0),
			Vector2(0, -1),
			Vector2(0, 1),
			Vector2(0, 0),

		};

		for (int i = 0; i < uv_offset_count; i++) {
			Vector2 ofs = uv_offsets[i];
			ofs.x /= p_region.size.width;
			ofs.y /= p_region.size.height;
			render_list_params.uv_offset = ofs;
			_render_list(draw_list, RD::get_singleton()->framebuffer_get_format(p_framebuffer), &render_list_params, 0, render_list_params.element_count); //first wireframe, for pseudo conservative
		}
		render_list_params.uv_offset = Vector2();
		render_list_params.force_wireframe = false;
		_render_list(draw_list, RD::get_singleton()->framebuffer_get_format(p_framebuffer), &render_list_params, 0, render_list_params.element_count); //second regular triangles

		RD::get_singleton()->draw_list_end();
	}

	RD::get_singleton()->draw_command_end_label();
}

void RenderForwardClustered::_render_sdfgi(Ref<RenderSceneBuffersRD> p_render_buffers, const Vector3i &p_from, const Vector3i &p_size, const AABB &p_bounds, const PagedArray<RenderGeometryInstance *> &p_instances, const RID &p_albedo_texture, const RID &p_emission_texture, const RID &p_emission_aniso_texture, const RID &p_geom_facing_texture, float p_exposure_normalization) {
	RENDER_TIMESTAMP("Render SDFGI");

	RD::get_singleton()->draw_command_begin_label("Render SDFGI Voxel");
	NextGeoBoolScope proxy_scope(nextgeo_proxy_active, true);

	RenderSceneDataRD scene_data;

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;

	_update_render_base_uniform_set();

	// Indicate pipelines for SDFGI are required.
	global_pipeline_data_required.use_sdfgi = true;

	PassMode pass_mode = PASS_MODE_SDF;
	_fill_render_list(RENDER_LIST_SECONDARY, &render_data, pass_mode);
	render_list[RENDER_LIST_SECONDARY].sort_by_key();
	_fill_instance_data(RENDER_LIST_SECONDARY);

	Vector3 half_size = p_bounds.size * 0.5;
	Vector3 center = p_bounds.position + half_size;

	//print_line("re-render " + p_from + " - " + p_size + " bounds " + p_bounds);
	for (int i = 0; i < 3; i++) {
		scene_state.ubo.sdf_offset[i] = p_from[i];
		scene_state.ubo.sdf_size[i] = p_size[i];
	}

	for (int i = 0; i < 3; i++) {
		Vector3 axis;
		axis[i] = 1.0;
		Vector3 up, right;
		int right_axis = (i + 1) % 3;
		int up_axis = (i + 2) % 3;
		up[up_axis] = 1.0;
		right[right_axis] = 1.0;

		Size2i fb_size;
		fb_size.x = p_size[right_axis];
		fb_size.y = p_size[up_axis];

		scene_data.cam_transform.origin = center + axis * half_size;
		scene_data.cam_transform.basis.set_column(0, right);
		scene_data.cam_transform.basis.set_column(1, up);
		scene_data.cam_transform.basis.set_column(2, axis);

		//print_line("pass: " + itos(i) + " xform " + scene_data.cam_transform);

		float h_size = half_size[right_axis];
		float v_size = half_size[up_axis];
		float d_size = half_size[i] * 2.0;
		scene_data.cam_projection.set_orthogonal(-h_size, h_size, -v_size, v_size, 0, d_size);
		//print_line("pass: " + itos(i) + " cam hsize: " + rtos(h_size) + " vsize: " + rtos(v_size) + " dsize " + rtos(v_size));

		Transform3D to_bounds;
		to_bounds.origin = p_bounds.position;
		to_bounds.basis.scale(p_bounds.size);

		RendererRD::MaterialStorage::store_transform(to_bounds.affine_inverse() * scene_data.cam_transform, scene_state.ubo.sdf_to_bounds);

		scene_data.emissive_exposure_normalization = p_exposure_normalization;
		uint32_t uniform_buffer_index = _setup_environment(&render_data, true, fb_size, fb_size, Color());

		RID rp_uniform_set = _setup_sdfgi_render_pass_uniform_set(p_albedo_texture, p_emission_texture, p_emission_aniso_texture, p_geom_facing_texture, RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), uniform_buffer_index);

		HashMap<Size2i, RID>::Iterator E = sdfgi_framebuffer_size_cache.find(fb_size);
		if (!E) {
			RID fb = RD::get_singleton()->framebuffer_create_empty(fb_size);
			E = sdfgi_framebuffer_size_cache.insert(fb_size, fb);
		}

		RenderListParameters render_list_params(render_list[RENDER_LIST_SECONDARY].elements.ptr(), render_list[RENDER_LIST_SECONDARY].element_info.ptr(), render_list[RENDER_LIST_SECONDARY].elements.size(), true, pass_mode, 0, true, false, rp_uniform_set, false);
		render_list_params.use_nextgeo_proxy = true;
		_render_list_with_draw_list(&render_list_params, E->value);
	}

	RD::get_singleton()->draw_command_end_label();
}

void RenderForwardClustered::base_uniforms_changed() {
	if (!render_base_uniform_set.is_null() && RD::get_singleton()->uniform_set_is_valid(render_base_uniform_set)) {
		RD::get_singleton()->free_rid(render_base_uniform_set);
	}
	render_base_uniform_set = RID();
}

// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7), nextgeo_vertex_offset (#164).
RendererRD::NextGeoShadingReason RenderForwardClustered::_nextgeo_shading_stage(bool p_stage_b, const Ref<RenderSceneBuffersRD> &p_rb, const Ref<RenderBufferDataForwardClustered> &p_rb_data, bool p_voxel_gi, bool p_megalights_inputs, uint32_t p_color_pass_flags, bool p_zero_velocity_write, const LocalVector<RD::Uniform> &p_pass_uniforms, NextGeoResolve::ComputeStage &r_stage, bool p_stage_a_velocity) {
	if (!nextgeo_shading_compute_active || p_rb.is_null() || p_rb_data.is_null()) {
		return RendererRD::NEXTGEO_SHADING_REASON_SETTING;
	}
	if (nextgeo_resolve.get_compute_bucket_count() == 0u) {
		return RendererRD::NEXTGEO_SHADING_REASON_COMPUTE_REJECTED;
	}
	if (!nextgeo_resolve.is_binned() || p_pass_uniforms.is_empty() || render_base_uniforms.is_empty()) {
		return RendererRD::NEXTGEO_SHADING_REASON_SETTING;
	}

	// The stage's targets, in resolve_compute_inc.glsl's binding order; it declares their formats
	// (NextGeoResolve::COMPUTE_STAGE_A_FORMATS and COMPUTE_STAGE_B_FORMATS).
	RID targets[NextGeoResolve::COMPUTE_IMAGE_COUNT];
	bool needed[NextGeoResolve::COMPUTE_IMAGE_COUNT] = {};
	const RD::DataFormat *formats = p_stage_b ? NextGeoResolve::COMPUTE_STAGE_B_FORMATS : NextGeoResolve::COMPUTE_STAGE_A_FORMATS;
	r_stage.flags = 0u;
	if (!p_stage_b) {
		r_stage.variant = SceneShaderForwardClustered::resolve_compute_stage_a_variant(p_voxel_gi, p_megalights_inputs, p_stage_a_velocity);
		targets[0] = p_rb_data->get_normal_roughness();
		needed[0] = true;
		if (p_megalights_inputs) {
			targets[1] = p_rb->get_ml_albedo();
			targets[2] = p_rb->get_ml_material();
			needed[1] = needed[2] = true;
		}
		if (p_voxel_gi) {
			targets[3] = p_rb_data->get_voxelgi();
			needed[3] = true;
		}
		targets[4] = p_rb->get_nextgeo_instance_id();
		needed[4] = true;
		if (p_stage_a_velocity) {
			targets[5] = p_rb->get_velocity_buffer(false);
			needed[5] = true;
		}
	} else {
		// The DLSS-RR albedo guides are attachments without storage usage.
		if ((p_color_pass_flags & COLOR_PASS_FLAG_RR_GUIDES) != 0u) {
			return RendererRD::NEXTGEO_SHADING_REASON_NO_STORAGE;
		}
		// The colour pass shades at the variable rate; a compute bin shades every pixel.
		if (p_rb->has_texture(RB_SCOPE_VRS, RB_TEXTURE)) {
			return RendererRD::NEXTGEO_SHADING_REASON_VRS;
		}
		const bool separate_specular = (p_color_pass_flags & COLOR_PASS_FLAG_SEPARATE_SPECULAR) != 0u;
		const bool motion_vectors = (p_color_pass_flags & COLOR_PASS_FLAG_MOTION_VECTORS) != 0u;
		r_stage.variant = SceneShaderForwardClustered::resolve_compute_stage_b_variant(separate_specular, motion_vectors);
		targets[0] = p_rb->get_internal_texture();
		needed[0] = true;
		if (separate_specular) {
			targets[1] = p_rb->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_SPECULAR);
			needed[1] = true;
		}
		if (motion_vectors) {
			targets[2] = p_rb->get_velocity_buffer(false);
			needed[2] = true;
			r_stage.flags = p_zero_velocity_write ? NextGeoResolve::COMPUTE_FLAG_SKIP_VELOCITY : 0u;
		}
	}
	const bool deny_storage = bool(GLOBAL_GET("rendering/nextgeo/debug_shading_bins_no_storage"));
	for (uint32_t i = 0; i < NextGeoResolve::COMPUTE_IMAGE_COUNT; i++) {
		r_stage.images[i] = RID();
		if (!needed[i]) {
			continue;
		}
		if (deny_storage || formats[i] == RD::DATA_FORMAT_MAX || !NextGeoResolve::compute_target_storable(targets[i], formats[i])) {
			return RendererRD::NEXTGEO_SHADING_REASON_NO_STORAGE;
		}
		r_stage.images[i] = targets[i];
	}

	r_stage.scene_uniforms.clear();
	for (const RD::Uniform &uniform : render_base_uniforms) {
		r_stage.scene_uniforms.push_back(uniform);
	}
	r_stage.pass_uniforms = p_pass_uniforms;
	return RendererRD::NEXTGEO_SHADING_REASON_NONE;
}

void RenderForwardClustered::_update_render_base_uniform_set() {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	if (render_base_uniform_set.is_null() || !RD::get_singleton()->uniform_set_is_valid(render_base_uniform_set) || (lightmap_texture_array_version != light_storage->lightmap_array_get_version())) {
		if (render_base_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(render_base_uniform_set)) {
			RD::get_singleton()->free_rid(render_base_uniform_set);
		}

		lightmap_texture_array_version = light_storage->lightmap_array_get_version();

		Vector<RD::Uniform> uniforms;

		{
			RD::Uniform u;
			u.binding = 2;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.append_id(scene_shader.shadow_sampler);
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 3;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_omni_light_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 4;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_spot_light_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 5;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_area_light_buffer());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 6;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_reflection_probe_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 7;
			u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_directional_light_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 8;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(scene_state.lightmap_buffer);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 9;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(scene_state.lightmap_capture_buffer);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 10;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			RID decal_atlas = RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture();
			u.append_id(decal_atlas);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 11;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			RID decal_atlas = RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture_srgb();
			u.append_id(decal_atlas);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 12;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::TextureStorage::get_singleton()->get_decal_buffer());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 13;
			u.append_id(RendererRD::MaterialStorage::get_singleton()->global_shader_uniforms_get_storage_buffer());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			u.binding = 14;
			u.append_id(sdfgi_get_ubo());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 15;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.append_id(RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CanvasItemTextureFilter::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CanvasItemTextureRepeat::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 16;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.append_id(best_fit_normal.texture);
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 17;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.append_id(dfg_lut.texture);
			uniforms.push_back(u);
		}

		{ // Lookup-table for Area Lights - Linearly transformed cosines (LTC)
			if (ltc.lut1_texture.is_null() || ltc.lut2_texture.is_null()) {
				Ref<Image> lut1_image;
				int dimensions = LTC_LUT_DIMENSIONS;
				int lut1_bytes = 4 * dimensions * dimensions;
				size_t lut1_size = lut1_bytes * 4; // float

				Vector<uint8_t> lut1_data;
				lut1_data.resize(lut1_size);

				memcpy(lut1_data.ptrw(), LTC_LUT1, lut1_size);
				lut1_image = Image::create_from_data(dimensions, dimensions, false, Image::FORMAT_RGBAF, lut1_data);

				ltc.lut1_texture = RS::get_singleton()->texture_2d_create(lut1_image);

				int lut2_bytes = 4 * dimensions * dimensions;
				size_t lut2_size = lut2_bytes * 4;

				Ref<Image> lut2_image;
				Vector<uint8_t> lut2_data;
				lut2_data.resize(lut2_size);

				memcpy(lut2_data.ptrw(), LTC_LUT2, lut2_size);
				lut2_image = Image::create_from_data(dimensions, dimensions, false, Image::FORMAT_RGBAF, lut2_data);

				ltc.lut2_texture = RS::get_singleton()->texture_2d_create(lut2_image);
			}
		}

		{
			RD::Uniform u;
			u.binding = 18;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.append_id(RendererRD::TextureStorage::get_singleton()->texture_get_rd_texture(ltc.lut1_texture));
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 19;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.append_id(RendererRD::TextureStorage::get_singleton()->texture_get_rd_texture(ltc.lut2_texture));
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 20;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			RID area_light_atlas = RendererRD::TextureStorage::get_singleton()->area_light_atlas_get_texture();
			u.append_id(area_light_atlas);
			uniforms.push_back(u);
		}

		render_base_uniform_set = RD::get_singleton()->uniform_set_create(uniforms, scene_shader.default_shader_rd, SCENE_UNIFORM_SET);
		render_base_uniforms = uniforms; // GodotNext: nextgeo_shading_bins (#158)
	}
}

// GodotNext: megalights_decals (#136). p_cluster_buffer binds the cluster to a pass that has no render data.
// GodotNext: nextgeo_shading_bins (#158). r_uniforms, when given, receives the set's uniforms.
RID RenderForwardClustered::_setup_render_pass_uniform_set(RenderListType p_render_list, const RenderDataRD *p_render_data, bool p_is_multiview, RID p_radiance_texture, const RendererRD::MaterialStorage::Samplers &p_samplers, uint32_t p_uniform_buffer_index, bool p_use_directional_shadow_atlas, bool p_front_layer_pass, RID p_cluster_buffer, LocalVector<RD::Uniform> *r_uniforms) {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	bool is_multiview = p_is_multiview;

	Ref<RenderSceneBuffersRD> rb; // handy for not having to fully type out p_render_data->render_buffers all the time...
	Ref<RenderBufferDataForwardClustered> rb_data;
	if (p_render_data && p_render_data->render_buffers.is_valid()) {
		rb = p_render_data->render_buffers;
		if (rb->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
			// Our forward clustered custom data buffer will only be available when we're rendering our normal view.
			// This will not be available when rendering reflection probes.
			rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
		}
	}

	//default render buffer and scene state uniform set

	thread_local LocalVector<RD::Uniform> uniforms;
	uniforms.clear();

	{
		RD::Uniform u;
		u.binding = 0;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 1;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.implementation_uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 2;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC;
		if (scene_state.instance_buffer[p_render_list].get_size(0u) == 0u) {
			// Any buffer will do since it's not used, so just create one.
			// We can't use scene_shader.default_vec4_xform_buffer because it's not dynamic.
			scene_state.instance_buffer[p_render_list].set_storage_size(0u, INSTANCE_DATA_BUFFER_MIN_SIZE * sizeof(SceneState::InstanceData));
			scene_state.instance_buffer[p_render_list].prepare_for_upload();
		}
		RID instance_buffer = scene_state.instance_buffer[p_render_list]._get(0u);
		u.append_id(instance_buffer);
		uniforms.push_back(u);
	}
	{
		RID radiance_texture;
		if (p_radiance_texture.is_valid()) {
			radiance_texture = p_radiance_texture;
		} else {
			radiance_texture = texture_storage->texture_rd_get_default(is_using_radiance_octmap_array() ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		}
		RD::Uniform u;
		u.binding = 3;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(radiance_texture);
		uniforms.push_back(u);
	}
	{
		RID ref_texture = (p_render_data && p_render_data->reflection_atlas.is_valid()) ? light_storage->reflection_atlas_get_texture(p_render_data->reflection_atlas) : RID();
		RD::Uniform u;
		u.binding = 4;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		if (ref_texture.is_valid()) {
			u.append_id(ref_texture);
		} else {
			u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK));
		}
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 5;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture;
		if (p_render_data && p_render_data->shadow_atlas.is_valid()) {
			texture = RendererRD::LightStorage::get_singleton()->shadow_atlas_get_texture(p_render_data->shadow_atlas);
		}
		if (!texture.is_valid()) {
			texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		}
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 6;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		if (p_use_directional_shadow_atlas && RendererRD::LightStorage::get_singleton()->directional_shadow_get_texture().is_valid()) {
			u.append_id(RendererRD::LightStorage::get_singleton()->directional_shadow_get_texture());
		} else {
			u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH));
		}
		uniforms.push_back(u);
	}
	{
		Vector<RID> textures;
		textures.resize(scene_state.max_lightmaps * 2);

		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);
		for (uint32_t i = 0; i < scene_state.max_lightmaps * 2; i++) {
			uint32_t current_lightmap_index = i < scene_state.max_lightmaps ? i : i - scene_state.max_lightmaps;

			if (p_render_data && current_lightmap_index < p_render_data->lightmaps->size()) {
				RID base = light_storage->lightmap_instance_get_lightmap((*p_render_data->lightmaps)[current_lightmap_index]);
				RID texture;

				if (i < scene_state.max_lightmaps) {
					// Lightmap
					texture = light_storage->lightmap_get_texture(base);
				} else {
					// Shadowmask
					texture = light_storage->shadowmask_get_texture(base);
				}

				if (texture.is_valid()) {
					RID rd_texture = texture_storage->texture_get_rd_texture(texture);
					textures.write[i] = rd_texture;
					continue;
				}
			}

			textures.write[i] = default_tex;
		}
		RD::Uniform u(RD::UNIFORM_TYPE_TEXTURE, 7, textures);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 8;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		for (int i = 0; i < MAX_VOXEL_GI_INSTANCESS; i++) {
			if (p_render_data && i < (int)p_render_data->voxel_gi_instances->size()) {
				RID tex = gi.voxel_gi_instance_get_texture((*p_render_data->voxel_gi_instances)[i]);
				if (!tex.is_valid()) {
					tex = default_tex;
				}
				u.append_id(tex);
			} else {
				u.append_id(default_tex);
			}
		}

		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 9;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		RID cb = (p_render_data && p_render_data->cluster_buffer.is_valid()) ? p_render_data->cluster_buffer : scene_shader.default_vec4_xform_buffer;
		// GodotNext: megalights_decals (#136)
		if (p_render_data == nullptr && p_cluster_buffer.is_valid()) {
			cb = p_cluster_buffer;
		}
		u.append_id(cb);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 10;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (decals_get_filter()) {
			case RSE::DECAL_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}

		u.append_id(sampler);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 11;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (light_projectors_get_filter()) {
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}

		u.append_id(sampler);
		uniforms.push_back(u);
	}

	p_samplers.append_uniforms(uniforms, 12);

	{
		RD::Uniform u;
		u.binding = 24;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture;
		if (rb.is_valid() && rb->has_texture(RB_SCOPE_BUFFERS, RB_TEX_BACK_DEPTH)) {
			texture = rb->get_texture(RB_SCOPE_BUFFERS, RB_TEX_BACK_DEPTH);
		} else {
			texture = texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_DEPTH : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		}
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 25;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID bbt = rb_data.is_valid() ? rb->get_back_buffer_texture() : RID();
		RID texture = bbt.is_valid() ? bbt : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 26;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = rb_data.is_valid() && rb_data->has_normal_roughness() ? rb_data->get_normal_roughness() : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_NORMAL : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_NORMAL);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 27;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID aot = rb.is_valid() && rb->has_texture(RB_SCOPE_SSAO, RB_FINAL) ? rb->get_texture(RB_SCOPE_SSAO, RB_FINAL) : RID();
		RID texture = aot.is_valid() ? aot : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 28;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = rb_data.is_valid() && rb->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT) ? rb->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT) : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 29;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = rb_data.is_valid() && rb->has_texture(RB_SCOPE_GI, RB_TEX_REFLECTION) ? rb->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION) : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 30;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID t;
		if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_SDFGI)) {
			Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
			t = sdfgi->lightprobe_texture;
		}
		if (t.is_null()) {
			t = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);
		}
		u.append_id(t);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 31;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID t;
		if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_SDFGI)) {
			Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
			t = sdfgi->occlusion_texture;
		}
		if (t.is_null()) {
			t = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		}
		u.append_id(t);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 32;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		RID voxel_gi;
		if (rb.is_valid() && rb->has_custom_data(RB_SCOPE_GI)) {
			Ref<RendererRD::GI::RenderBuffersGI> rbgi = rb->get_custom_data(RB_SCOPE_GI);
			voxel_gi = rbgi->get_voxel_gi_buffer();
		}
		u.append_id(voxel_gi.is_valid() ? voxel_gi : render_buffers_get_default_voxel_gi_buffer());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 33;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID vfog;
		if (rb_data.is_valid() && rb->has_custom_data(RB_SCOPE_FOG)) {
			Ref<RendererRD::Fog::VolumetricFog> fog = rb->get_custom_data(RB_SCOPE_FOG);
			vfog = fog->fog_map;
			if (vfog.is_null()) {
				vfog = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
			}
		} else {
			vfog = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		}
		u.append_id(vfog);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 34;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID ssil = rb.is_valid() && rb->has_texture(RB_SCOPE_SSIL, RB_FINAL) ? rb->get_texture(RB_SCOPE_SSIL, RB_FINAL) : RID();
		RID texture = ssil.is_valid() ? ssil : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 35;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;

		RID ssr;
		if (rb_data.is_valid()) {
			if (rb_data->ss_effects_data.ssr.half_size) {
				if (rb->has_texture(RB_SCOPE_SSR, RB_FINAL)) {
					ssr = rb->get_texture(RB_SCOPE_SSR, RB_FINAL);
				}
			} else {
				if (rb->has_texture(RB_SCOPE_SSR, RB_SSR)) {
					ssr = rb->get_texture(RB_SCOPE_SSR, RB_SSR);
				}
			}
		}

		RID texture = ssr.is_valid() ? ssr : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 36;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;

		RID ssr_mip_level = (rb_data.is_valid() && !rb_data->ss_effects_data.ssr.half_size && rb->has_texture(RB_SCOPE_SSR, RB_MIP_LEVEL)) ? rb->get_texture(RB_SCOPE_SSR, RB_MIP_LEVEL) : RID();
		RID texture = ssr_mip_level.is_valid() ? ssr_mip_level : texture_storage->texture_rd_get_default(is_multiview ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 37;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;

		RID sscs;
		if (rb_data.is_valid()) {
			if (rb->has_texture(RB_SCOPE_SSCS, RB_SSCS)) {
				sscs = rb->get_texture(RB_SCOPE_SSCS, RB_SSCS);
			}
		}

		RID texture = sscs.is_valid() ? sscs : texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
		u.append_id(texture);
		uniforms.push_back(u);
	}
#ifdef MODULE_TEXTURE_STREAMING_ENABLED
	{
		RD::Uniform u;
		u.binding = 38;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		RID instance_buffer = TextureStreaming::get_singleton()->feedback_buffer_get_uniform_rid();
		if (instance_buffer.is_null()) {
			instance_buffer = scene_shader.default_material_feedback_buffer;
		}
		u.append_id(instance_buffer);
		uniforms.push_back(u);
	}
#endif // MODULE_TEXTURE_STREAMING_ENABLED
	{
		// GodotNext: nextgi_alpha_policy (#61). WRC SH clipmap for the translucency cut fallback.
		RendererRD::NextGIWRC *wrc = next_gi.get_wrc(rb.ptr()); // #253: rb is null without render data.
		RID sh_textures[3] = { RID(), RID(), RID() };
		RID wrc_params;
		if (wrc != nullptr && wrc->is_ready()) {
			sh_textures[0] = wrc->get_sh_r();
			sh_textures[1] = wrc->get_sh_g();
			sh_textures[2] = wrc->get_sh_b();
			wrc_params = wrc->get_params_buffer();
		}
		const RID black_3d = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_BLACK);
		for (int i = 0; i < 3; i++) {
			RD::Uniform u;
			u.binding = 39 + i;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.append_id(sh_textures[i].is_valid() ? sh_textures[i] : black_3d);
			uniforms.push_back(u);
		}
		RD::Uniform u;
		u.binding = 42;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.append_id(wrc_params.is_valid() ? wrc_params : scene_shader.default_material_feedback_buffer);
		uniforms.push_back(u);
		RID volume_textures[3] = { RID(), RID(), RID() };
		if (wrc != nullptr && wrc->is_ready()) {
			volume_textures[0] = wrc->get_translucency_r();
			volume_textures[1] = wrc->get_translucency_g();
			volume_textures[2] = wrc->get_translucency_b();
		}
		for (int i = 0; i < 3; i++) {
			RD::Uniform volume_uniform;
			volume_uniform.binding = 43 + i;
			volume_uniform.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			volume_uniform.append_id(volume_textures[i].is_valid() ? volume_textures[i] : black_3d);
			uniforms.push_back(volume_uniform);
		}
	}
	{
		RID opaque_depth = front_layer.get_opaque_depth();
		RID layer_depth = p_front_layer_pass ? RID() : front_layer.get_depth();
		RID layer_reflection = front_layer.get_reflection();
		for (uint32_t i = 0; i < 3; i++) {
			RD::Uniform u;
			u.binding = 46 + i;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			RID texture = i == 0 ? opaque_depth : (i == 1 ? layer_depth : layer_reflection);
			if (texture.is_null()) {
				texture = texture_storage->texture_rd_get_default(i == 2 ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
			}
			u.append_id(p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
			u.append_id(texture);
			uniforms.push_back(u);
		}
	}
	{
		// GodotNext: avboit (#295). The AVBOIT grids the current AVBOIT pass reads; black otherwise.
		RD::Uniform bounds;
		bounds.binding = 49;
		bounds.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		bounds.append_id(avboit_bind_bounds.is_valid() ? avboit_bind_bounds : texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK));
		uniforms.push_back(bounds);
		RD::Uniform occlusion;
		occlusion.binding = 50;
		occlusion.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		occlusion.append_id(avboit_bind_occlusion.is_valid() ? avboit_bind_occlusion : texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK));
		uniforms.push_back(occlusion);
	}
	{
		// GodotNext: vsm_projection (#222). What the light loop reads to sample virtual shadow maps: the pool,
		// the page table, the per-map data and state, the view's params and #54's blue noise. Defaults while
		// VSM is off; a view that does not sample VSM never reads them.
		_ensure_vsm_projection_resources();
		RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
		const bool vsm_ready = _vsm_projection_buffers_ready(vsm);
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 51, vsm_ready ? vsm->get_pool_texture() : vsm_projection_default_pool));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 52, vsm_ready ? vsm->get_page_table_buffer() : vsm_projection_default_buffer));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 53, vsm_ready ? vsm->get_map_data_buffer() : vsm_projection_default_buffer));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 54, vsm_ready ? vsm->get_map_state_buffer() : vsm_projection_default_buffer));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 55, vsm_projection_params_buffer));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 56, vsm_projection_blue_noise));
		// GodotNext: vsm_fog_translucency (#215). Missing-coarse-page atomic counters for transparent coarse lookups.
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 57, vsm_ready ? vsm->get_coarse_miss_buffer() : vsm_projection_default_miss_buffer));
		// GodotNext: df_shadows (#214). Half-res R8 DF shadow mask array and RGBA16F guide (defaults when off).
		_ensure_df_default_textures();
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 58, df_bind_mask.is_valid() ? df_bind_mask : df_default_mask));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 59, df_bind_guide.is_valid() ? df_bind_guide : df_default_guide));
	}
	{
		// GodotNext: light_grid (#139, plan §21.3 contract M2). Packed per-cell light list buffers
		// (default dummy buffer when the packed light grid is inactive).
		const RID default_lg = cluster_builder_shared.get_default_light_grid_buffer();
		const RID lg_headers = (p_render_data && p_render_data->packed_light_grid_headers_buffer.is_valid()) ? p_render_data->packed_light_grid_headers_buffer : default_lg;
		const RID lg_indices = (p_render_data && p_render_data->packed_light_grid_index_buffer.is_valid()) ? p_render_data->packed_light_grid_index_buffer : default_lg;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 60, lg_headers));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 61, lg_indices));
	}
	{
		// GodotNext: light_function_atlas (#138, plan §21.3 contract M8). Bindings 62 and 63 hold the
		// Light Function Atlas texture and per-view LightFunctionEntry SSBO (binding 64 holds #140 IES).
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 62, light_function_atlas.get_atlas_texture_or_default()));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 63, light_function_atlas.get_view_entry_buffer_or_default()));
		// GodotNext: light_source_parity (#140, plan §21.3 contract M8). IES profile Texture2DArray atlas.
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 64, light_storage->get_ies_atlas_texture()));
	}
	{
		// GodotNext: rt_shadows (#141, plan §21.3 contract M9). Tile-sparse denoised R8 RT shadow mask atlas,
		// per-light screen-tile lookup table, and R16F hit-distance atlas (defaults when off or in depth prepass).
		_ensure_rt_shadow_default_resources();
		const bool bind_rt = p_render_data != nullptr && rt_shadows_view && rt_bind_mask.is_valid() && rt_bind_lookup.is_valid() && rt_bind_hit_dist.is_valid();
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 65, bind_rt ? rt_bind_mask : rt_default_mask));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 66, bind_rt ? rt_bind_lookup : rt_default_lookup));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 67, bind_rt ? rt_bind_hit_dist : rt_default_hit_dist));
	}
	{
		// GodotNext: spline_meshes (#161, plan §21.4 contract N12). Per-render-list spline deform SSBO.
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 68, scene_state.get_spline_deform_buffer(p_render_list)));
	}

	// GodotNext: nextgeo_shading_bins (#158)
	if (r_uniforms != nullptr) {
		*r_uniforms = uniforms;
	}
	return UniformSetCacheRD::get_singleton()->get_cache_vec(scene_shader.get_default_shader_rd(is_multiview), RENDER_PASS_UNIFORM_SET, uniforms);
}

RID RenderForwardClustered::_setup_sdfgi_render_pass_uniform_set(RID p_albedo_texture, RID p_emission_texture, RID p_emission_aniso_texture, RID p_geom_facing_texture, const RendererRD::MaterialStorage::Samplers &p_samplers, uint32_t p_uniform_buffer_index) {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	thread_local LocalVector<RD::Uniform> uniforms;
	uniforms.clear();

	{
		RD::Uniform u;
		u.binding = 0;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 1;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.implementation_uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 2;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC;
		if (scene_state.instance_buffer[RENDER_LIST_SECONDARY].get_size(0u) == 0u) {
			// Any buffer will do since it's not used, so just create one.
			// We can't use scene_shader.default_vec4_xform_buffer because it's not dynamic.
			scene_state.instance_buffer[RENDER_LIST_SECONDARY].set_storage_size(0u, INSTANCE_DATA_BUFFER_MIN_SIZE * sizeof(SceneState::InstanceData));
			scene_state.instance_buffer[RENDER_LIST_SECONDARY].prepare_for_upload();
		}
		RID instance_buffer = scene_state.instance_buffer[RENDER_LIST_SECONDARY]._get(0u);
		u.append_id(instance_buffer);
		uniforms.push_back(u);
	}
	{
		// No radiance texture.
		RID radiance_texture = texture_storage->texture_rd_get_default(is_using_radiance_octmap_array() ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		RD::Uniform u;
		u.binding = 3;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(radiance_texture);
		uniforms.push_back(u);
	}

	{
		// No reflection atlas.
		RID ref_texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
		RD::Uniform u;
		u.binding = 4;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(ref_texture);
		uniforms.push_back(u);
	}

	{
		// No shadow atlas.
		RD::Uniform u;
		u.binding = 5;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		// No directional shadow atlas.
		RD::Uniform u;
		u.binding = 6;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		u.append_id(texture);
		uniforms.push_back(u);
	}

	{
		// No Lightmaps
		RD::Uniform u;
		u.binding = 7;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;

		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);
		for (uint32_t i = 0; i < scene_state.max_lightmaps * 2; i++) {
			u.append_id(default_tex);
		}

		uniforms.push_back(u);
	}

	{
		// No VoxelGIs
		RD::Uniform u;
		u.binding = 8;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;

		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		for (int i = 0; i < MAX_VOXEL_GI_INSTANCESS; i++) {
			u.append_id(default_tex);
		}

		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 9;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		RID cb = scene_shader.default_vec4_xform_buffer;
		u.append_id(cb);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 10;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (decals_get_filter()) {
			case RSE::DECAL_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}

		u.append_id(sampler);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 11;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (light_projectors_get_filter()) {
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}

		u.append_id(sampler);
		uniforms.push_back(u);
	}

	p_samplers.append_uniforms(uniforms, 12);

	// actual sdfgi stuff

	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 24;
		u.append_id(p_albedo_texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 25;
		u.append_id(p_emission_texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 26;
		u.append_id(p_emission_aniso_texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 27;
		u.append_id(p_geom_facing_texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 38;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		RID instance_buffer = scene_shader.default_material_feedback_buffer;
		u.append_id(instance_buffer);
		uniforms.push_back(u);
	}
	{
		const RID black_3d = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_BLACK);
		for (int i = 0; i < 3; i++) {
			RD::Uniform u;
			u.binding = 39 + i;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.append_id(black_3d);
			uniforms.push_back(u);
		}
		RD::Uniform u;
		u.binding = 42;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.append_id(scene_shader.default_material_feedback_buffer);
		uniforms.push_back(u);
		for (int i = 0; i < 3; i++) {
			RD::Uniform volume_uniform;
			volume_uniform.binding = 43 + i;
			volume_uniform.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			volume_uniform.append_id(black_3d);
			uniforms.push_back(volume_uniform);
		}
		const RID nearest_sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		for (uint32_t i = 0; i < 3; i++) {
			RD::Uniform layer_uniform;
			layer_uniform.binding = 46 + i;
			layer_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			layer_uniform.append_id(nearest_sampler);
			layer_uniform.append_id(texture_storage->texture_rd_get_default(i == 2 ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH));
			uniforms.push_back(layer_uniform);
		}
		// GodotNext: avboit (#295). The SDF pass reads no AVBOIT grid.
		RD::Uniform avboit_bounds;
		avboit_bounds.binding = 49;
		avboit_bounds.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		avboit_bounds.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK));
		uniforms.push_back(avboit_bounds);
		RD::Uniform avboit_occlusion;
		avboit_occlusion.binding = 50;
		avboit_occlusion.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		avboit_occlusion.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK));
		uniforms.push_back(avboit_occlusion);
		// GodotNext: spline_meshes (#161).
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 68, scene_state.get_spline_deform_buffer(RENDER_LIST_SECONDARY)));
	}

	if (scene_shader.default_shader_sdfgi_rd.is_null()) {
		// The variant for SDF from the default material should only be retrieved when SDFGI is required.
		ERR_FAIL_NULL_V(scene_shader.default_material_shader_ptr, RID());
		scene_shader.enable_advanced_shader_group();
		scene_shader.default_shader_sdfgi_rd = scene_shader.default_material_shader_ptr->get_shader_variant(SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_SDF, 0, true);
		ERR_FAIL_COND_V(scene_shader.default_shader_sdfgi_rd.is_null(), RID());
	}

	return UniformSetCacheRD::get_singleton()->get_cache_vec(scene_shader.default_shader_sdfgi_rd, RENDER_PASS_UNIFORM_SET, uniforms);
}

RID RenderForwardClustered::_setup_vsm_render_pass_uniform_set(const RendererRD::MaterialStorage::Samplers &p_samplers, uint32_t p_uniform_buffer_index) {
	// GodotNext: vsm_non_nanite (#218). Mirrors _setup_sdfgi_render_pass_uniform_set with MODE_VSM's Set 1 bindings 24-30.
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	RendererRD::VirtualShadowMapStorage *vsm = RendererRD::VirtualShadowMapStorage::get_singleton();
	ERR_FAIL_NULL_V(vsm, RID());

	thread_local LocalVector<RD::Uniform> uniforms;
	uniforms.clear();

	{
		RD::Uniform u;
		u.binding = 0;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 1;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(scene_state.implementation_uniform_buffers[p_uniform_buffer_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 2;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC;
		if (scene_state.instance_buffer[RENDER_LIST_SECONDARY].get_size(0u) == 0u) {
			scene_state.instance_buffer[RENDER_LIST_SECONDARY].set_storage_size(0u, INSTANCE_DATA_BUFFER_MIN_SIZE * sizeof(SceneState::InstanceData));
			scene_state.instance_buffer[RENDER_LIST_SECONDARY].prepare_for_upload();
		}
		u.append_id(scene_state.instance_buffer[RENDER_LIST_SECONDARY]._get(0u));
		uniforms.push_back(u);
	}
	{
		RID radiance_texture = texture_storage->texture_rd_get_default(is_using_radiance_octmap_array() ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		RD::Uniform u;
		u.binding = 3;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(radiance_texture);
		uniforms.push_back(u);
	}
	{
		RID ref_texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
		RD::Uniform u;
		u.binding = 4;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(ref_texture);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 5;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH));
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 6;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH));
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 7;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);
		for (uint32_t i = 0; i < scene_state.max_lightmaps * 2; i++) {
			u.append_id(default_tex);
		}
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 8;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		RID default_tex = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		for (int i = 0; i < MAX_VOXEL_GI_INSTANCESS; i++) {
			u.append_id(default_tex);
		}
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 9;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.append_id(scene_shader.default_vec4_xform_buffer);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 10;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (decals_get_filter()) {
			case RSE::DECAL_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}
		u.append_id(sampler);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 11;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		RID sampler;
		switch (light_projectors_get_filter()) {
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}
		u.append_id(sampler);
		uniforms.push_back(u);
	}

	p_samplers.append_uniforms(uniforms, 12);

	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 24, vsm->get_pool_texture()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, vsm_group_offsets_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 26, vsm_pairs_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, vsm->get_dirty_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, vsm->get_map_data_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, vsm_pair_texels_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 30, vsm_pair_boxes_buffer));

	{
		RD::Uniform u;
		u.binding = 38;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.append_id(scene_shader.default_material_feedback_buffer);
		uniforms.push_back(u);
	}
	{
		const RID black_3d = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_BLACK);
		for (int i = 0; i < 3; i++) {
			RD::Uniform u;
			u.binding = 39 + i;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.append_id(black_3d);
			uniforms.push_back(u);
		}
		RD::Uniform u;
		u.binding = 42;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.append_id(scene_shader.default_material_feedback_buffer);
		uniforms.push_back(u);
		for (int i = 0; i < 3; i++) {
			RD::Uniform volume_uniform;
			volume_uniform.binding = 43 + i;
			volume_uniform.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			volume_uniform.append_id(black_3d);
			uniforms.push_back(volume_uniform);
		}
		const RID nearest_sampler = p_samplers.get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		for (uint32_t i = 0; i < 3; i++) {
			RD::Uniform layer_uniform;
			layer_uniform.binding = 46 + i;
			layer_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			layer_uniform.append_id(nearest_sampler);
			layer_uniform.append_id(texture_storage->texture_rd_get_default(i == 2 ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH));
			uniforms.push_back(layer_uniform);
		}
		// GodotNext: avboit (#295). The VSM pass reads no AVBOIT grid.
		RD::Uniform avboit_bounds;
		avboit_bounds.binding = 49;
		avboit_bounds.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		avboit_bounds.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK));
		uniforms.push_back(avboit_bounds);
		RD::Uniform avboit_occlusion;
		avboit_occlusion.binding = 50;
		avboit_occlusion.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		avboit_occlusion.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK));
		uniforms.push_back(avboit_occlusion);
		// GodotNext: spline_meshes (#161). Non-Nanite VSM depth raster deforms spline instances in-place.
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 68, scene_state.get_spline_deform_buffer(RENDER_LIST_SECONDARY)));
	}

	if (scene_shader.default_shader_vsm_rd.is_null()) {
		ERR_FAIL_NULL_V(scene_shader.default_material_shader_ptr, RID());
		scene_shader.enable_vsm_shader_group();
		scene_shader.default_shader_vsm_rd = scene_shader.default_material_shader_ptr->get_shader_variant(SceneShaderForwardClustered::PIPELINE_VERSION_VSM_DEPTH, 0, true);
		ERR_FAIL_COND_V(scene_shader.default_shader_vsm_rd.is_null(), RID());
	}

	return UniformSetCacheRD::get_singleton()->get_cache_vec(scene_shader.default_shader_vsm_rd, RENDER_PASS_UNIFORM_SET, uniforms);
}

RID RenderForwardClustered::_render_buffers_get_normal_texture(Ref<RenderSceneBuffersRD> p_render_buffers) {
	Ref<RenderBufferDataForwardClustered> rb_data = p_render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);

	return rb_data->get_normal_roughness();
}

RID RenderForwardClustered::_render_buffers_get_velocity_texture(Ref<RenderSceneBuffersRD> p_render_buffers) {
	return p_render_buffers->get_velocity_buffer(false);
}

void RenderForwardClustered::environment_set_ssao_quality(RSE::EnvironmentSSAOQuality p_quality, bool p_half_size, float p_adaptive_target, int p_blur_passes, float p_fadeout_from, float p_fadeout_to) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_quality < RSE::EnvironmentSSAOQuality::ENV_SSAO_QUALITY_VERY_LOW || p_quality > RSE::EnvironmentSSAOQuality::ENV_SSAO_QUALITY_ULTRA);
	ss_effects->ssao_set_quality(p_quality, p_half_size, p_adaptive_target, p_blur_passes, p_fadeout_from, p_fadeout_to);
}

void RenderForwardClustered::environment_set_ssil_quality(RSE::EnvironmentSSILQuality p_quality, bool p_half_size, float p_adaptive_target, int p_blur_passes, float p_fadeout_from, float p_fadeout_to) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_quality < RSE::EnvironmentSSILQuality::ENV_SSIL_QUALITY_VERY_LOW || p_quality > RSE::EnvironmentSSILQuality::ENV_SSIL_QUALITY_ULTRA);
	ss_effects->ssil_set_quality(p_quality, p_half_size, p_adaptive_target, p_blur_passes, p_fadeout_from, p_fadeout_to);
}

void RenderForwardClustered::environment_set_ssr_half_size(bool p_half_size) {
	ERR_FAIL_NULL(ss_effects);
	ss_effects->ssr_set_half_size(p_half_size);
}

void RenderForwardClustered::environment_set_ssr_roughness_quality(RSE::EnvironmentSSRRoughnessQuality p_quality) {
	WARN_PRINT_ONCE("environment_set_ssr_roughness_quality has been deprecated and no longer does anything.");
}

void RenderForwardClustered::sub_surface_scattering_set_quality(RSE::SubSurfaceScatteringQuality p_quality) {
	ERR_FAIL_NULL(ss_effects);
	ERR_FAIL_COND(p_quality < RSE::SubSurfaceScatteringQuality::SUB_SURFACE_SCATTERING_QUALITY_DISABLED || p_quality > RSE::SubSurfaceScatteringQuality::SUB_SURFACE_SCATTERING_QUALITY_HIGH);
	ss_effects->sss_set_quality(p_quality);
}

void RenderForwardClustered::sub_surface_scattering_set_scale(float p_scale, float p_depth_scale) {
	ERR_FAIL_NULL(ss_effects);
	ss_effects->sss_set_scale(p_scale, p_depth_scale);
}

RenderForwardClustered *RenderForwardClustered::singleton = nullptr;

void RenderForwardClustered::sdfgi_update(const Ref<RenderSceneBuffers> &p_render_buffers, RID p_environment, const Vector3 &p_world_position) {
	Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	ERR_FAIL_COND(rb.is_null());
	Ref<RendererRD::GI::SDFGI> sdfgi;
	if (rb->has_custom_data(RB_SCOPE_SDFGI)) {
		sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
	}

	bool needs_sdfgi = p_environment.is_valid() && environment_get_sdfgi_enabled(p_environment);
	bool needs_reset = sdfgi.is_valid() ? sdfgi->version != gi.sdfgi_current_version : false;

	if (!needs_sdfgi || needs_reset) {
		if (sdfgi.is_valid()) {
			// delete it
			sdfgi.unref();
			rb->set_custom_data(RB_SCOPE_SDFGI, sdfgi);
		}

		if (!needs_sdfgi) {
			return;
		}
	}

	// Ensure advanced shaders are available if SDFGI is used.
	// Call here as this is the first entry point for SDFGI.
	scene_shader.enable_advanced_shader_group();

	static const uint32_t history_frames_to_converge[RSE::ENV_SDFGI_CONVERGE_MAX] = { 5, 10, 15, 20, 25, 30 };
	uint32_t requested_history_size = history_frames_to_converge[gi.sdfgi_frames_to_converge];

	if (sdfgi.is_valid() && (sdfgi->num_cascades != environment_get_sdfgi_cascades(p_environment) || sdfgi->min_cell_size != environment_get_sdfgi_min_cell_size(p_environment) || requested_history_size != sdfgi->history_size || sdfgi->uses_occlusion != environment_get_sdfgi_use_occlusion(p_environment) || sdfgi->y_scale_mode != environment_get_sdfgi_y_scale(p_environment))) {
		//configuration changed, erase
		sdfgi.unref();
		rb->set_custom_data(RB_SCOPE_SDFGI, sdfgi);
	}

	if (sdfgi.is_null()) {
		// re-create
		sdfgi = gi.create_sdfgi(p_environment, p_world_position, requested_history_size);
		rb->set_custom_data(RB_SCOPE_SDFGI, sdfgi);
	} else {
		//check for updates
		sdfgi->update(p_environment, p_world_position);
	}
}

int RenderForwardClustered::sdfgi_get_pending_region_count(const Ref<RenderSceneBuffers> &p_render_buffers) const {
	Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	ERR_FAIL_COND_V(rb.is_null(), 0);

	if (!rb->has_custom_data(RB_SCOPE_SDFGI)) {
		return 0;
	}
	Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);

	int dirty_count = 0;
	for (const RendererRD::GI::SDFGI::Cascade &c : sdfgi->cascades) {
		if (c.dirty_regions == RendererRD::GI::SDFGI::Cascade::DIRTY_ALL) {
			dirty_count++;
		} else {
			for (int j = 0; j < 3; j++) {
				if (c.dirty_regions[j] != 0) {
					dirty_count++;
				}
			}
		}
	}

	return dirty_count;
}

AABB RenderForwardClustered::sdfgi_get_pending_region_bounds(const Ref<RenderSceneBuffers> &p_render_buffers, int p_region) const {
	AABB bounds;
	Vector3i from;
	Vector3i size;

	Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	ERR_FAIL_COND_V(rb.is_null(), AABB());
	Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
	ERR_FAIL_COND_V(sdfgi.is_null(), AABB());

	int c = sdfgi->get_pending_region_data(p_region, from, size, bounds);
	ERR_FAIL_COND_V(c == -1, AABB());
	return bounds;
}

uint32_t RenderForwardClustered::sdfgi_get_pending_region_cascade(const Ref<RenderSceneBuffers> &p_render_buffers, int p_region) const {
	AABB bounds;
	Vector3i from;
	Vector3i size;

	Ref<RenderSceneBuffersRD> rb = p_render_buffers;
	ERR_FAIL_COND_V(rb.is_null(), -1);
	Ref<RendererRD::GI::SDFGI> sdfgi = rb->get_custom_data(RB_SCOPE_SDFGI);
	ERR_FAIL_COND_V(sdfgi.is_null(), -1);

	return sdfgi->get_pending_region_data(p_region, from, size, bounds);
}

void RenderForwardClustered::GeometryInstanceForwardClustered::_mark_dirty() {
	if (dirty_list_element.in_list()) {
		return;
	}

	//clear surface caches
	GeometryInstanceSurfaceDataCache *surf = surface_caches;

	while (surf) {
		GeometryInstanceSurfaceDataCache *next = surf->next;
		// GodotNext: nextgeo_raster_bins (#155). The surface's reference to its programmable raster bin.
		if (surf->nextgeo_raster_bin != 0u && RSG::virtual_mesh_storage != nullptr) {
			RSG::virtual_mesh_storage->raster_bin_release(surf->nextgeo_raster_bin);
		}
		RenderForwardClustered::get_singleton()->geometry_instance_surface_alloc.free(surf);
		surf = next;
	}

	surface_caches = nullptr;

	RenderForwardClustered::get_singleton()->geometry_instance_dirty_list.add(&dirty_list_element);
}

void RenderForwardClustered::_update_global_pipeline_data_requirements_from_project() {
	const int msaa_3d_mode = GLOBAL_GET_CACHED(int, "rendering/anti_aliasing/quality/msaa_3d");
	const bool directional_shadow_16_bits = GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/directional_shadow/16_bits");
	const bool positional_shadow_16_bits = GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/positional_shadow/atlas_16_bits");
	global_pipeline_data_required.use_16_bit_shadows = directional_shadow_16_bits || positional_shadow_16_bits;
	global_pipeline_data_required.use_32_bit_shadows = !directional_shadow_16_bits || !positional_shadow_16_bits;
	global_pipeline_data_required.texture_samples = RenderSceneBuffersRD::msaa_to_samples(RSE::ViewportMSAA(msaa_3d_mode));
}

void RenderForwardClustered::_update_global_pipeline_data_requirements_from_light_storage() {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	global_pipeline_data_required.use_shadow_cubemaps = light_storage->get_shadow_cubemaps_used();
	global_pipeline_data_required.use_shadow_dual_paraboloid = light_storage->get_shadow_dual_paraboloid_used();
}

void RenderForwardClustered::_geometry_instance_add_surface_with_material(GeometryInstanceForwardClustered *ginstance, uint32_t p_surface, SceneShaderForwardClustered::MaterialData *p_material, uint32_t p_material_id, uint32_t p_shader_id, RID p_mesh, bool p_chain_blocked, uint32_t p_bucket_offset, bool p_assembly_part) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	uint32_t flags = 0;

	if (p_material->shader_data->uses_sss) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_SUBSURFACE_SCATTERING;
		global_surface_data.sss_used = true;
	}

	if (p_material->shader_data->uses_screen_texture) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_SCREEN_TEXTURE;
		global_surface_data.screen_texture_used = true;
	}

	if (p_material->shader_data->uses_depth_texture) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_DEPTH_TEXTURE;
		global_surface_data.depth_texture_used = true;
	}

	if (p_material->shader_data->uses_normal_texture) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_NORMAL_TEXTURE;
		global_surface_data.normal_texture_used = true;
	}

	if (ginstance->data->cast_double_sided_shadows) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_DOUBLE_SIDED_SHADOWS;
	}

	if (p_material->shader_data->stencil_enabled) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_STENCIL;
	}

	if (p_material->shader_data->uses_alpha_pass()) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA;
		if (p_material->shader_data->uses_depth_in_alpha_pass()) {
			flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH;
			flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW;
		}
	} else {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE;
		flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH;
		flags |= GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW;
	}

	if (p_material->shader_data->uses_particle_trails) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_PARTICLE_TRAILS;
	}

	if (p_material->shader_data->is_animated()) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_MOTION_VECTOR;
	}

	if (p_material->shader_data->stencil_enabled) {
		if (p_material->shader_data->stencil_flags & SceneShaderForwardClustered::ShaderData::STENCIL_FLAG_READ) {
			// Stencil materials which read from the stencil buffer must be in the alpha pass.
			// This is critical to preserve compatibility once we'll have the compositor.
			if (!(flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA)) {
				String shader_path = p_material->shader_data->path.is_empty() ? "" : "(" + p_material->shader_data->path + ")";
				ERR_PRINT_ED(vformat("Attempting to use a shader %s that reads stencil but is not in the alpha queue. Ensure the material uses alpha blending or has depth_draw disabled or depth_test disabled.", shader_path));
			}
		}
	}

	SceneShaderForwardClustered::MaterialData *material_shadow = nullptr;
	void *surface_shadow = nullptr;
	if (p_material->shader_data->uses_shared_shadow_material()) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_USES_SHARED_SHADOW_MATERIAL;
		material_shadow = static_cast<SceneShaderForwardClustered::MaterialData *>(RendererRD::MaterialStorage::get_singleton()->material_get_data(scene_shader.default_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));

		RID shadow_mesh = mesh_storage->mesh_get_shadow_mesh(p_mesh);
		if (shadow_mesh.is_valid()) {
			surface_shadow = mesh_storage->mesh_get_surface(shadow_mesh, p_surface);
		}
	} else {
		material_shadow = p_material;
	}

	// GodotNext: nextgeo_exclusive_dispatch (#79) / nextgeo_resolve (#80) / nextgeo_foliage_instancing (#151).
	// Mark the surface as a virtual-geometry candidate here; the view-level
	// gates (MSAA, multiview, debug draw, draw_ready) are applied per view.
	RendererRD::NextGeoSurfaceFacts route_facts;
	route_facts.instance_mesh = ginstance->data->base_type == RSE::INSTANCE_MESH ||
			(ginstance->data->base_type == RSE::INSTANCE_MULTIMESH && ginstance->nextgeo_record_index != UINT32_MAX);
	if (route_facts.instance_mesh && RSG::virtual_mesh_storage != nullptr) {
		route_facts.mesh_has_virtual_geometry = RSG::virtual_mesh_storage->mesh_has_virtual_geometry(p_mesh);
		route_facts.surface_in_mask = route_facts.mesh_has_virtual_geometry && RSG::virtual_mesh_storage->mesh_surface_is_virtual(p_mesh, p_surface);
		const uint32_t clear_reason = RSG::virtual_mesh_storage->mesh_clear_reason(p_mesh);
		route_facts.mutated = clear_reason == RenderingServerTypes::VIRTUAL_GEOMETRY_CLEAR_MUTATED_MESH;
		route_facts.baked_gridmap = clear_reason == RenderingServerTypes::VIRTUAL_GEOMETRY_CLEAR_BAKED_GRIDMAP;
	}
	route_facts.skeleton = ginstance->data->skeleton.is_valid();
	route_facts.blend_shapes = mesh_storage->mesh_get_blend_shape_count(p_mesh) > 0;
	// GodotNext: nextgeo_skinned_runtime (#160, plan §21.4 contract N8). Admit skinned virtual geometry
	// when the mesh carries #159 cluster skinning data and a valid 3D skeleton is attached.
	if (route_facts.mesh_has_virtual_geometry && route_facts.skeleton && RSG::virtual_mesh_storage != nullptr) {
		route_facts.mesh_skinned_virtual_geometry = RSG::virtual_mesh_storage->mesh_is_skinned_virtual_geometry(p_mesh) &&
				mesh_storage->skeleton_get_bone_count(ginstance->data->skeleton) > 0 &&
				!mesh_storage->skeleton_is_2d(ginstance->data->skeleton);
	}
	route_facts.instance_uniforms = ginstance->shader_uniforms_offset >= 0;
	route_facts.lightmapped = ginstance->lightmap_instance.is_valid();
	route_facts.chain_blocked = p_chain_blocked;
	route_facts.material_fallback = RendererRD::NextGeoFallback(p_material->shader_data->get_nextgeo_fallback());
	// GodotNext: nextgeo_displacement (#150). Combine shader features with material displacement_magnitude > 0.
	const uint32_t material_raster_features = p_material->get_raster_features();
	const bool material_displaced = (material_raster_features & RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) != 0u;
	const uint32_t material_raster_rejects = RendererRD::nextgeo_raster_rejects(p_material->shader_data->raster_uses, material_raster_features);
	if (material_displaced && material_raster_rejects != 0u && route_facts.material_fallback == RendererRD::NEXTGEO_FALLBACK_NONE) {
		route_facts.material_fallback = RendererRD::NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE;
	}
	// GodotNext: nextgeo_masked_two_sided (#153), depth_offset (#149), nextgeo_vertex_offset (#164), nextgeo_tessellation (#163), nextgeo_raster_bins (#155, plan §21.4 N3, N5, N6, N11).
	// Pixel-programmable surfaces (masked: alpha scissor, alpha hash, fragment discard; depth offset: DEPTH_OFFSET)
	// and vertex-offset (VERTEX write) surfaces route into programmable raster bins by default on the Nanite
	// device tier. Displacement (#163) routes into programmable bins when rendering/nextgeo/experimental/tessellation
	// is enabled (or debug_programmable_raster).
	bool admit_programmable = false;
	const bool default_prog_admitted = (material_raster_features & (RendererVirtualMeshStorage::RASTER_PIXEL_PROGRAMMABLE_FEATURES | RendererVirtualMeshStorage::RASTER_FEATURE_VERTEX_OFFSET)) != 0u &&
			p_material->shader_data->nextgeo_masked_fallback == RendererRD::NEXTGEO_FALLBACK_NONE;
	const bool tess_admitted = material_displaced &&
			p_material->shader_data->nextgeo_programmable_fallback == RendererRD::NEXTGEO_FALLBACK_NONE &&
			bool(GLOBAL_GET("rendering/nextgeo/experimental/tessellation"));
	const bool wants_debug_programmable = (route_facts.material_fallback != RendererRD::NEXTGEO_FALLBACK_NONE) || material_displaced;
	const bool debug_admitted = wants_debug_programmable &&
			p_material->shader_data->nextgeo_programmable_fallback == RendererRD::NEXTGEO_FALLBACK_NONE &&
			bool(GLOBAL_GET("rendering/nextgeo/debug_programmable_raster"));
	if ((default_prog_admitted || tess_admitted || debug_admitted) && material_raster_rejects == 0u &&
			p_material->shader_data->has_raster_target() && route_facts.mesh_has_virtual_geometry &&
			RSG::virtual_mesh_storage != nullptr && nextgeo_cut.is_hybrid_raster_available() &&
			p_material->shader_data->ensure_raster_target()) {
		admit_programmable = true;
		route_facts.material_fallback = RendererRD::NEXTGEO_FALLBACK_NONE;
	}
	if (route_facts.material_fallback == RendererRD::NEXTGEO_FALLBACK_NONE &&
			(ginstance->fade_near || ginstance->fade_far || (ginstance->force_alpha * ginstance->parent_fade_alpha) < FADE_ALPHA_PASS_THRESHOLD)) {
		route_facts.material_fallback = RendererRD::NEXTGEO_FALLBACK_ALPHA;
	}
	// Eligibility is recorded regardless of the draw-ready gate; the gate is
	// applied per view so caches built before draw-ready still route once set.
	route_facts.draw_ready = true;
	RendererRD::NextGeoSurfaceRoute route = RendererRD::nextgeo_resolve_surface_route(route_facts);
	// GodotNext: nextgeo_raster_bins (#155). An admitted surface holds a reference to its bin; with every
	// bin taken it keeps the material's default route.
	uint32_t raster_bin = 0;
	if (route.virtual_geometry && admit_programmable) {
		RendererVirtualMeshStorage::RasterBinKey bin_key;
		bin_key.features = material_raster_features | (ginstance->spline_deform_active ? uint32_t(RendererVirtualMeshStorage::RASTER_FEATURE_SPLINE) : 0u) | ((route_facts.skeleton && route_facts.mesh_skinned_virtual_geometry) ? uint32_t(RendererVirtualMeshStorage::RASTER_FEATURE_SKINNED) : 0u);
		bin_key.shader_version = p_material->shader_data->version;
		bin_key.material = p_material->get_self();
		raster_bin = RSG::virtual_mesh_storage->raster_bin_acquire(bin_key);
		if (raster_bin == 0u) {
			// GodotNext: nextgeo_displacement (#150). Displaced-only materials keep their virtual
			// fixed-function bin when bins are full; masked and vertex-offset materials return to legacy.
			RendererRD::nextgeo_raster_bin_apply_exhausted(route, RendererRD::NextGeoFallback(p_material->shader_data->get_nextgeo_fallback()));
			if (!nextgeo_raster_bins_exhausted_warned) {
				nextgeo_raster_bins_exhausted_warned = true;
				WARN_PRINT(vformat("NextGeo: all %d programmable raster bins are in use; further masked, vertex-offset, and displaced materials keep their default route.", int(RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX)));
			}
		}
	}
	// N4: one line per material shader the raster target rejects.
	const uint32_t effective_raster_rejects = p_material->shader_data->raster_rejects | material_raster_rejects;
	if (route.reason == RendererRD::NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE && !p_material->shader_data->nextgeo_raster_reject_logged) {
		p_material->shader_data->nextgeo_raster_reject_logged = true;
		const String shader_name = p_material->shader_data->path.is_empty() ? vformat("<built-in>:%d", p_material->shader_data->code.hash()) : p_material->shader_data->path;
		print_line(vformat("NextGeo: material shader \"%s\" stays on legacy raster (UNSUPPORTED_RASTER_FEATURE: %s).", shader_name, RendererRD::nextgeo_raster_reject_names(effective_raster_rejects)));
	}
	if (route.virtual_geometry) {
		flags |= GeometryInstanceSurfaceDataCache::FLAG_VIRTUAL;
	}

	GeometryInstanceSurfaceDataCache *sdcache = geometry_instance_surface_alloc.alloc();

	sdcache->flags = flags;
	sdcache->nextgeo_fallback_reason = route.reason;
	sdcache->nextgeo_raster_bin = raster_bin;
	sdcache->mesh_has_virtual_geometry = RSG::virtual_mesh_storage != nullptr && RSG::virtual_mesh_storage->mesh_has_virtual_geometry(p_mesh);

	sdcache->shader = p_material->shader_data;
	sdcache->material = p_material;
	sdcache->material_uniform_set = p_material->uniform_set;
	sdcache->surface = mesh_storage->mesh_get_surface(p_mesh, p_surface);
	sdcache->surface_proxy = (!route_facts.skeleton && ginstance->mesh_instance.is_null() && sdcache->mesh_has_virtual_geometry) ? mesh_storage->mesh_get_virtual_proxy_surface(p_mesh, p_surface) : nullptr;
	sdcache->surface_rt_proxy = nullptr;
	sdcache->primitive = mesh_storage->mesh_surface_get_primitive(sdcache->surface);
	sdcache->surface_index = p_surface;
	sdcache->nextgeo_bucket_offset = p_bucket_offset;
	sdcache->nextgeo_assembly_part = p_assembly_part;

	if (ginstance->data->dirty_dependencies) {
		RSG::utilities->base_update_dependency(p_mesh, &ginstance->data->dependency_tracker);
	}

	//shadow
	sdcache->shader_shadow = material_shadow->shader_data;
	sdcache->material_uniform_set_shadow = material_shadow->uniform_set;

	sdcache->surface_shadow = surface_shadow ? surface_shadow : sdcache->surface;

	sdcache->owner = ginstance;

	sdcache->next = ginstance->surface_caches;
	ginstance->surface_caches = sdcache;

	//sortkey

	sdcache->sort.sort_key1 = 0;
	sdcache->sort.sort_key2 = 0;

	sdcache->sort.surface_index = p_surface;
	sdcache->sort.material_id_hi = (p_material_id & 0xFF000000) >> 24;
	sdcache->sort.material_id_lo = (p_material_id & 0x00FFFFFF);
	sdcache->sort.shader_id = p_shader_id;
	sdcache->sort.geometry_id = p_mesh.get_local_index(); //only meshes can repeat anyway
	sdcache->sort.uses_forward_gi = ginstance->can_sdfgi;
	sdcache->sort.priority = p_material->priority;
	sdcache->sort.uses_projector = ginstance->using_projectors;
	sdcache->sort.uses_softshadow = ginstance->using_softshadows;

	uint64_t format = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_format(sdcache->surface);
	if (p_material->shader_data->uses_tangent && !p_material->shader_data->writes_tangent && !(format & RSE::ARRAY_FORMAT_TANGENT)) {
		String shader_path = p_material->shader_data->path.is_empty() ? "" : "(" + p_material->shader_data->path + ")";
		String mesh_path = mesh_storage->mesh_get_path(p_mesh).is_empty() ? "" : "(" + mesh_storage->mesh_get_path(p_mesh) + ")";
		WARN_PRINT_ED(vformat("Attempting to use a shader %s that requires tangents with a mesh %s that doesn't contain tangents. Ensure that meshes are imported with the 'ensure_tangents' option. If creating your own meshes, add an `ARRAY_TANGENT` array (when using ArrayMesh) or call `generate_tangents()` (when using SurfaceTool).", shader_path, mesh_path));
	}

#if PRELOAD_PIPELINES_ON_SURFACE_CACHE_CONSTRUCTION
	if (!sdcache->compilation_dirty_element.in_list()) {
		geometry_surface_compilation_dirty_list.add(&sdcache->compilation_dirty_element);
	}

	if (!sdcache->compilation_all_element.in_list()) {
		geometry_surface_compilation_all_list.add(&sdcache->compilation_all_element);
	}
#endif
}

void RenderForwardClustered::_geometry_instance_add_surface_with_material_chain(GeometryInstanceForwardClustered *ginstance, uint32_t p_surface, SceneShaderForwardClustered::MaterialData *p_material, RID p_mat_src, RID p_mesh, bool p_overlay, uint32_t p_bucket_offset, bool p_assembly_part) {
	SceneShaderForwardClustered::MaterialData *material = p_material;
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();

	// A valid next pass, a valid material overlay or stencil anywhere in the
	// chain clears virtual routing for the whole chain, base surface included.
	bool chain_blocked = p_overlay;
	for (SceneShaderForwardClustered::MaterialData *walk = material; walk != nullptr;) {
		if (walk->shader_data->stencil_enabled) {
			chain_blocked = true;
		}
		if (walk->next_pass.is_null()) {
			break;
		}
		SceneShaderForwardClustered::MaterialData *next = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(walk->next_pass, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		if (next == nullptr || !next->shader_data->is_valid()) {
			break;
		}
		chain_blocked = true;
		walk = next;
	}

	_geometry_instance_add_surface_with_material(ginstance, p_surface, material, p_mat_src.get_local_index(), material_storage->material_get_shader_id(p_mat_src), p_mesh, chain_blocked, p_bucket_offset, p_assembly_part);

	while (material->next_pass.is_valid()) {
		RID next_pass = material->next_pass;
		material = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(next_pass, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		if (!material || !material->shader_data->is_valid()) {
			break;
		}
		if (ginstance->data->dirty_dependencies) {
			material_storage->material_update_dependency(next_pass, &ginstance->data->dependency_tracker);
		}
		_geometry_instance_add_surface_with_material(ginstance, p_surface, material, next_pass.get_local_index(), material_storage->material_get_shader_id(next_pass), p_mesh, chain_blocked, p_bucket_offset, p_assembly_part);
	}
}

void RenderForwardClustered::_geometry_instance_add_surface(GeometryInstanceForwardClustered *ginstance, uint32_t p_surface, RID p_material, RID p_mesh, uint32_t p_bucket_offset, bool p_assembly_part) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RID m_src;

	m_src = ginstance->data->material_override.is_valid() ? ginstance->data->material_override : p_material;

	SceneShaderForwardClustered::MaterialData *material = nullptr;

	if (m_src.is_valid()) {
		material = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(m_src, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		if (!material || !material->shader_data->is_valid()) {
			material = nullptr;
		}
	}

	if (material) {
		if (ginstance->data->dirty_dependencies) {
			material_storage->material_update_dependency(m_src, &ginstance->data->dependency_tracker);
		}
	} else {
		material = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(scene_shader.default_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		m_src = scene_shader.default_material;
	}

	ERR_FAIL_NULL(material);

	// The overlay is resolved before the base chain so a valid overlay blocks
	// routing for the base surface too.
	bool overlay_valid = false;
	if (ginstance->data->material_overlay.is_valid()) {
		SceneShaderForwardClustered::MaterialData *overlay = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(ginstance->data->material_overlay, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		overlay_valid = overlay != nullptr && overlay->shader_data->is_valid();
	}

	_geometry_instance_add_surface_with_material_chain(ginstance, p_surface, material, m_src, p_mesh, overlay_valid, p_bucket_offset, p_assembly_part);

	if (ginstance->data->material_overlay.is_valid()) {
		m_src = ginstance->data->material_overlay;

		material = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(m_src, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		if (material && material->shader_data->is_valid()) {
			if (ginstance->data->dirty_dependencies) {
				material_storage->material_update_dependency(m_src, &ginstance->data->dependency_tracker);
			}

			_geometry_instance_add_surface_with_material_chain(ginstance, p_surface, material, m_src, p_mesh, overlay_valid, p_bucket_offset, p_assembly_part);
		}
	}
}

void RenderForwardClustered::_geometry_instance_update(RenderGeometryInstance *p_geometry_instance) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::ParticlesStorage *particles_storage = RendererRD::ParticlesStorage::get_singleton();
	GeometryInstanceForwardClustered *ginstance = static_cast<GeometryInstanceForwardClustered *>(p_geometry_instance);

	if (ginstance->data->dirty_dependencies) {
		ginstance->data->dependency_tracker.update_begin();
	}

	auto add_mesh_or_assembly = [&](RID p_mesh, const RID *p_inst_materials, uint32_t p_inst_mat_count) {
		// GodotNext: nextgeo_foliage_instancing (#151, plan §21.4 GN-7). When experimental assemblies are
		// active, register each unique part mesh's surfaces under its contiguous bucket slot offset.
		if (RSG::virtual_mesh_storage != nullptr && ginstance->nextgeo_record_index != UINT32_MAX &&
				RSG::virtual_mesh_storage->are_assemblies_enabled() && RSG::virtual_mesh_storage->mesh_has_assembly(p_mesh)) {
			const LocalVector<RID> *unique_parts = RSG::virtual_mesh_storage->mesh_get_assembly_unique_part_meshes(p_mesh);
			if (unique_parts != nullptr && !unique_parts->is_empty()) {
				for (uint32_t u = 0; u < unique_parts->size(); u++) {
					const RID part_mesh = (*unique_parts)[u];
					uint32_t part_surface_count = 0;
					const RID *part_materials = mesh_storage->mesh_get_surface_count_and_materials(part_mesh, part_surface_count);
					if (part_materials) {
						for (uint32_t j = 0; j < part_surface_count; j++) {
							_geometry_instance_add_surface(ginstance, j, part_materials[j], part_mesh, u, true);
						}
					}
				}
				return;
			}
		}
		uint32_t surface_count = 0;
		const RID *materials = mesh_storage->mesh_get_surface_count_and_materials(p_mesh, surface_count);
		if (materials) {
			for (uint32_t j = 0; j < surface_count; j++) {
				RID material = (p_inst_materials != nullptr && j < p_inst_mat_count && p_inst_materials[j].is_valid()) ? p_inst_materials[j] : materials[j];
				_geometry_instance_add_surface(ginstance, j, material, p_mesh);
			}
		}
	};

	//add geometry for drawing
	switch (ginstance->data->base_type) {
		case RSE::INSTANCE_MESH: {
			RID mesh = ginstance->data->base;
			add_mesh_or_assembly(mesh, ginstance->data->surface_materials.ptr(), ginstance->data->surface_materials.size());
			ginstance->instance_count = 1;

		} break;

		case RSE::INSTANCE_MULTIMESH: {
			RID mesh = mesh_storage->multimesh_get_mesh(ginstance->data->base);
			if (mesh.is_valid()) {
				// GodotNext: nextgeo_foliage_instancing (#151). Eligible 3D non-indirect MultiMeshInstance3D
				// routes through NextGeo; only ineligible MultiMeshes (e.g. GridMap octants, 2D, indirect)
				// emit the VGD_IGNORED warning.
				if (ginstance->nextgeo_record_index == UINT32_MAX && RSG::virtual_mesh_storage != nullptr && RSG::virtual_mesh_storage->mesh_has_virtual_geometry(mesh)) {
					if (ginstance->warned_vgd_mesh != mesh) {
						ginstance->warned_vgd_mesh = mesh;
						RSG::virtual_mesh_storage->nextgeo_count_vgd_ignored_warning();
						WARN_PRINT_ONCE("NextGeo VirtualGeometryData on a MultiMesh/GridMap mesh is ignored in v1; rendering via the legacy instancing path.");
					}
				} else {
					ginstance->warned_vgd_mesh = RID();
				}
				add_mesh_or_assembly(mesh, nullptr, 0);

				ginstance->instance_count = mesh_storage->multimesh_get_instances_to_draw(ginstance->data->base);
			} else {
				ginstance->warned_vgd_mesh = RID();
			}

		} break;
#if 0
		case RSE::INSTANCE_IMMEDIATE: {
			RasterizerStorageGLES3::Immediate *immediate = storage->immediate_owner.get_or_null(inst->base);
			ERR_CONTINUE(!immediate);

			_add_geometry(immediate, inst, nullptr, -1, p_depth_pass, p_shadow_pass);

		} break;
#endif
		case RSE::INSTANCE_PARTICLES: {
			int draw_passes = particles_storage->particles_get_draw_passes(ginstance->data->base);
			RID first_vgd_mesh;

			for (int j = 0; j < draw_passes; j++) {
				RID mesh = particles_storage->particles_get_draw_pass_mesh(ginstance->data->base, j);
				if (!mesh.is_valid()) {
					continue;
				}
				if (first_vgd_mesh.is_null() && RSG::virtual_mesh_storage != nullptr && RSG::virtual_mesh_storage->mesh_has_virtual_geometry(mesh)) {
					first_vgd_mesh = mesh;
				}

				const RID *materials = nullptr;
				uint32_t surface_count;

				materials = mesh_storage->mesh_get_surface_count_and_materials(mesh, surface_count);
				if (materials) {
					for (uint32_t k = 0; k < surface_count; k++) {
						_geometry_instance_add_surface(ginstance, k, materials[k], mesh);
					}
				}
			}
			if (first_vgd_mesh.is_valid()) {
				if (ginstance->warned_vgd_mesh != first_vgd_mesh) {
					ginstance->warned_vgd_mesh = first_vgd_mesh;
					RSG::virtual_mesh_storage->nextgeo_count_vgd_ignored_warning();
					WARN_PRINT_ONCE("NextGeo VirtualGeometryData on a Particles draw-pass mesh is ignored in v1; rendering via the legacy particles path.");
				}
			} else {
				ginstance->warned_vgd_mesh = RID();
			}

			ginstance->instance_count = particles_storage->particles_get_amount(ginstance->data->base, ginstance->trail_steps);

		} break;

		default: {
		}
	}

	//Fill push constant

	ginstance->base_flags = 0;
	// GodotNext: light_source_parity (#140). Record shadow casting flag for contact-shadow intensity split.
	if (ginstance->data->casts_shadows) {
		ginstance->base_flags |= INSTANCE_DATA_FLAG_CASTS_SHADOWS;
	}

	bool store_transform = true;
	if (ginstance->data->base_type == RSE::INSTANCE_MULTIMESH) {
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH;

		if (mesh_storage->multimesh_get_transform_format(ginstance->data->base) == RSE::MULTIMESH_TRANSFORM_2D) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D;
		}
		if (mesh_storage->multimesh_uses_colors(ginstance->data->base)) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR;
		}
		if (mesh_storage->multimesh_uses_custom_data(ginstance->data->base)) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA;
		}
		if (mesh_storage->multimesh_uses_indirect(ginstance->data->base)) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_INDIRECT;
		}

		ginstance->transforms_uniform_set = mesh_storage->multimesh_get_3d_uniform_set(ginstance->data->base, scene_shader.default_shader_rd, TRANSFORMS_UNIFORM_SET);

	} else if (ginstance->data->base_type == RSE::INSTANCE_PARTICLES) {
		ginstance->base_flags |= INSTANCE_DATA_FLAG_PARTICLES;
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH;

		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR;
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA;

		//for particles, stride is the trail size
		ginstance->base_flags |= (ginstance->trail_steps << INSTANCE_DATA_FLAGS_PARTICLE_TRAIL_SHIFT);

		if (!particles_storage->particles_is_using_local_coords(ginstance->data->base)) {
			store_transform = false;
		}
		if (particles_storage->particles_get_frame_counter(ginstance->data->base) == 0) {
			// Particles haven't been cleared or updated, update once now to ensure they are ready to render.
			particles_storage->update_particles();
		}

		ginstance->transforms_uniform_set = particles_storage->particles_get_instance_buffer_uniform_set(ginstance->data->base, scene_shader.default_shader_rd, TRANSFORMS_UNIFORM_SET);

		if (ginstance->data->dirty_dependencies) {
			particles_storage->particles_update_dependency(ginstance->data->base, &ginstance->data->dependency_tracker);
		}
	} else if (ginstance->data->base_type == RSE::INSTANCE_MESH) {
		// GodotNext: spline_meshes (#161). Active spline instances carry spline_deform_data[1] = 1 in InstanceData.
		if (mesh_storage->skeleton_is_valid(ginstance->data->skeleton)) {
			ginstance->transforms_uniform_set = mesh_storage->skeleton_get_3d_uniform_set(ginstance->data->skeleton, scene_shader.default_shader_rd, TRANSFORMS_UNIFORM_SET);
			if (ginstance->data->dirty_dependencies) {
				mesh_storage->skeleton_update_dependency(ginstance->data->skeleton, &ginstance->data->dependency_tracker);
			}
		} else {
			ginstance->transforms_uniform_set = RID();
		}
	}

	ginstance->store_transform_cache = store_transform;
	ginstance->can_sdfgi = false;

	if (!RendererRD::LightStorage::get_singleton()->lightmap_instance_is_valid(ginstance->lightmap_instance)) {
		if (ginstance->voxel_gi_instances[0].is_null() && (ginstance->data->use_baked_light || ginstance->data->use_dynamic_gi)) {
			ginstance->can_sdfgi = true;
		}
	}

	if (ginstance->data->dirty_dependencies) {
		ginstance->data->dependency_tracker.update_end();
		ginstance->data->dirty_dependencies = false;
	}

	if (RSG::virtual_mesh_storage != nullptr && ginstance->nextgeo_record_index != UINT32_MAX) {
		const uint32_t bslot_base = ginstance->nextgeo_bucket_slot;
		const uint32_t bslot_count = MAX(RSG::virtual_mesh_storage->instance_record_get_bucket_slot_count(ginstance->nextgeo_record_index), 1u);
		LocalVector<uint32_t> part_vmask;
		LocalVector<uint32_t> part_smask;
		LocalVector<uint32_t> part_cdmask;
		LocalVector<uint32_t> part_cfmask;
		part_vmask.resize_initialized(bslot_count);
		part_smask.resize_initialized(bslot_count);
		part_cdmask.resize_initialized(bslot_count);
		part_cfmask.resize_initialized(bslot_count);
		uint32_t virtual_surface_mask = 0u;
		uint32_t shadow_surface_mask = 0u;
		uint32_t cull_disabled_mask = 0u;
		uint32_t cull_front_mask = 0u;
		for (GeometryInstanceSurfaceDataCache *s = ginstance->surface_caches; s != nullptr; s = s->next) {
			if ((s->flags & GeometryInstanceSurfaceDataCache::FLAG_VIRTUAL) && s->surface_index < 32) {
				const uint32_t bit = 1u << s->surface_index;
				const uint32_t off = s->nextgeo_bucket_offset;
				virtual_surface_mask |= bit;
				if (off < bslot_count) {
					part_vmask[off] |= bit;
				}
				if (s->nextgeo_raster_bin == 0u) {
					shadow_surface_mask |= bit;
					if (off < bslot_count) {
						part_smask[off] |= bit;
					}
				}
				if ((s->flags & GeometryInstanceSurfaceDataCache::FLAG_USES_DOUBLE_SIDED_SHADOWS) ||
						(s->shader != nullptr && s->shader->cull_mode == RSE::CULL_MODE_DISABLED)) {
					cull_disabled_mask |= bit;
					if (off < bslot_count) {
						part_cdmask[off] |= bit;
					}
				} else if (s->shader != nullptr && s->shader->cull_mode == RSE::CULL_MODE_FRONT) {
					cull_front_mask |= bit;
					if (off < bslot_count) {
						part_cfmask[off] |= bit;
					}
				}
			}
		}
		if (bslot_base != UINT32_MAX) {
			for (uint32_t u = 0; u < bslot_count; u++) {
				RSG::virtual_mesh_storage->bucket_slot_set_surface_masks(bslot_base + u, part_vmask[u], part_smask[u], part_cdmask[u], part_cfmask[u]);
			}
		}
		RSG::virtual_mesh_storage->instance_record_set_surface_mask(ginstance->nextgeo_record_index, virtual_surface_mask, shadow_surface_mask);
		RSG::virtual_mesh_storage->instance_record_set_surface_cull_masks(ginstance->nextgeo_record_index, cull_disabled_mask, cull_front_mask);
	}

	ginstance->dirty_list_element.remove_from_list();
}

static RD::FramebufferFormatID _get_color_framebuffer_format_for_pipeline(RD::DataFormat p_color_format, bool p_can_be_storage, RD::TextureSamples p_samples, bool p_specular, bool p_velocity, uint32_t p_view_count) {
	const bool multisampling = p_samples > RD::TEXTURE_SAMPLES_1;
	RD::AttachmentFormat attachment;
	attachment.samples = p_samples;

	RD::AttachmentFormat unused_attachment;
	unused_attachment.usage_flags = RD::AttachmentFormat::UNUSED_ATTACHMENT;

	thread_local Vector<RD::AttachmentFormat> attachments;
	attachments.clear();

	// Color attachment.
	attachment.format = p_color_format;
	attachment.usage_flags = RenderSceneBuffersRD::get_color_usage_bits(false, multisampling, p_can_be_storage);
	attachments.push_back(attachment);

	if (p_specular) {
		attachment.format = RenderForwardClustered::RenderBufferDataForwardClustered::get_specular_format();
		attachment.usage_flags = RenderForwardClustered::RenderBufferDataForwardClustered::get_specular_usage_bits(false, multisampling, p_can_be_storage);
		attachments.push_back(attachment);
	} else {
		attachments.push_back(unused_attachment);
	}

	if (p_velocity) {
		attachment.format = RenderSceneBuffersRD::get_velocity_format();
		attachment.usage_flags = RenderSceneBuffersRD::get_velocity_usage_bits(false, multisampling, p_can_be_storage);
		attachments.push_back(attachment);
	} else {
		attachments.push_back(unused_attachment);
	}

	// Depth attachment.
	attachment.format = RenderSceneBuffersRD::get_depth_format(false, multisampling, p_can_be_storage);
	attachment.usage_flags = RenderSceneBuffersRD::get_depth_usage_bits(false, multisampling, p_can_be_storage);
	attachments.push_back(attachment);

	thread_local Vector<RD::FramebufferPass> passes;
	passes.resize(1);
	passes.ptrw()[0].color_attachments.resize(attachments.size() - 1);

	int *color_attachments = passes.ptrw()[0].color_attachments.ptrw();
	for (int64_t i = 0; i < attachments.size() - 1; i++) {
		color_attachments[i] = (attachments[i].usage_flags == RD::AttachmentFormat::UNUSED_ATTACHMENT) ? RD::ATTACHMENT_UNUSED : i;
	}

	passes.ptrw()[0].depth_attachment = attachments.size() - 1;

	return RD::get_singleton()->framebuffer_format_create_multipass(attachments, passes, p_view_count);
}

static RD::FramebufferFormatID _get_reflection_probe_color_framebuffer_format_for_pipeline(bool p_storage) {
	RD::AttachmentFormat attachment;
	thread_local Vector<RD::AttachmentFormat> attachments;
	attachments.clear();

	attachment.format = RendererRD::LightStorage::get_reflection_probe_color_format();
	attachment.usage_flags = RendererRD::LightStorage::get_reflection_probe_color_usage_bits(p_storage);
	attachments.push_back(attachment);

	attachment.format = RendererRD::LightStorage::get_reflection_probe_depth_format();
	attachment.usage_flags = RendererRD::LightStorage::get_reflection_probe_depth_usage_bits();
	attachments.push_back(attachment);

	return RD::get_singleton()->framebuffer_format_create(attachments);
}

static RD::FramebufferFormatID _get_depth_framebuffer_format_for_pipeline(bool p_can_be_storage, RD::TextureSamples p_samples, bool p_normal_roughness, bool p_voxelgi) {
	const bool multisampling = p_samples > RD::TEXTURE_SAMPLES_1;
	RD::AttachmentFormat attachment;
	attachment.samples = p_samples;

	thread_local LocalVector<RD::AttachmentFormat> attachments;
	attachments.clear();

	attachment.format = RenderSceneBuffersRD::get_depth_format(false, multisampling, p_can_be_storage);
	attachment.usage_flags = RenderSceneBuffersRD::get_depth_usage_bits(false, multisampling, p_can_be_storage);
	attachments.push_back(attachment);

	if (p_normal_roughness) {
		attachment.format = RenderForwardClustered::RenderBufferDataForwardClustered::get_normal_roughness_format();
		attachment.usage_flags = RenderForwardClustered::RenderBufferDataForwardClustered::get_normal_roughness_usage_bits(false, multisampling, p_can_be_storage);
		attachments.push_back(attachment);
	}

	if (p_voxelgi) {
		attachment.format = RenderForwardClustered::RenderBufferDataForwardClustered::get_voxelgi_format();
		attachment.usage_flags = RenderForwardClustered::RenderBufferDataForwardClustered::get_voxelgi_usage_bits(false, multisampling, p_can_be_storage);
		attachments.push_back(attachment);
	}

	thread_local Vector<RD::FramebufferPass> passes;
	passes.resize(1);
	passes.ptrw()[0].color_attachments.resize(attachments.size() - 1);

	int *color_attachments = passes.ptrw()[0].color_attachments.ptrw();
	for (int64_t i = 1; i < attachments.size(); i++) {
		color_attachments[i - 1] = (attachments[i].usage_flags == RD::AttachmentFormat::UNUSED_ATTACHMENT) ? RD::ATTACHMENT_UNUSED : i;
	}

	passes.ptrw()[0].depth_attachment = 0;

	return RD::get_singleton()->framebuffer_format_create_multipass(Vector<RD::AttachmentFormat>(attachments), passes);
}

static RD::FramebufferFormatID _get_shadow_cubemap_framebuffer_format_for_pipeline() {
	thread_local LocalVector<RD::AttachmentFormat> attachments;
	attachments.clear();

	RD::AttachmentFormat attachment;
	attachment.format = RendererRD::LightStorage::get_cubemap_depth_format();
	attachment.usage_flags = RendererRD::LightStorage::get_cubemap_depth_usage_bits();
	attachments.push_back(attachment);

	return RD::get_singleton()->framebuffer_format_create(Vector<RD::AttachmentFormat>(attachments));
}

static RD::FramebufferFormatID _get_shadow_atlas_framebuffer_format_for_pipeline(bool p_use_16_bits) {
	thread_local LocalVector<RD::AttachmentFormat> attachments;
	attachments.clear();

	RD::AttachmentFormat attachment;
	attachment.format = RendererRD::LightStorage::get_shadow_atlas_depth_format(p_use_16_bits);
	attachment.usage_flags = RendererRD::LightStorage::get_shadow_atlas_depth_usage_bits();
	attachments.push_back(attachment);

	return RD::get_singleton()->framebuffer_format_create(Vector<RD::AttachmentFormat>(attachments));
}

static RD::FramebufferFormatID _get_reflection_probe_depth_framebuffer_format_for_pipeline() {
	thread_local LocalVector<RD::AttachmentFormat> attachments;
	attachments.clear();

	RD::AttachmentFormat attachment;
	attachment.format = RendererRD::LightStorage::get_reflection_probe_depth_format();
	attachment.usage_flags = RendererRD::LightStorage::get_reflection_probe_depth_usage_bits();
	attachments.push_back(attachment);

	return RD::get_singleton()->framebuffer_format_create(Vector<RD::AttachmentFormat>(attachments));
}

void RenderForwardClustered::_mesh_compile_pipeline_for_surface(SceneShaderForwardClustered::ShaderData *p_shader, void *p_mesh_surface, bool p_ubershader, bool p_instanced_surface, RSE::PipelineSource p_source, SceneShaderForwardClustered::ShaderData::PipelineKey &r_pipeline_key, Vector<ShaderPipelinePair> *r_pipeline_pairs) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	uint64_t input_mask = p_shader->get_vertex_input_mask(r_pipeline_key.version, r_pipeline_key.color_pass_flags, p_ubershader);
	bool pipeline_motion_vectors = r_pipeline_key.color_pass_flags & SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS;
	bool emulate_point_size = p_shader->uses_point_size && scene_shader.emulate_point_size;
	r_pipeline_key.vertex_format_id = mesh_storage->mesh_surface_get_vertex_format(p_mesh_surface, input_mask, p_instanced_surface, pipeline_motion_vectors, emulate_point_size);
	r_pipeline_key.ubershader = p_ubershader;

	p_shader->pipeline_hash_map.compile_pipeline(r_pipeline_key, r_pipeline_key.hash(), p_source, p_ubershader);

	if (r_pipeline_pairs != nullptr) {
		r_pipeline_pairs->push_back({ p_shader, r_pipeline_key });
	}
}

void RenderForwardClustered::_mesh_compile_pipelines_for_surface(const SurfacePipelineData &p_surface, const GlobalPipelineData &p_global, RSE::PipelineSource p_source, Vector<ShaderPipelinePair> *r_pipeline_pairs) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	bool octmap_use_storage = !copy_effects->get_raster_effects().has_flag(RendererRD::CopyEffects::RASTER_EFFECT_OCTMAP);

	// Retrieve from the scene shader which groups are currently enabled.
	const bool multiview_enabled = p_global.use_multiview && scene_shader.is_multiview_shader_group_enabled();
	const RD::DataFormat buffers_color_format = _render_buffers_get_preferred_color_format();
	const bool buffers_can_be_storage = _render_buffers_can_be_storage();

	// Set the attributes common to all pipelines.
	SceneShaderForwardClustered::ShaderData::PipelineKey pipeline_key;
	pipeline_key.cull_mode = RD::POLYGON_CULL_DISABLED;
	pipeline_key.primitive_type = mesh_storage->mesh_surface_get_primitive(p_surface.mesh_surface);
	pipeline_key.wireframe = false;

	// Grab the shader and surface used for most passes.
	const uint32_t multiview_iterations = multiview_enabled ? 2 : 1;
	const uint32_t lightmap_iterations = p_global.use_lightmaps && p_surface.can_use_lightmap ? 2 : 1;
	const uint32_t alpha_iterations = p_surface.uses_transparent ? 2 : 1;
	for (uint32_t multiview = 0; multiview < multiview_iterations; multiview++) {
		for (uint32_t lightmap = 0; lightmap < lightmap_iterations; lightmap++) {
			for (uint32_t alpha = p_surface.uses_opaque ? 0 : 1; alpha < alpha_iterations; alpha++) {
				// Generate all the possible variants used during the color pass.
				pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_COLOR_PASS;
				pipeline_key.color_pass_flags = 0;

				if (lightmap) {
					pipeline_key.color_pass_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_LIGHTMAP;
				}

				if (alpha) {
					pipeline_key.color_pass_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_TRANSPARENT;
				}

				if (multiview) {
					pipeline_key.color_pass_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_MULTIVIEW;
				} else if (p_global.use_reflection_probes) {
					// Reflection probe can't be rendered in multiview.
					pipeline_key.framebuffer_format_id = _get_reflection_probe_color_framebuffer_format_for_pipeline(octmap_use_storage);
					_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
				}

				// View count is assumed to be 2 as the configuration is dependent on the viewport. It's likely a safe assumption for stereo rendering.
				uint32_t view_count = multiview ? 2 : 1;
				pipeline_key.framebuffer_format_id = _get_color_framebuffer_format_for_pipeline(buffers_color_format, buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), false, false, view_count);
				_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);

				// Generate all the possible variants used during the advanced color passes.
				const uint32_t separate_specular_iterations = p_global.use_separate_specular ? 2 : 1;
				const uint32_t motion_vectors_iterations = p_global.use_motion_vectors ? 2 : 1;
				uint32_t base_color_pass_flags = pipeline_key.color_pass_flags;
				for (uint32_t separate_specular = 0; separate_specular < separate_specular_iterations; separate_specular++) {
					for (uint32_t motion_vectors = 0; motion_vectors < motion_vectors_iterations; motion_vectors++) {
						if (!separate_specular && !motion_vectors) {
							// This case was already generated.
							continue;
						}

						pipeline_key.color_pass_flags = base_color_pass_flags;

						if (separate_specular) {
							pipeline_key.color_pass_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_SEPARATE_SPECULAR;
						}

						if (motion_vectors) {
							pipeline_key.color_pass_flags |= SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS;
						}

						pipeline_key.framebuffer_format_id = _get_color_framebuffer_format_for_pipeline(buffers_color_format, buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), separate_specular, motion_vectors, view_count);
						_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
					}
				}
			}
		}
	}

	if (!p_surface.uses_depth) {
		return;
	}

	// Generate the depth pipelines if the material supports depth or it must be part of the shadow pass.
	pipeline_key.color_pass_flags = 0;

	if (p_global.use_normal_and_roughness) {
		// A lot of different effects rely on normal and roughness being written to during the depth pass.
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS;
		pipeline_key.framebuffer_format_id = _get_depth_framebuffer_format_for_pipeline(buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), true, false);
		_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	if (p_global.use_voxelgi) {
		// Depth pass with VoxelGI support.
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI;
		pipeline_key.framebuffer_format_id = _get_depth_framebuffer_format_for_pipeline(buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), true, true);
		_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	if (p_global.use_sdfgi) {
		// Depth pass with SDFGI support.
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_WITH_SDF;
		pipeline_key.framebuffer_format_id = _get_depth_framebuffer_format_for_pipeline(buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), false, false);
		_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);

		// Depth pass with SDFGI support for an empty framebuffer.
		pipeline_key.framebuffer_format_id = RD::get_singleton()->framebuffer_format_create_empty();
		_mesh_compile_pipeline_for_surface(p_surface.shader, p_surface.mesh_surface, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	// The dedicated depth passes use a different version of the surface and the shader.
	pipeline_key.primitive_type = mesh_storage->mesh_surface_get_primitive(p_surface.mesh_surface_shadow);
	pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS;
	pipeline_key.framebuffer_format_id = _get_depth_framebuffer_format_for_pipeline(buffers_can_be_storage, RD::TextureSamples(p_global.texture_samples), false, false);
	_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);

	if (p_global.use_shadow_dual_paraboloid) {
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_DP;
		_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	if (p_global.use_shadow_cubemaps) {
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS;
		pipeline_key.framebuffer_format_id = _get_shadow_cubemap_framebuffer_format_for_pipeline();
		_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	// Atlas shadowmaps (omni lights) can be in both 16-bit and 32-bit versions.
	const uint32_t use_16_bits_start = p_global.use_32_bit_shadows ? 0 : 1;
	const uint32_t use_16_bits_iterations = p_global.use_16_bit_shadows ? 2 : 1;
	for (uint32_t use_16_bits = use_16_bits_start; use_16_bits < use_16_bits_iterations; use_16_bits++) {
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS;
		pipeline_key.framebuffer_format_id = _get_shadow_atlas_framebuffer_format_for_pipeline(use_16_bits);
		_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);

		if (p_global.use_shadow_dual_paraboloid) {
			pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS_DP;
			_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
		}
	}

	if (p_global.use_reflection_probes) {
		// Depth pass for reflection probes. Normally this will be redundant as the format is the exact same as the shadow cubemap.
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_DEPTH_PASS;
		pipeline_key.framebuffer_format_id = _get_reflection_probe_depth_framebuffer_format_for_pipeline();
		_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}

	// GodotNext: vsm_non_nanite (#218). The VSM depth variant draws into an attachmentless framebuffer.
	if (scene_shader.is_vsm_shader_group_enabled()) {
		pipeline_key.version = SceneShaderForwardClustered::PIPELINE_VERSION_VSM_DEPTH;
		pipeline_key.framebuffer_format_id = RD::get_singleton()->framebuffer_format_create_empty();
		_mesh_compile_pipeline_for_surface(p_surface.shader_shadow, p_surface.mesh_surface_shadow, true, p_surface.instanced, p_source, pipeline_key, r_pipeline_pairs);
	}
}

void RenderForwardClustered::_mesh_generate_all_pipelines_for_surface_cache(GeometryInstanceSurfaceDataCache *p_surface_cache, const GlobalPipelineData &p_global) {
	bool uses_alpha_pass = (p_surface_cache->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA) != 0;
	float multiplied_fade_alpha = p_surface_cache->owner->force_alpha * p_surface_cache->owner->parent_fade_alpha;
	bool uses_fade = (multiplied_fade_alpha < FADE_ALPHA_PASS_THRESHOLD) || p_surface_cache->owner->fade_near || p_surface_cache->owner->fade_far;
	SurfacePipelineData surface;
	surface.mesh_surface = p_surface_cache->surface;
	surface.mesh_surface_shadow = p_surface_cache->surface_shadow;
	surface.shader = p_surface_cache->shader;
	surface.shader_shadow = p_surface_cache->shader_shadow;
	surface.instanced = p_surface_cache->owner->mesh_instance.is_valid();
	surface.uses_opaque = !uses_alpha_pass;
	surface.uses_transparent = uses_alpha_pass || uses_fade;
	surface.uses_depth = (p_surface_cache->flags & (GeometryInstanceSurfaceDataCache::FLAG_PASS_DEPTH | GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE | GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW)) != 0;
	surface.can_use_lightmap = p_surface_cache->owner->lightmap_instance.is_valid() || p_surface_cache->owner->lightmap_sh;
	_mesh_compile_pipelines_for_surface(surface, p_global, RSE::PIPELINE_SOURCE_SURFACE);
}

void RenderForwardClustered::_update_dirty_geometry_instances() {
	while (geometry_instance_dirty_list.first()) {
		_geometry_instance_update(geometry_instance_dirty_list.first()->self());
	}

	_update_dirty_geometry_pipelines();
}

void RenderForwardClustered::_update_dirty_geometry_pipelines() {
	if (global_pipeline_data_required.key != global_pipeline_data_compiled.key) {
		// Go through the entire list of surfaces and compile pipelines for everything again.
		for (GeometryInstanceSurfaceDataCache &surface_cache : geometry_surface_compilation_all_list) {
			_mesh_generate_all_pipelines_for_surface_cache(&surface_cache, global_pipeline_data_required);

			if (surface_cache.compilation_dirty_element.in_list()) {
				// Remove any elements from the dirty list as they don't need to be processed again.
				geometry_surface_compilation_dirty_list.remove(&surface_cache.compilation_dirty_element);
			}
		}

		global_pipeline_data_compiled.key = global_pipeline_data_required.key;
	} else {
		// Compile pipelines only for the dirty list.
		if (!geometry_surface_compilation_dirty_list.first()) {
			return;
		}

		while (geometry_surface_compilation_dirty_list.first() != nullptr) {
			GeometryInstanceSurfaceDataCache *surface_cache = geometry_surface_compilation_dirty_list.first()->self();
			_mesh_generate_all_pipelines_for_surface_cache(surface_cache, global_pipeline_data_compiled);
			surface_cache->compilation_dirty_element.remove_from_list();
		}
	}
}

void RenderForwardClustered::_geometry_instance_dependency_changed(Dependency::DependencyChangedNotification p_notification, DependencyTracker *p_tracker) {
	switch (p_notification) {
		case Dependency::DEPENDENCY_CHANGED_MATERIAL:
		case Dependency::DEPENDENCY_CHANGED_MESH:
		case Dependency::DEPENDENCY_CHANGED_PARTICLES:
		case Dependency::DEPENDENCY_CHANGED_PARTICLES_INSTANCES:
		case Dependency::DEPENDENCY_CHANGED_MULTIMESH:
		case Dependency::DEPENDENCY_CHANGED_SKELETON_DATA: {
			static_cast<RenderGeometryInstance *>(p_tracker->userdata)->_mark_dirty();
			static_cast<GeometryInstanceForwardClustered *>(p_tracker->userdata)->data->dirty_dependencies = true;
		} break;
		case Dependency::DEPENDENCY_CHANGED_MULTIMESH_VISIBLE_INSTANCES: {
			GeometryInstanceForwardClustered *ginstance = static_cast<GeometryInstanceForwardClustered *>(p_tracker->userdata);
			if (ginstance->data->base_type == RSE::INSTANCE_MULTIMESH) {
				ginstance->instance_count = RendererRD::MeshStorage::get_singleton()->multimesh_get_instances_to_draw(ginstance->data->base);
			}
		} break;
		default: {
			//rest of notifications of no interest
		} break;
	}
}
void RenderForwardClustered::_geometry_instance_dependency_deleted(const RID &p_dependency, DependencyTracker *p_tracker) {
	static_cast<RenderGeometryInstance *>(p_tracker->userdata)->_mark_dirty();
	static_cast<GeometryInstanceForwardClustered *>(p_tracker->userdata)->data->dirty_dependencies = true;
}

RenderGeometryInstance *RenderForwardClustered::geometry_instance_create(RID p_base) {
	RSE::InstanceType type = RSG::utilities->get_base_type(p_base);
	ERR_FAIL_COND_V(!((1 << type) & RSE::INSTANCE_GEOMETRY_MASK), nullptr);

	GeometryInstanceForwardClustered *ginstance = geometry_instance_alloc.alloc();
	ginstance->data = memnew(GeometryInstanceForwardClustered::Data);

	ginstance->data->base = p_base;
	ginstance->data->base_type = type;
	ginstance->data->dependency_tracker.userdata = ginstance;
	ginstance->data->dependency_tracker.changed_callback = _geometry_instance_dependency_changed;
	ginstance->data->dependency_tracker.deleted_callback = _geometry_instance_dependency_deleted;

	ginstance->_mark_dirty();

	return ginstance;
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_transform(const Transform3D &p_transform, const AABB &p_aabb, const AABB &p_transformed_aabb) {
	uint64_t frame = RSG::rasterizer->get_frame_number();
	if (frame != prev_transform_change_frame) {
		prev_transform = transform;
		prev_transform_change_frame = frame;
		transform_status = TransformStatus::MOVED;
	} else if (unlikely(transform_status == TransformStatus::TELEPORTED)) {
		prev_transform = transform;
	}

	RenderGeometryInstanceBase::set_transform(p_transform, p_aabb, p_transformed_aabb);
}

void RenderForwardClustered::GeometryInstanceForwardClustered::reset_motion_vectors() {
	prev_transform = transform;
	transform_status = TransformStatus::TELEPORTED;
	prev_spline_deform = spline_deform;
	spline_changed_this_frame = false;
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_spline_deform(bool p_active, const SplineDeformPose &p_pose, uint64_t p_generation) {
	const uint64_t frame = RSG::rasterizer ? RSG::rasterizer->get_frame_number() : 0;
	const bool was_active = spline_deform_active;
	if (!was_active && p_active) {
		prev_spline_deform = p_pose;
		prev_spline_change_frame = frame;
		spline_changed_this_frame = true;
	} else if (was_active && p_active) {
		if (frame != prev_spline_change_frame) {
			prev_spline_deform = spline_deform;
			prev_spline_change_frame = frame;
		}
		spline_changed_this_frame = true;
	} else if (!p_active) {
		prev_spline_deform = p_pose;
		spline_changed_this_frame = false;
	}
	RenderGeometryInstanceBase::set_spline_deform(p_active, p_pose, p_generation);
	if (was_active != p_active) {
		_mark_dirty();
	}
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_use_lightmap(RID p_lightmap_instance, const Rect2 &p_lightmap_uv_scale, int p_lightmap_slice_index) {
	lightmap_instance = p_lightmap_instance;
	lightmap_uv_scale = p_lightmap_uv_scale;
	lightmap_slice_index = p_lightmap_slice_index;

	_mark_dirty();
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_lightmap_capture(const Color *p_sh9) {
	if (p_sh9) {
		if (lightmap_sh == nullptr) {
			lightmap_sh = RenderForwardClustered::get_singleton()->geometry_instance_lightmap_sh.alloc();
		}

		memcpy(lightmap_sh->sh, p_sh9, sizeof(Color) * 9);
	} else {
		if (lightmap_sh != nullptr) {
			RenderForwardClustered::get_singleton()->geometry_instance_lightmap_sh.free(lightmap_sh);
			lightmap_sh = nullptr;
		}
	}
	_mark_dirty();
}

bool RenderForwardClustered::_nextgeo_view_runs_hybrid_raster() {
	return RendererRD::NextGeoRasterMode(int(GLOBAL_GET("rendering/nextgeo/raster_mode"))) == RendererRD::NEXTGEO_RASTER_MODE_HYBRID && nextgeo_cut.is_hybrid_raster_available() &&
			nextgeo_cut.get_vis64_storage(bool(GLOBAL_GET("rendering/nextgeo/debug_vis64_buffer"))) != RendererRD::NEXTGEO_VIS64_NONE;
}

void RenderForwardClustered::geometry_instance_free(RenderGeometryInstance *p_geometry_instance) {
	GeometryInstanceForwardClustered *ginstance = static_cast<GeometryInstanceForwardClustered *>(p_geometry_instance);
	ERR_FAIL_NULL(ginstance);
	if (ginstance->lightmap_sh != nullptr) {
		geometry_instance_lightmap_sh.free(ginstance->lightmap_sh);
	}
	GeometryInstanceSurfaceDataCache *surf = ginstance->surface_caches;
	while (surf) {
		GeometryInstanceSurfaceDataCache *next = surf->next;
		// GodotNext: nextgeo_raster_bins (#155).
		if (surf->nextgeo_raster_bin != 0u && RSG::virtual_mesh_storage != nullptr) {
			RSG::virtual_mesh_storage->raster_bin_release(surf->nextgeo_raster_bin);
		}
		geometry_instance_surface_alloc.free(surf);
		surf = next;
	}
	memdelete(ginstance->data);
	geometry_instance_alloc.free(ginstance);
}

Dictionary RenderForwardClustered::geometry_instance_get_nextgeo_debug_stats(RenderGeometryInstance *p_geometry_instance) const {
	Dictionary out;
	if (p_geometry_instance == nullptr) {
		return out;
	}
	const_cast<RenderForwardClustered *>(this)->_update_dirty_geometry_instances();
	const GeometryInstanceForwardClustered *ginstance = static_cast<const GeometryInstanceForwardClustered *>(p_geometry_instance);
	const bool draw_ready = RSG::virtual_mesh_storage == nullptr || RSG::virtual_mesh_storage->is_nextgeo_draw_ready() || bool(GLOBAL_GET("rendering/nextgeo/visibility_only"));
	const bool is_multimesh_or_particles = ginstance->data->base_type == RSE::INSTANCE_PARTICLES ||
			(ginstance->data->base_type == RSE::INSTANCE_MULTIMESH && ginstance->nextgeo_record_index == UINT32_MAX);

	struct SurfaceEntry {
		uint32_t index = 0;
		bool is_virtual = false;
		String reason;
	};
	LocalVector<SurfaceEntry> ordered;
	for (const GeometryInstanceSurfaceDataCache *s = ginstance->surface_caches; s != nullptr; s = s->next) {
		bool found = false;
		for (uint32_t k = 0; k < ordered.size(); k++) {
			if (ordered[k].index == s->surface_index) {
				found = true;
				break;
			}
		}
		if (found) {
			continue;
		}
		SurfaceEntry entry;
		entry.index = s->surface_index;
		if (is_multimesh_or_particles) {
			entry.is_virtual = false;
			entry.reason = "MULTIMESH_OR_PARTICLES";
		} else {
			entry.is_virtual = ((s->flags & GeometryInstanceSurfaceDataCache::FLAG_VIRTUAL) != 0) && draw_ready;
			entry.reason = RendererRD::nextgeo_fallback_name(RendererRD::NextGeoFallback(s->nextgeo_fallback_reason));
		}
		ordered.push_back(entry);
	}
	for (uint32_t a = 0; a < ordered.size(); a++) {
		for (uint32_t b = a + 1; b < ordered.size(); b++) {
			if (ordered[b].index < ordered[a].index) {
				SWAP(ordered[a], ordered[b]);
			}
		}
	}

	Array surfaces;
	for (uint32_t i = 0; i < ordered.size(); i++) {
		Dictionary sd;
		sd["surface"] = int64_t(ordered[i].index);
		sd["route"] = ordered[i].is_virtual ? "virtual" : "legacy";
		sd["reason"] = ordered[i].reason;
		surfaces.push_back(sd);
	}

	out["record_index"] = ginstance->nextgeo_record_index == UINT32_MAX ? int64_t(-1) : int64_t(ginstance->nextgeo_record_index);
	out["surfaces"] = surfaces;
	if (!ordered.is_empty()) {
		out["route"] = ordered[0].is_virtual ? "virtual" : "legacy";
		out["reason"] = ordered[0].reason;
	} else if (is_multimesh_or_particles) {
		out["route"] = "legacy";
		out["reason"] = "MULTIMESH_OR_PARTICLES";
	} else {
		out["route"] = "legacy";
		out["reason"] = "NONE";
	}
	return out;
}

uint32_t RenderForwardClustered::geometry_instance_get_pair_mask() {
	return (1 << RSE::INSTANCE_VOXEL_GI);
}

void RenderForwardClustered::mesh_generate_pipelines(RID p_mesh, bool p_background_compilation) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RID shadow_mesh = mesh_storage->mesh_get_shadow_mesh(p_mesh);
	uint32_t surface_count = 0;
	const RID *materials = mesh_storage->mesh_get_surface_count_and_materials(p_mesh, surface_count);
	Vector<ShaderPipelinePair> pipeline_pairs;
	for (uint32_t i = 0; i < surface_count; i++) {
		if (materials[i].is_null()) {
			continue;
		}

		void *mesh_surface = mesh_storage->mesh_get_surface(p_mesh, i);
		void *mesh_surface_shadow = mesh_surface;
		SceneShaderForwardClustered::MaterialData *material = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(materials[i], RendererRD::MaterialStorage::SHADER_TYPE_3D));
		if (material == nullptr || !material->shader_data->is_valid()) {
			continue;
		}

		SceneShaderForwardClustered::ShaderData *shader = material->shader_data;
		SceneShaderForwardClustered::ShaderData *shader_shadow = shader;
		if (material->shader_data->uses_shared_shadow_material()) {
			SceneShaderForwardClustered::MaterialData *material_shadow = static_cast<SceneShaderForwardClustered::MaterialData *>(material_storage->material_get_data(scene_shader.default_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
			if (material_shadow != nullptr) {
				shader_shadow = material_shadow->shader_data;
				if (shadow_mesh.is_valid()) {
					mesh_surface_shadow = mesh_storage->mesh_get_surface(shadow_mesh, i);
				}
			}
		}

		if (!shader->is_valid()) {
			continue;
		}

		SurfacePipelineData surface;
		surface.mesh_surface = mesh_surface;
		surface.mesh_surface_shadow = mesh_surface_shadow;
		surface.shader = shader;
		surface.shader_shadow = shader_shadow;
		surface.instanced = mesh_storage->mesh_needs_instance(p_mesh, true);
		surface.uses_opaque = !material->shader_data->uses_alpha_pass();
		surface.uses_transparent = material->shader_data->uses_alpha_pass();
		surface.uses_depth = surface.uses_opaque || (surface.uses_transparent && material->shader_data->uses_depth_in_alpha_pass());
		surface.can_use_lightmap = mesh_storage->mesh_surface_get_format(mesh_surface) & RSE::ARRAY_FORMAT_TEX_UV2;
		_mesh_compile_pipelines_for_surface(surface, global_pipeline_data_required, RSE::PIPELINE_SOURCE_MESH, &pipeline_pairs);
	}

	// Wait for all the pipelines that were compiled. This will force the loader to wait on all ubershader pipelines to be ready.
	if (!p_background_compilation && !pipeline_pairs.is_empty()) {
		for (ShaderPipelinePair pair : pipeline_pairs) {
			pair.first->pipeline_hash_map.wait_for_pipeline(pair.second.hash());
		}
	}
}

uint32_t RenderForwardClustered::get_pipeline_compilations(RSE::PipelineSource p_source) {
	return scene_shader.get_pipeline_compilations(p_source);
}

void RenderForwardClustered::enable_features(BitField<FeatureBits> p_feature_bits) {
	if (p_feature_bits.has_flag(FEATURE_MULTIVIEW_BIT)) {
		scene_shader.enable_multiview_shader_group();
	}

	if (p_feature_bits.has_flag(FEATURE_ADVANCED_BIT)) {
		scene_shader.enable_advanced_shader_group(p_feature_bits.has_flag(FEATURE_MULTIVIEW_BIT));
	}

	if (p_feature_bits.has_flag(FEATURE_VRS_BIT)) {
		gi.enable_vrs_shader_group();
	}
}

String RenderForwardClustered::get_name() const {
	return "forward_clustered";
}

void RenderForwardClustered::GeometryInstanceForwardClustered::pair_voxel_gi_instances(const RID *p_voxel_gi_instances, uint32_t p_voxel_gi_instance_count) {
	if (p_voxel_gi_instance_count > 0) {
		voxel_gi_instances[0] = p_voxel_gi_instances[0];
	} else {
		voxel_gi_instances[0] = RID();
	}

	if (p_voxel_gi_instance_count > 1) {
		voxel_gi_instances[1] = p_voxel_gi_instances[1];
	} else {
		voxel_gi_instances[1] = RID();
	}
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_softshadow_projector_pairing(bool p_softshadow, bool p_projector) {
	using_projectors = p_projector;
	using_softshadows = p_softshadow;
	_mark_dirty();
}

void RenderForwardClustered::_update_shader_quality_settings() {
	SceneShaderForwardClustered::ShaderSpecialization specialization = {};
	specialization.decal_use_mipmaps = decals_get_filter() == RSE::DECAL_FILTER_NEAREST_MIPMAPS ||
			decals_get_filter() == RSE::DECAL_FILTER_LINEAR_MIPMAPS ||
			decals_get_filter() == RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC ||
			decals_get_filter() == RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC;
	;
	specialization.projector_use_mipmaps = light_projectors_get_filter() == RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS ||
			light_projectors_get_filter() == RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS ||
			light_projectors_get_filter() == RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC ||
			light_projectors_get_filter() == RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC;

	specialization.soft_shadow_samples = soft_shadow_samples_get();
	specialization.penumbra_shadow_samples = penumbra_shadow_samples_get();
	specialization.directional_soft_shadow_samples = directional_soft_shadow_samples_get();
	specialization.directional_penumbra_shadow_samples = directional_penumbra_shadow_samples_get();
	specialization.use_lightmap_bicubic_filter = lightmap_filter_bicubic_get();
	specialization.fog_use_legacy_blending = fog_use_legacy_blending_get();
	scene_shader.set_default_specialization(specialization);

	base_uniforms_changed(); //also need this
}

RenderForwardClustered::RenderForwardClustered() {
	singleton = this;

	/* SCENE SHADER */

	{
		String defines;
		defines += "\n#define MAX_ROUGHNESS_LOD " + itos(get_roughness_layers() - 1) + ".0\n";
		if (is_using_radiance_octmap_array()) {
			defines += "\n#define USE_RADIANCE_OCTMAP_ARRAY \n";
		}
		defines += "\n#define SDFGI_OCT_SIZE " + itos(gi.sdfgi_get_lightprobe_octahedron_size()) + "\n";
		defines += "\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(MAX_DIRECTIONAL_LIGHTS) + "\n";

		bool force_vertex_shading = GLOBAL_GET("rendering/shading/overrides/force_vertex_shading");
		if (force_vertex_shading) {
			defines += "\n#define USE_VERTEX_LIGHTING\n";
		}

		bool multi_bounce_occlusion = GLOBAL_GET("rendering/lights_and_shadows/multi_bounce_occlusion/enabled");
		if (multi_bounce_occlusion) {
			defines += "\n#define MULTI_BOUNCE_OCCLUSION_ENABLED\n";
		}

		bool specular_occlusion = GLOBAL_GET("rendering/reflections/specular_occlusion/enabled");
		if (!specular_occlusion) {
			defines += "\n#define SPECULAR_OCCLUSION_DISABLED\n";
		}

#ifdef MODULE_TEXTURE_STREAMING_ENABLED
		bool texture_streaming = GLOBAL_GET("rendering/textures/streaming/enabled");
		if (texture_streaming) {
			defines += "\n#define TEXTURE_STREAMING\n";

			bool prepass_enabled = GLOBAL_GET("rendering/driver/depth_prepass/enable");
			if (prepass_enabled) {
				defines += "\n#define TEXTURE_STREAMING_DEPTH_PREPASS\n";
			}
		}
#endif

		{
			//lightmaps
			scene_state.max_lightmaps = MAX_LIGHTMAPS;
			defines += "\n#define MAX_LIGHTMAP_TEXTURES " + itos(scene_state.max_lightmaps) + "\n";
			defines += "\n#define MAX_LIGHTMAPS " + itos(scene_state.max_lightmaps) + "\n";

			scene_state.lightmap_buffer = RD::get_singleton()->storage_buffer_create(sizeof(LightmapData) * scene_state.max_lightmaps);
		}
		{
			//captures
			scene_state.max_lightmap_captures = 2048;
			scene_state.lightmap_captures = memnew_arr(LightmapCaptureData, scene_state.max_lightmap_captures);
			scene_state.lightmap_capture_buffer = RD::get_singleton()->storage_buffer_create(sizeof(LightmapCaptureData) * scene_state.max_lightmap_captures);
		}
		{
			// GodotNext: spline_meshes (#161). 224-byte default SSBO bound when a render list has no spline instances.
			Vector<uint8_t> spline_zero;
			spline_zero.resize(sizeof(SplineDeformEntry));
			spline_zero.fill(0);
			scene_state.default_spline_deform_buffer = RD::get_singleton()->storage_buffer_create(sizeof(SplineDeformEntry), spline_zero);
		}
		{
			defines += "\n#define MATERIAL_UNIFORM_SET " + itos(MATERIAL_UNIFORM_SET) + "\n";
		}
#ifdef REAL_T_IS_DOUBLE
		{
			defines += "\n#define USE_DOUBLE_PRECISION \n";
		}
#endif
		// GodotNext: vsm_projection (#222) / vsm_fog_translucency (#215). Every variant: the light loop samples
		// the maps and the VSM raster variant (#218) draws them.
		defines += RendererVirtualShadowMapStorage::get_glsl_defines();
		if (!RD::get_singleton()->has_feature(RD::SUPPORTS_IMAGE_ATOMIC_32_BIT)) {
			defines += "\n#define VSM_NO_COARSE_MISS_ATOMICS\n";
		}

		scene_shader.init(defines);
	}

	/* shadow sampler */
	{
		RD::SamplerState sampler;
		sampler.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		sampler.min_filter = RD::SAMPLER_FILTER_NEAREST;
		sampler.enable_compare = true;
		sampler.compare_op = RD::COMPARE_OP_GREATER;
		shadow_sampler = RD::get_singleton()->sampler_create(sampler);
	}

	{
		Vector<String> modes;
		modes.push_back("\n");
		best_fit_normal.shader.initialize(modes);
		best_fit_normal.shader_version = best_fit_normal.shader.version_create();
		best_fit_normal.pipeline = RD::get_singleton()->compute_pipeline_create(best_fit_normal.shader.version_get_shader(best_fit_normal.shader_version, 0));

		RD::TextureFormat tformat;
		tformat.format = RD::DATA_FORMAT_R8_UNORM;
		tformat.width = 1024;
		tformat.height = 1024;
		tformat.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		tformat.texture_type = RD::TEXTURE_TYPE_2D;
		best_fit_normal.texture = RD::get_singleton()->texture_create(tformat, RD::TextureView());

		RID shader = best_fit_normal.shader.version_get_shader(best_fit_normal.shader_version, 0);
		ERR_FAIL_COND(shader.is_null());

		Vector<RD::Uniform> uniforms;

		{
			RD::Uniform u;
			u.binding = 0;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(best_fit_normal.texture);
			uniforms.push_back(u);
		}
		RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, shader, 0);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, best_fit_normal.pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, tformat.width, tformat.height, 1);
		RD::get_singleton()->compute_list_end();
	}

	/* DFG LUT */
	{
		Vector<String> modes;
		modes.push_back("\n");
		dfg_lut.shader.initialize(modes);
		dfg_lut.shader_version = dfg_lut.shader.version_create();
		dfg_lut.pipeline = RD::get_singleton()->compute_pipeline_create(dfg_lut.shader.version_get_shader(dfg_lut.shader_version, 0));

		RD::TextureFormat tformat;
		tformat.format = RD::DATA_FORMAT_R16G16_SFLOAT;
		tformat.width = 128;
		tformat.height = 128;
		tformat.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		tformat.texture_type = RD::TEXTURE_TYPE_2D;
		dfg_lut.texture = RD::get_singleton()->texture_create(tformat, RD::TextureView());

		RID shader = dfg_lut.shader.version_get_shader(dfg_lut.shader_version, 0);
		ERR_FAIL_COND(shader.is_null());

		Vector<RD::Uniform> uniforms;

		{
			RD::Uniform u;
			u.binding = 0;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dfg_lut.texture);
			uniforms.push_back(u);
		}
		RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, shader, 0);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, dfg_lut.pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, tformat.width, tformat.height, 1);
		RD::get_singleton()->compute_list_end();
	}

	front_layer.set_owner(&next_gi);
	_update_shader_quality_settings();
	_update_global_pipeline_data_requirements_from_project();

	taa = memnew(RendererRD::TAA);
	fsr2_effect = memnew(RendererRD::FSR2Effect);
	tsr_effect = memnew(RendererRD::TSR);
	ss_effects = memnew(RendererRD::SSEffects);
	motion_vectors_store = memnew(RendererRD::MotionVectorsStore);
	TemporalUpscalerRegistry::initialize_dev_copy_upscaler();
	if (RSG::virtual_mesh_storage != nullptr) {
		RSG::virtual_mesh_storage->set_nextgeo_draw_ready(!bool(GLOBAL_GET("rendering/nextgeo/visibility_only")));
	}
	// GodotNext: vsm_non_nanite (#218). The VSM storage does not exist yet: _vsm_raster_non_nanite()
	// registers the raster check and enables the VSM shader group on first use.
	vsm_cull_instances.set_page_pool(&cull_argument_pool);
#ifdef METAL_MFXTEMPORAL_ENABLED
	mfx_temporal_effect = memnew(RendererRD::MFXTemporalEffect);
#endif
}

RenderForwardClustered::~RenderForwardClustered() {
	// GodotNext: vsm_non_nanite (#218)
	if (RendererRD::VirtualShadowMapStorage::get_singleton() != nullptr) {
		RendererRD::VirtualShadowMapStorage::get_singleton()->set_raster_check_callbacks(nullptr, nullptr, nullptr);
	}
	_free_vsm_raster_resources();
	_free_vsm_projection_resources(); // GodotNext: vsm_projection (#222)
	_free_df_resources(); // GodotNext: df_shadows (#214)
	_free_rt_shadow_default_resources(); // GodotNext: rt_shadows (#141)
	vsm_cull_instances.reset();
	next_gi.free_far_field();
	nextgeo_cut.free_resources();
	nextgeo_resolve.free_resources();
	nextgeo_debug.free_resources();
	TemporalUpscalerRegistry::shutdown();
	if (ss_effects != nullptr) {
		memdelete(ss_effects);
		ss_effects = nullptr;
	}

	if (taa != nullptr) {
		memdelete(taa);
		taa = nullptr;
	}

	if (fsr2_effect) {
		memdelete(fsr2_effect);
		fsr2_effect = nullptr;
	}

	if (tsr_effect) {
		memdelete(tsr_effect);
		tsr_effect = nullptr;
	}

#ifdef METAL_MFXTEMPORAL_ENABLED
	if (mfx_temporal_effect) {
		memdelete(mfx_temporal_effect);
		mfx_temporal_effect = nullptr;
	}
#endif

	if (motion_vectors_store) {
		memdelete(motion_vectors_store);
		motion_vectors_store = nullptr;
	}

	if (rr_guides) {
		memdelete(rr_guides);
		rr_guides = nullptr;
	}

	if (mega_lights) {
		memdelete(mega_lights);
		mega_lights = nullptr;
	}

	RD::get_singleton()->free_rid(shadow_sampler);
	RSG::light_storage->directional_shadow_atlas_set_size(0);

	RD::get_singleton()->free_rid(best_fit_normal.pipeline);
	RD::get_singleton()->free_rid(best_fit_normal.texture);
	best_fit_normal.shader.version_free(best_fit_normal.shader_version);

	RD::get_singleton()->free_rid(dfg_lut.pipeline);
	RD::get_singleton()->free_rid(dfg_lut.texture);
	dfg_lut.shader.version_free(dfg_lut.shader_version);

	if (ltc.lut1_texture.is_valid()) {
		RS::get_singleton()->free_rid(ltc.lut1_texture);
	}
	if (ltc.lut2_texture.is_valid()) {
		RS::get_singleton()->free_rid(ltc.lut2_texture);
	}

	{
		for (const RID &rid : scene_state.uniform_buffers) {
			RD::get_singleton()->free_rid(rid);
		}
		for (const RID &rid : scene_state.implementation_uniform_buffers) {
			RD::get_singleton()->free_rid(rid);
		}
		RD::get_singleton()->free_rid(scene_state.lightmap_buffer);
		RD::get_singleton()->free_rid(scene_state.lightmap_capture_buffer);
		if (scene_state.default_spline_deform_buffer.is_valid()) {
			RD::get_singleton()->free_rid(scene_state.default_spline_deform_buffer);
		}
		for (uint32_t i = 0; i < RENDER_LIST_MAX; i++) {
			scene_state.instance_buffer[i].uninit();
			if (scene_state.spline_deform_buffer[i].is_valid()) {
				RD::get_singleton()->free_rid(scene_state.spline_deform_buffer[i]);
			}
		}
		memdelete_arr(scene_state.lightmap_captures);
	}

	while (sdfgi_framebuffer_size_cache.begin()) {
		RD::get_singleton()->free_rid(sdfgi_framebuffer_size_cache.begin()->value);
		sdfgi_framebuffer_size_cache.remove(sdfgi_framebuffer_size_cache.begin());
	}

	// GodotNext: vsm_non_nanite (#218). A VSM cull-state readback that arrives later finds no renderer.
	singleton = nullptr;
}
