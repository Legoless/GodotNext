/**************************************************************************/
/*  dlss_ngx_rr.cpp                                                       */
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

// GodotNext: pu_dlss_rr (#206).
// Vulkan NGX DLSS Ray Reconstruction (DLSSD) backend unit.
// Only this translation unit includes nvsdk_ngx_helpers_dlssd_vk.h (from dlss_sdk_path).

#include "dlss_ngx_rr.h"

#if defined(DLSS_BACKEND_NGX) && defined(VULKAN_ENABLED)

#include "core/string/print_string.h"
#include "core/variant/variant.h"

#include <drivers/vulkan/godot_vulkan.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace {

String wstring_to_string_rr(const wchar_t *p_wstr) {
	if (p_wstr == nullptr) {
		return String();
	}
	String out;
	while (*p_wstr != 0) {
		out += static_cast<char32_t>(*p_wstr);
		p_wstr++;
	}
	return out;
}

NVSDK_NGX_PerfQuality_Value quality_to_ngx_rr(DLSSBackend::QualityMode p_mode) {
	switch (p_mode) {
		case DLSSBackend::QUALITY_MAX_PERFORMANCE:
			return NVSDK_NGX_PerfQuality_Value_MaxPerf;
		case DLSSBackend::QUALITY_BALANCED:
			return NVSDK_NGX_PerfQuality_Value_Balanced;
		case DLSSBackend::QUALITY_MAX_QUALITY:
			return NVSDK_NGX_PerfQuality_Value_MaxQuality;
		case DLSSBackend::QUALITY_ULTRA_PERFORMANCE:
			return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
		case DLSSBackend::QUALITY_DLAA:
			return NVSDK_NGX_PerfQuality_Value_DLAA;
		default:
			return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	}
}

NVSDK_NGX_RayReconstruction_Hint_Render_Preset rr_preset_to_ngx(DLSSBackend::RRPreset p_preset) {
	switch (p_preset) {
		case DLSSBackend::RR_PRESET_D:
			return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D;
		case DLSSBackend::RR_PRESET_E:
			return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E;
		case DLSSBackend::RR_PRESET_F:
			return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F;
		case DLSSBackend::RR_PRESET_DEFAULT:
		default:
			return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default;
	}
}

DLSSBackend::Status map_rr_ngx_result(NVSDK_NGX_Result p_res, bool p_for_query) {
	if (NVSDK_NGX_SUCCEED(p_res)) {
		return DLSSBackend::OK;
	}
	switch (p_res) {
		case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
			return DLSSBackend::NOT_NVIDIA_RTX;
		case NVSDK_NGX_Result_FAIL_OutOfDate:
			return DLSSBackend::DRIVER_TOO_OLD;
		case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:
		case NVSDK_NGX_Result_FAIL_FeatureNotFound:
		case NVSDK_NGX_Result_FAIL_PlatformError:
			return p_for_query ? DLSSBackend::RUNTIME_MISSING : DLSSBackend::FEATURE_CREATE_FAILED;
		default:
			return DLSSBackend::FEATURE_CREATE_FAILED;
	}
}

NVSDK_NGX_Resource_VK to_ngx_vk_resource_rr(const DLSSBackend::TextureResource &p_tex) {
	VkImageSubresourceRange range = {};
	VkImageAspectFlags aspect = 0;
	if (p_tex.aspect_mask & 0x1u) {
		aspect |= VK_IMAGE_ASPECT_COLOR_BIT;
	}
	if (p_tex.aspect_mask & 0x2u) {
		aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
	}
	if (p_tex.aspect_mask & 0x4u) {
		aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}
	if (aspect == 0) {
		aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	}
	range.aspectMask = aspect;
	range.baseMipLevel = p_tex.base_mip_level;
	range.levelCount = MAX(1u, p_tex.mip_levels);
	range.baseArrayLayer = p_tex.base_array_layer;
	range.layerCount = MAX(1u, p_tex.array_layers);

	return NVSDK_NGX_Create_ImageView_Resource_VK(
			reinterpret_cast<VkImageView>(p_tex.image_view_handle),
			reinterpret_cast<VkImage>(p_tex.image_handle),
			range,
			static_cast<VkFormat>(p_tex.format),
			p_tex.width,
			p_tex.height,
			p_tex.read_write);
}

