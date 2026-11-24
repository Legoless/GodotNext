/**************************************************************************/
/*  dlss_ngx_rr.h                                                         */
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

// GodotNext: pu_dlss_rr (#206).
// Pure MIT header for Vulkan NGX DLSS Ray Reconstruction (DLSSD).
// Contains no Vulkan or proprietary NGX SDK headers.

#include "dlss_backend.h"

#ifdef DLSS_BACKEND_NGX
DLSSBackend::Status dlss_ngx_vk_rr_query_availability(void *p_capability_params, int32_t &r_last_result);
DLSSBackend::Status dlss_ngx_vk_rr_create_feature(
		uint64_t p_vk_device,
		uint64_t p_vk_cmd_buf,
		const DLSSBackend::FeatureCreateParams &p_params,
		uint64_t &r_feature_handle,
		int32_t &r_last_result);
DLSSBackend::Status dlss_ngx_vk_rr_evaluate(
		uint64_t p_vk_cmd_buf,
		uint64_t p_feature_handle,
		void *p_eval_params,
		const DLSSBackend::EvaluateParams &p_params,
		int32_t &r_last_result);
uint64_t dlss_ngx_vk_rr_get_vram_bytes(void *p_capability_params);
#endif
