/**************************************************************************/
/*  next_gi_irradiance_field.h                                            */
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

#include "servers/rendering/rendering_device.h"

class RenderDataRD;

namespace RendererRD {

// GodotNext: ps_lumen_lite (#183). Half-resolution irradiance field over WRC levels 0-1.
// The software tier writes the probe buffer from its SDF tracer. The hardware tier packs
// the WRC radiance image. Neither path dispatches screen-probe or reflection rays.
class NextGIIrradianceField {
public:
	static constexpr uint32_t PROBE_COUNT = 8192;
	static constexpr uint32_t PROBES_PER_FRAME = 512;

	NextGIIrradianceField();
	~NextGIIrradianceField();
	NextGIIrradianceField(const NextGIIrradianceField &) = delete;
	NextGIIrradianceField &operator=(const NextGIIrradianceField &) = delete;

	bool ensure_probe_buffer();
	RID get_probe_buffer() const { return probe_buffer; }
	RID get_debug_texture() const { return half_image; }

	uint32_t consume_swrt_probe_base();
	uint32_t consume_hwrt_probe_base();

	// p_hwrt packs the WRC before interpolation. Reflections-only skips the write.
	void integrate(RenderDataRD *p_render_data, bool p_hwrt, bool p_reflections_only);

private:
	struct GpuState;
	GpuState *gpu = nullptr;
	RID probe_buffer;
	RID half_image;
	Size2i half_size;
	uint32_t swrt_cursor = 0;
	uint32_t hwrt_cursor = 0;
	bool shader_failed = false;

	bool _ensure_gpu();
	uint32_t _consume(uint32_t &r_cursor);
};

} // namespace RendererRD