void projection_to_float16_rr(const Projection &p_proj, float r_out[16]) {
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			r_out[c * 4 + r] = static_cast<float>(p_proj.columns[c][r]);
		}
	}
}

} // namespace

DLSSBackend::Status dlss_ngx_vk_rr_query_availability(void *p_capability_params, int32_t &r_last_result) {
	NVSDK_NGX_Parameter *cap = static_cast<NVSDK_NGX_Parameter *>(p_capability_params);
	if (cap == nullptr) {
		return DLSSBackend::RUNTIME_MISSING;
	}

	int needs_updated_driver = 0;
	if (NVSDK_NGX_SUCCEED(cap->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needs_updated_driver)) && needs_updated_driver != 0) {
		unsigned int min_major = 0;
		unsigned int min_minor = 0;
		cap->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &min_major);
		cap->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &min_minor);
		print_verbose(vformat("DLSS-RR Vulkan: driver update required (minimum %d.%02d).", int(min_major), int(min_minor)));
		return DLSSBackend::DRIVER_TOO_OLD;
	}

	int rr_available = 0;
	const NVSDK_NGX_Result avail_res = cap->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rr_available);
	if (NVSDK_NGX_FAILED(avail_res) || rr_available == 0) {
		int init_res = static_cast<int>(NVSDK_NGX_Result_FAIL_UnableToInitializeFeature);
		cap->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &init_res);
		r_last_result = init_res;
		const NVSDK_NGX_Result init_enum = static_cast<NVSDK_NGX_Result>(init_res);
		print_verbose(vformat("DLSS-RR Vulkan unavailable: FeatureInitResult=0x%08x (%s)", uint32_t(init_res), wstring_to_string_rr(GetNGXResultAsString(init_enum))));
		return map_rr_ngx_result(init_enum, true);
	}

	return DLSSBackend::OK;
}

DLSSBackend::Status dlss_ngx_vk_rr_create_feature(
		uint64_t p_vk_device,
		uint64_t p_vk_cmd_buf,
		const DLSSBackend::FeatureCreateParams &p_params,
		uint64_t &r_feature_handle,
		int32_t &r_last_result) {
	r_feature_handle = 0;
	VkDevice vk_device = reinterpret_cast<VkDevice>(p_vk_device);
	VkCommandBuffer cmd_buf = reinterpret_cast<VkCommandBuffer>(p_vk_cmd_buf);
	if (vk_device == VK_NULL_HANDLE || cmd_buf == VK_NULL_HANDLE) {
		return DLSSBackend::FEATURE_CREATE_FAILED;
	}

	NVSDK_NGX_Parameter *create_bag = nullptr;
	NVSDK_NGX_Result res = NVSDK_NGX_VULKAN_AllocateParameters(&create_bag);
	r_last_result = static_cast<int32_t>(res);
	if (NVSDK_NGX_FAILED(res) || create_bag == nullptr) {
		return DLSSBackend::FEATURE_CREATE_FAILED;
	}

	const unsigned int preset_val = static_cast<unsigned int>(rr_preset_to_ngx(p_params.rr_preset));
	NVSDK_NGX_Parameter_SetUI(create_bag, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, preset_val);
	NVSDK_NGX_Parameter_SetUI(create_bag, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality, preset_val);
	NVSDK_NGX_Parameter_SetUI(create_bag, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, preset_val);
	NVSDK_NGX_Parameter_SetUI(create_bag, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, preset_val);
	NVSDK_NGX_Parameter_SetUI(create_bag, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, preset_val);
	NVSDK_NGX_Parameter_SetUI(create_bag, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality, preset_val);
	NVSDK_NGX_Parameter_SetI(create_bag, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);

	int flags = NVSDK_NGX_DLSS_Feature_Flags_None;
	if (p_params.is_hdr) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
	}
	if (p_params.depth_inverted) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
	}
	if (p_params.mv_low_res) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
	}
	if (p_params.mv_jittered) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
	}
	if (p_params.auto_exposure) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
	}

	NVSDK_NGX_DLSSD_Create_Params dlssd_create = {};
	dlssd_create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
	dlssd_create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
	dlssd_create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
	dlssd_create.InWidth = uint32_t(MAX(1, p_params.render_size.width));
	dlssd_create.InHeight = uint32_t(MAX(1, p_params.render_size.height));
	dlssd_create.InTargetWidth = uint32_t(MAX(1, p_params.output_size.width));
	dlssd_create.InTargetHeight = uint32_t(MAX(1, p_params.output_size.height));
	dlssd_create.InPerfQualityValue = quality_to_ngx_rr(p_params.quality_mode);
	dlssd_create.InFeatureCreateFlags = flags;
	dlssd_create.InEnableOutputSubrects = false;

	NVSDK_NGX_Handle *ngx_handle = nullptr;
	res = NGX_VULKAN_CREATE_DLSSD_EXT1(vk_device, cmd_buf, 1, 1, &ngx_handle, create_bag, &dlssd_create);
	r_last_result = static_cast<int32_t>(res);
	NVSDK_NGX_VULKAN_DestroyParameters(create_bag);

	if (NVSDK_NGX_FAILED(res) || ngx_handle == nullptr) {
		print_verbose(vformat("DLSS-RR Vulkan create_feature failed: 0x%08x (%s)", uint32_t(res), wstring_to_string_rr(GetNGXResultAsString(res))));
		return map_rr_ngx_result(res, false);
	}

	r_feature_handle = reinterpret_cast<uint64_t>(ngx_handle);
	return DLSSBackend::OK;
}

