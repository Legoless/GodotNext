/**************************************************************************/
/*  oidn_denoiser_real.cpp                                                */
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

#include "oidn_denoiser.h"

#include "core/os/mutex.h"

// Developer-supplied Intel OpenImageDenoise SDK header from oidn_sdk_path (plan §19; never committed).
#include <OpenImageDenoise/oidn.h>

namespace OIDNModule {

namespace {

Mutex g_oidn_mutex;
OIDNDevice g_oidn_device = nullptr;

bool _ensure_device(String &r_error) {
	if (g_oidn_device != nullptr) {
		return true;
	}
	g_oidn_device = oidnNewDevice(OIDN_DEVICE_TYPE_DEFAULT);
	if (g_oidn_device == nullptr) {
		r_error = "Failed to create OIDN device.";
		return false;
	}
	oidnCommitDevice(g_oidn_device);
	const char *err_msg = nullptr;
	if (oidnGetDeviceError(g_oidn_device, &err_msg) != OIDN_ERROR_NONE) {
		r_error = err_msg != nullptr ? String::utf8(err_msg) : String("OIDN device commit failed.");
		oidnReleaseDevice(g_oidn_device);
		g_oidn_device = nullptr;
		return false;
	}
	return true;
}

} // namespace

const char *backend_name() {
	return "oidn";
}

bool is_mock_backend() {
	return false;
}

MockStats get_mock_stats() {
	return MockStats();
}

void reset_mock_stats() {
}

void set_mock_fail_next(bool p_fail, const String &p_message) {
	(void)p_fail;
	(void)p_message;
}

Error denoise_impl(const RendererRD::PathTracerOIDNInput &p_input, String &r_error) {
	MutexLock lock(g_oidn_mutex);
	if (!_ensure_device(r_error)) {
		return ERR_CANT_CREATE;
	}

	OIDNFilter filter = oidnNewFilter(g_oidn_device, "RT");
	if (filter == nullptr) {
		const char *err_msg = nullptr;
		oidnGetDeviceError(g_oidn_device, &err_msg);
		r_error = err_msg != nullptr ? String::utf8(err_msg) : String("Failed to create OIDN RT filter.");
		return ERR_CANT_CREATE;
	}

	const size_t width = size_t(p_input.width);
	const size_t height = size_t(p_input.height);
	const size_t byte_size = width * height * 3u * sizeof(float);

#if defined(OIDN_VERSION_MAJOR) && OIDN_VERSION_MAJOR >= 2
	// OIDN 2.x buffer API works transparently across CPU, CUDA, HIP, SYCL and Metal devices.
	OIDNBuffer color_buf = oidnNewBuffer(g_oidn_device, byte_size);
	OIDNBuffer output_buf = oidnNewBuffer(g_oidn_device, byte_size);
	OIDNBuffer albedo_buf = p_input.albedo != nullptr ? oidnNewBuffer(g_oidn_device, byte_size) : nullptr;
	OIDNBuffer normal_buf = p_input.normal != nullptr ? oidnNewBuffer(g_oidn_device, byte_size) : nullptr;

	bool buffers_ok = color_buf != nullptr && output_buf != nullptr &&
			(p_input.albedo == nullptr || albedo_buf != nullptr) &&
			(p_input.normal == nullptr || normal_buf != nullptr);
	if (!buffers_ok) {
		const char *err_msg = nullptr;
		oidnGetDeviceError(g_oidn_device, &err_msg);
		r_error = err_msg != nullptr ? String::utf8(err_msg) : String("Failed to allocate OIDN buffers.");
		if (normal_buf != nullptr) {
			oidnReleaseBuffer(normal_buf);
		}
		if (albedo_buf != nullptr) {
			oidnReleaseBuffer(albedo_buf);
		}
		if (output_buf != nullptr) {
			oidnReleaseBuffer(output_buf);
		}
		if (color_buf != nullptr) {
			oidnReleaseBuffer(color_buf);
		}
		oidnReleaseFilter(filter);
		return ERR_OUT_OF_MEMORY;
	}

	oidnWriteBuffer(color_buf, 0, byte_size, p_input.color);
	oidnSetFilterImage(filter, "color", color_buf, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
	if (albedo_buf != nullptr) {
		oidnWriteBuffer(albedo_buf, 0, byte_size, p_input.albedo);
		oidnSetFilterImage(filter, "albedo", albedo_buf, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
	}
	if (normal_buf != nullptr) {
		oidnWriteBuffer(normal_buf, 0, byte_size, p_input.normal);
		oidnSetFilterImage(filter, "normal", normal_buf, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
	}
	oidnSetFilterImage(filter, "output", output_buf, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
	oidnSetFilterBool(filter, "hdr", p_input.hdr);
	oidnSetFilterBool(filter, "srgb", p_input.srgb);
	oidnSetFilterBool(filter, "cleanAux", p_input.clean_aux);
	oidnCommitFilter(filter);
	oidnExecuteFilter(filter);

	const char *err_msg = nullptr;
	const OIDNError oidn_err = oidnGetDeviceError(g_oidn_device, &err_msg);
	if (oidn_err == OIDN_ERROR_NONE) {
		oidnReadBuffer(output_buf, 0, byte_size, p_input.output);
	} else {
		r_error = err_msg != nullptr ? String::utf8(err_msg) : String("OIDN filter execution failed.");
	}

	if (normal_buf != nullptr) {
		oidnReleaseBuffer(normal_buf);
	}
	if (albedo_buf != nullptr) {
		oidnReleaseBuffer(albedo_buf);
	}
	oidnReleaseBuffer(output_buf);
	oidnReleaseBuffer(color_buf);
	oidnReleaseFilter(filter);
	return oidn_err == OIDN_ERROR_NONE ? OK : FAILED;
#else
	(void)byte_size;
	oidnSetSharedFilterImage(filter, "color", const_cast<float *>(p_input.color), OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
	if (p_input.albedo != nullptr) {
		oidnSetSharedFilterImage(filter, "albedo", const_cast<float *>(p_input.albedo), OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
	}
	if (p_input.normal != nullptr) {
		oidnSetSharedFilterImage(filter, "normal", const_cast<float *>(p_input.normal), OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
	}
	oidnSetSharedFilterImage(filter, "output", p_input.output, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
	oidnSetFilter1b(filter, "hdr", p_input.hdr);
	oidnSetFilter1b(filter, "srgb", p_input.srgb);
	oidnSetFilter1b(filter, "cleanAux", p_input.clean_aux);
	oidnCommitFilter(filter);
	oidnExecuteFilter(filter);

	const char *err_msg = nullptr;
	const OIDNError oidn_err = oidnGetDeviceError(g_oidn_device, &err_msg);
	if (oidn_err != OIDN_ERROR_NONE) {
		r_error = err_msg != nullptr ? String::utf8(err_msg) : String("OIDN filter execution failed.");
	}
	oidnReleaseFilter(filter);
	return oidn_err == OIDN_ERROR_NONE ? OK : FAILED;
#endif
}

void shutdown() {
	MutexLock lock(g_oidn_mutex);
	if (g_oidn_device != nullptr) {
		oidnReleaseDevice(g_oidn_device);
		g_oidn_device = nullptr;
	}
}

} // namespace OIDNModule
