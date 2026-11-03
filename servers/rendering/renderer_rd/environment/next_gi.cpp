/**************************************************************************/
/*  next_gi.cpp                                                           */
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

#include "next_gi.h"

#include "servers/rendering/renderer_rd/shader_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_rq_selftest.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

static uint32_t _nextgi_spirv_version_word(const Vector<uint8_t> &p_spirv) {
	if (p_spirv.size() < 8) {
		return 0;
	}
	const uint8_t *bytes = p_spirv.ptr();
	return uint32_t(bytes[4]) | (uint32_t(bytes[5]) << 8) | (uint32_t(bytes[6]) << 16) | (uint32_t(bytes[7]) << 24);
}

String RendererRD::NextGI::build_toolchain_selftest_source() {
	NextgiRqSelftestShaderRD shader;
	shader.initialize({ "" });
	RID version = shader.version_create(false);
	ERR_FAIL_COND_V(!version.is_valid(), String());

	Vector<String> sources = shader.version_build_variant_stage_sources(version, 0);
	shader.version_free(version);

	if (sources.size() <= RenderingDevice::SHADER_STAGE_COMPUTE) {
		return String();
	}
	return sources[RenderingDevice::SHADER_STAGE_COMPUTE];
}

bool RendererRD::NextGI::run_toolchain_selftest(String &r_report) {
	// Metal and D3D12 do not expose ray query yet (#48, #47, PD). Leave the ShaderRD untouched.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || !rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY)) {
		r_report = "{\"pass\":false,\"reason\":\"NO_RAY_QUERY\"}";
		return false;
	}

	NextgiRqSelftestShaderRD shader;
	shader.initialize({ "" });
	RID version = shader.version_create(false);
	if (!version.is_valid()) {
		r_report = vformat("{\"pass\":false,\"reason\":\"COMPILE_FAILED\",\"shader\":\"%s\"}", shader.get_name());
		return false;
	}

	// Non-embedded: the shader baker never bakes this self-test.
	RID shader_rid = shader.version_get_shader(version, 0);

	uint32_t spirv_word = 0;
	{
		Vector<String> sources = shader.version_build_variant_stage_sources(version, 0);
		Vector<RenderingDevice::ShaderStageSPIRVData> stages = ShaderRD::compile_stages(sources, Vector<uint64_t>());
		for (int i = 0; i < stages.size(); i++) {
			if (stages[i].shader_stage == RenderingDevice::SHADER_STAGE_COMPUTE) {
				spirv_word = _nextgi_spirv_version_word(stages[i].spirv);
				break;
			}
		}
	}

	RID pipeline;
	if (shader_rid.is_valid()) {
		pipeline = rd->compute_pipeline_create(shader_rid);
	}
	const bool pipeline_ok = pipeline.is_valid();
	if (pipeline_ok) {
		rd->free_rid(pipeline);
	}
	shader.version_free(version);

	const uint32_t major = (spirv_word >> 16) & 0xff;
	const uint32_t minor = (spirv_word >> 8) & 0xff;
	r_report = vformat(
			"{\"pass\":%s,\"spirv\":\"%d.%d\",\"spirv_word\":\"0x%08x\",\"pipeline\":%s,\"shader\":\"%s\"}",
			pipeline_ok ? "true" : "false",
			major,
			minor,
			spirv_word,
			pipeline_ok ? "true" : "false",
			shader.get_name());
	return pipeline_ok;
}
