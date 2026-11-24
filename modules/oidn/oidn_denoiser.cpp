/**************************************************************************/
/*  oidn_denoiser.cpp                                                     */
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

namespace OIDNModule {

Error validate_input(const RendererRD::PathTracerOIDNInput &p_input, String &r_error) {
	if (p_input.width == 0 || p_input.height == 0) {
		r_error = "OIDN input dimensions must be positive.";
		return ERR_INVALID_PARAMETER;
	}
	const uint64_t pixel_count = uint64_t(p_input.width) * uint64_t(p_input.height);
	if (pixel_count > (UINT32_MAX / 3u)) {
		r_error = "OIDN input image dimensions exceed supported buffer limits.";
		return ERR_INVALID_PARAMETER;
	}
	if (p_input.color == nullptr || p_input.output == nullptr) {
		r_error = "OIDN input color and output buffers must not be null.";
		return ERR_INVALID_PARAMETER;
	}
	return OK;
}

Error denoise(const RendererRD::PathTracerOIDNInput &p_input, String &r_error) {
	const Error valid = validate_input(p_input, r_error);
	if (valid != OK) {
		return valid;
	}
	return denoise_impl(p_input, r_error);
}

const RendererRD::PathTracerOIDNBackend *get_backend() {
	static const RendererRD::PathTracerOIDNBackend backend = {
		backend_name(),
		&denoise,
	};
	return &backend;
}

} // namespace OIDNModule
