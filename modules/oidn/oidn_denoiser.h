/**************************************************************************/
/*  oidn_denoiser.h                                                       */
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

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "servers/rendering/renderer_rd/effects/path_tracer.h"

#include <cstdint>

namespace OIDNModule {

struct MockStats {
	uint64_t calls = 0;
	uint32_t last_width = 0;
	uint32_t last_height = 0;
	bool last_had_albedo = false;
	bool last_had_normal = false;
	bool last_hdr = false;
	bool last_clean_aux = false;
};

// Shared input validation before dispatching to the active backend ("oidn" or "mock").
Error validate_input(const RendererRD::PathTracerOIDNInput &p_input, String &r_error);

// Backend entry points implemented by oidn_denoiser_mock.cpp or oidn_denoiser_real.cpp.
const char *backend_name();
bool is_mock_backend();
Error denoise_impl(const RendererRD::PathTracerOIDNInput &p_input, String &r_error);
void shutdown();

// Mock telemetry and error-injection hooks (no-ops in the real OIDN backend).
MockStats get_mock_stats();
void reset_mock_stats();
void set_mock_fail_next(bool p_fail, const String &p_message = String());

// Public entry point registered on RendererRD::PathTracer.
Error denoise(const RendererRD::PathTracerOIDNInput &p_input, String &r_error);
const RendererRD::PathTracerOIDNBackend *get_backend();

} // namespace OIDNModule
