/**************************************************************************/
/*  test_nextgi_rq_toolchain.cpp                                          */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_nextgi_rq_toolchain)

#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_commons.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_GLSLANG_ENABLED
#include "modules/glslang/shader_compile.h"
#endif

namespace TestNextGIRqToolchain {

int count_occurrences(const String &p_text, const String &p_needle) {
	int count = 0;
	int from = 0;
	while (true) {
		const int at = p_text.find(p_needle, from);
		if (at < 0) {
			break;
		}
		count++;
		from = at + p_needle.length();
	}
	return count;
}

uint32_t spirv_word_at(const Vector<uint8_t> &p_spirv, int p_word) {
	const int offset = p_word * 4;
	if (p_spirv.size() < offset + 4) {
		return 0;
	}
	const uint8_t *bytes = p_spirv.ptr();
	return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) | (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
}

} // namespace TestNextGIRqToolchain

TEST_CASE("[NextGI] Ray-query self-test compiles through glslang to SPIR-V 1.3") {
	const String source = RendererRD::NextGI::build_toolchain_selftest_source();
	REQUIRE_FALSE(source.is_empty());
	REQUIRE(source.contains("nextgi_trace_opaque"));
	REQUIRE(source.contains("#version 460"));
	REQUIRE(source.contains("GL_EXT_ray_query"));
	CHECK_EQ(TestNextGIRqToolchain::count_occurrences(source, "rayQueryProceedEXT"), 1);
	CHECK_FALSE(source.contains("while"));
	CHECK_FALSE(source.contains("rayQueryGetIntersectionTriangleVertexPositionsEXT"));
	CHECK_FALSE(source.contains("ShaderBindingTableRecordOffset"));
	CHECK_FALSE(source.contains("ray_tracing_position_fetch"));

#ifndef MODULE_GLSLANG_ENABLED
	REQUIRE_MESSAGE(false, "The glslang module is required to compile the ray-query self-test.");
#else
	// Same targets as Vulkan compute in rendering_shader_container_vulkan.cpp: Vulkan 1.1, SPIR-V 1.3.
	// The Metal container targets SPIR-V 1.6, and run_toolchain_selftest does not compile on Metal today.
	String error;
	const Vector<uint8_t> spirv = compile_glslang_shader(
			RenderingDeviceCommons::SHADER_STAGE_COMPUTE,
			source,
			RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1,
			RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_3,
			&error);
	if (spirv.is_empty()) {
		FAIL_CHECK(("glslang failed to compile the ray-query self-test:\n" + error).utf8().get_data());
	}
	REQUIRE_GE(spirv.size(), 20);
	CHECK_EQ(TestNextGIRqToolchain::spirv_word_at(spirv, 0), 0x07230203u);
	const uint32_t spirv_version = TestNextGIRqToolchain::spirv_word_at(spirv, 1);
	print_line(vformat("nextgi spirv version word 0x%08x", spirv_version));
	CHECK_EQ(spirv_version, 0x00010300u);

	// CI runners have no spirv-val or spirv-cross. The module is written only when
	// misc/scripts/godotnext_rq_toolchain_check.sh asks for it.
	const String spirv_out = OS::get_singleton()->get_environment("GODOTNEXT_RQ_SPIRV_OUT");
	if (!spirv_out.is_empty()) {
		Ref<FileAccess> out = FileAccess::open(spirv_out, FileAccess::WRITE);
		REQUIRE_MESSAGE(out.is_valid(), "GODOTNEXT_RQ_SPIRV_OUT could not be opened for writing.");
		REQUIRE(out->store_buffer(spirv));
	}
#endif
}

TEST_CASE("[NextGI] Devices without ray query report NO_RAY_QUERY and skip the ShaderRD") {
	String report;
	const bool pass = RendererRD::NextGI::run_toolchain_selftest(report);
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr && rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY)) {
		CHECK(report.contains("\"shader\":\"NextgiRqSelftestShaderRD\""));
		CHECK(report.contains("\"pipeline\":"));
		CHECK(report.contains("\"spirv_word\":\"0x"));
		CHECK_EQ(pass, report.contains("\"pipeline\":true"));
	} else {
		CHECK_FALSE(pass);
		CHECK_EQ(report, String("{\"pass\":false,\"reason\":\"NO_RAY_QUERY\"}"));
	}
}