DLSSBackend::Status dlss_ngx_vk_rr_evaluate(
		uint64_t p_vk_cmd_buf,
		uint64_t p_feature_handle,
		void *p_eval_params,
		const DLSSBackend::EvaluateParams &p_params,
		int32_t &r_last_result) {
	VkCommandBuffer cmd_buf = reinterpret_cast<VkCommandBuffer>(p_vk_cmd_buf);
	NVSDK_NGX_Handle *handle = reinterpret_cast<NVSDK_NGX_Handle *>(p_feature_handle);
	NVSDK_NGX_Parameter *eval_bag = static_cast<NVSDK_NGX_Parameter *>(p_eval_params);
	if (cmd_buf == VK_NULL_HANDLE || handle == nullptr || eval_bag == nullptr) {
		return DLSSBackend::FEATURE_CREATE_FAILED;
	}
	if (p_params.diffuse_albedo.image_handle == 0 || p_params.specular_albedo.image_handle == 0 || p_params.normal_roughness.image_handle == 0) {
		return DLSSBackend::FEATURE_CREATE_FAILED;
	}

	NVSDK_NGX_Resource_VK color_res = to_ngx_vk_resource_rr(p_params.color);
	NVSDK_NGX_Resource_VK depth_res = to_ngx_vk_resource_rr(p_params.depth);
	NVSDK_NGX_Resource_VK mv_res = to_ngx_vk_resource_rr(p_params.velocity);
	NVSDK_NGX_Resource_VK out_res = to_ngx_vk_resource_rr(p_params.output);
	NVSDK_NGX_Resource_VK diffuse_res = to_ngx_vk_resource_rr(p_params.diffuse_albedo);
	NVSDK_NGX_Resource_VK specular_res = to_ngx_vk_resource_rr(p_params.specular_albedo);
	NVSDK_NGX_Resource_VK normal_rough_res = to_ngx_vk_resource_rr(p_params.normal_roughness);
	NVSDK_NGX_Resource_VK hit_dist_res = {};
	const bool has_hit_dist = (p_params.specular_hit_distance.image_handle != 0 && p_params.specular_hit_distance.image_view_handle != 0);
	if (has_hit_dist) {
		hit_dist_res = to_ngx_vk_resource_rr(p_params.specular_hit_distance);
	}

	float world_to_view_mat[16];
	float view_to_clip_mat[16];
	projection_to_float16_rr(Projection(p_params.world_to_view), world_to_view_mat);
	projection_to_float16_rr(p_params.view_to_clip, view_to_clip_mat);

	NVSDK_NGX_VK_DLSSD_Eval_Params dlssd_eval = {};
	dlssd_eval.pInDiffuseAlbedo = &diffuse_res;
	dlssd_eval.pInSpecularAlbedo = &specular_res;
	dlssd_eval.pInNormals = &normal_rough_res;
	dlssd_eval.pInRoughness = nullptr; // Packed in normals.w (NVSDK_NGX_DLSS_Roughness_Mode_Packed).
	dlssd_eval.pInColor = &color_res;
	dlssd_eval.pInOutput = &out_res;
	dlssd_eval.pInDepth = &depth_res;
	dlssd_eval.pInMotionVectors = &mv_res;
	dlssd_eval.pInSpecularHitDistance = has_hit_dist ? &hit_dist_res : nullptr;
	dlssd_eval.InJitterOffsetX = p_params.jitter_offset.x;
	dlssd_eval.InJitterOffsetY = p_params.jitter_offset.y;
	dlssd_eval.InRenderSubrectDimensions.Width = uint32_t(MAX(1, p_params.render_size.width));
	dlssd_eval.InRenderSubrectDimensions.Height = uint32_t(MAX(1, p_params.render_size.height));
	dlssd_eval.InReset = p_params.reset ? 1 : 0;
	dlssd_eval.InMVScaleX = p_params.mv_scale.x;
	dlssd_eval.InMVScaleY = p_params.mv_scale.y;
	dlssd_eval.pInTransparencyMask = nullptr;
	dlssd_eval.pInExposureTexture = nullptr;
	dlssd_eval.pInBiasCurrentColorMask = nullptr;
	dlssd_eval.InPreExposure = p_params.pre_exposure > 0.0f ? p_params.pre_exposure : 1.0f;
	dlssd_eval.InExposureScale = p_params.exposure_scale > 0.0f ? p_params.exposure_scale : 1.0f;
	dlssd_eval.InFrameTimeDeltaInMsec = p_params.frame_time_delta_ms;
	dlssd_eval.pInWorldToViewMatrix = world_to_view_mat;
	dlssd_eval.pInViewToClipMatrix = view_to_clip_mat;

	eval_bag->Reset();
	const NVSDK_NGX_Result res = NGX_VULKAN_EVALUATE_DLSSD_EXT(cmd_buf, handle, eval_bag, &dlssd_eval);
	r_last_result = static_cast<int32_t>(res);
	if (NVSDK_NGX_FAILED(res)) {
		print_verbose(vformat("DLSS-RR Vulkan evaluate failed: 0x%08x (%s)", uint32_t(res), wstring_to_string_rr(GetNGXResultAsString(res))));
		return map_rr_ngx_result(res, false);
	}
	return DLSSBackend::OK;
}

uint64_t dlss_ngx_vk_rr_get_vram_bytes(void *p_capability_params) {
	NVSDK_NGX_Parameter *cap = static_cast<NVSDK_NGX_Parameter *>(p_capability_params);
	if (cap == nullptr) {
		return 0;
	}
	void *callback = nullptr;
	NVSDK_NGX_Parameter_GetVoidPointer(cap, NVSDK_NGX_Parameter_DLSSDGetStatsCallback, &callback);
	if (callback == nullptr) {
		return 0;
	}
	PFN_NVSDK_NGX_DLSS_GetStatsCallback fn = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetStatsCallback>(callback);
	if (NVSDK_NGX_SUCCEED(fn(cap))) {
		unsigned long long vram_bytes = 0;
		if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetULL(cap, NVSDK_NGX_Parameter_SizeInBytes, &vram_bytes))) {
			return static_cast<uint64_t>(vram_bytes);
		}
	}
	return 0;
}

#endif // DLSS_BACKEND_NGX && VULKAN_ENABLED
