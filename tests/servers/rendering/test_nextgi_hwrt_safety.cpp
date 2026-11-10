/**************************************************************************/
/*  test_nextgi_hwrt_safety.cpp                                           */
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

TEST_FORCE_LINK(test_nextgi_hwrt_safety)

#ifdef RD_ENABLED

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/hash_set.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_gather.h"
#include "servers/rendering/rendering_device.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_GLSLANG_ENABLED
#include "servers/rendering/rendering_device_commons.h"

#include "modules/glslang/shader_compile.h"
#endif

using NextGI = RendererRD::NextGI;

namespace TestNextGIHWRTSafety {

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

NextGI::TierInputs hwrt_inputs() {
	NextGI::TierInputs inputs;
	inputs.forward_plus = true;
	inputs.project_enabled = true;
	inputs.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_AUTO;
	inputs.ray_query = true;
	inputs.driver_reason = NextGI::TIER_REASON_NONE;
	inputs.ray_query_hw = true;
	return inputs;
}

NextGI::TierInputs inputs_for_reason(NextGI::TierReason p_reason) {
	NextGI::TierInputs inputs = hwrt_inputs();
	switch (p_reason) {
		case NextGI::NO_RAY_QUERY:
			inputs.ray_query = false;
			inputs.ray_query_hw = false;
			inputs.driver_reason = NextGI::NO_RAY_QUERY;
			break;
		case NextGI::NOT_HW_ACCELERATED:
			inputs.ray_query_hw = false;
			break;
		case NextGI::CPU_DEVICE:
			inputs.cpu_device = true;
			break;
		case NextGI::D3D12_NO_DXR:
			inputs.ray_query = false;
			inputs.ray_query_hw = false;
			inputs.driver_reason = NextGI::D3D12_NO_DXR;
			break;
		case NextGI::METAL_SYNC_MODE:
			inputs.ray_query = false;
			inputs.ray_query_hw = false;
			inputs.driver_reason = NextGI::METAL_SYNC_MODE;
			break;
		case NextGI::METAL_NO_ARGUMENT_BUFFERS:
			inputs.ray_query = false;
			inputs.ray_query_hw = false;
			inputs.driver_reason = NextGI::METAL_NO_ARGUMENT_BUFFERS;
			break;
		case NextGI::SETTING_OFF:
			inputs.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_OFF;
			break;
		case NextGI::AS_MEMORY:
			inputs.as_memory_ok = false;
			break;
		case NextGI::SELFTEST_FAILED:
			inputs.selftest_passed = false;
			break;
		case NextGI::HWRT_FAULTED:
			inputs.fault_latched = true;
			break;
		default:
			break;
	}
	return inputs;
}

struct TempFaultFile {
	String path;
	TempFaultFile() {
		path = OS::get_singleton()->get_temp_path().path_join("godotnext_hwrt_fault_" + itos(OS::get_singleton()->get_ticks_usec()) + ".cfg");
		NextGI::remove_hwrt_fault_file(path);
	}
	~TempFaultFile() {
		NextGI::remove_hwrt_fault_file(path);
	}
};

struct PrintCapture {
	Vector<String> lines;
	PrintHandlerList handler;
	static void _print(void *p_self, const String &p_string, bool p_error, bool p_rich) {
		(void)p_rich;
		if (!p_error) {
			static_cast<PrintCapture *>(p_self)->lines.push_back(p_string);
		}
	}
	PrintCapture() {
		handler.printfunc = _print;
		handler.userdata = this;
		handler.next = nullptr;
		add_print_handler(&handler);
	}
	~PrintCapture() {
		remove_print_handler(&handler);
	}
	int nextgi_lines() const {
		int count = 0;
		for (const String &line : lines) {
			if (line.begins_with("NextGI:")) {
				count++;
			}
		}
		return count;
	}
};

#ifdef MODULE_GLSLANG_ENABLED
uint32_t spirv_word_at(const Vector<uint8_t> &p_spirv, int p_word) {
	const int offset = p_word * 4;
	if (p_spirv.size() < offset + 4) {
		return 0;
	}
	const uint8_t *bytes = p_spirv.ptr();
	return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) | (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
}
#endif

} // namespace TestNextGIHWRTSafety

TEST_CASE("[NextGI] Driver version key format") {
	CHECK_EQ(RenderingDeviceDriver::format_driver_version_key("Apple", "Apple M5", "1.2"), "Apple|Apple M5|1.2");
	CHECK_EQ(RenderingDeviceDriver::format_vulkan_driver_version_key(0x1002u, 0x744cu, 0x00800000u), "4098:29772:8388608");

	RenderingDevice *rd = memnew(RenderingDevice);
	CHECK(rd->get_driver_version_key().is_empty());
	rd->set_device_lost_callback(NextGI::hwrt_device_lost_callback, nullptr);
	memdelete(rd);

	RenderingDevice *live = RenderingDevice::get_singleton();
	if (live != nullptr && live->get_device_driver() != nullptr) {
		const String key = live->get_driver_version_key();
		CHECK_FALSE(key.is_empty());
		if (live->get_device_api_name() == "Vulkan") {
			CHECK_EQ(key.split(":").size(), 3);
		} else {
			CHECK(key.begins_with(live->get_device_vendor_name() + "|"));
			CHECK(key.ends_with("|" + live->get_device_api_version()));
		}
	}
}

TEST_CASE("[NextGI] Startup self-test table is 48 hits and 16 misses") {
	NextGI::HWRTSelfTestRay rays[NextGI::HWRT_SELFTEST_RAY_COUNT];
	NextGI::HWRTSelfTestRecord expected[NextGI::HWRT_SELFTEST_RAY_COUNT];
	NextGI::fill_hwrt_selftest(rays, expected);

	int hits = 0;
	int misses = 0;
	HashSet<int> hit_t_milli;
	for (int i = 0; i < NextGI::HWRT_SELFTEST_RAY_COUNT; i++) {
		if (expected[i].hit == 0) {
			misses++;
			continue;
		}
		hits++;
		const float u = expected[i].bary_u;
		const float v = expected[i].bary_v;
		const float edge = MIN(MIN(u, v), 1.0f - u - v);
		CHECK(edge >= float(NextGI::HWRT_SELFTEST_MIN_EDGE));
		CHECK(rays[i].origin_x == u);
		CHECK(rays[i].origin_y == v);
		CHECK(rays[i].origin_z == expected[i].t);
		CHECK(rays[i].dir_z == -1.0f);
		CHECK_EQ(expected[i].front_face, 1u);
		CHECK_EQ(expected[i].custom_index, 7u);
		hit_t_milli.insert(int(Math::round(double(expected[i].t) * 1000.0)));
	}
	CHECK_EQ(hits, NextGI::HWRT_SELFTEST_HIT_COUNT);
	CHECK_EQ(misses, NextGI::HWRT_SELFTEST_MISS_COUNT);
	CHECK_EQ(hit_t_milli.size(), NextGI::HWRT_SELFTEST_HIT_COUNT);

	String reason;
	CHECK(NextGI::hwrt_selftest_records_match(expected, expected, NextGI::HWRT_SELFTEST_RAY_COUNT, false, &reason));
	CHECK_FALSE(NextGI::hwrt_selftest_records_match(expected, expected, NextGI::HWRT_SELFTEST_RAY_COUNT, true, &reason));
	CHECK(reason.contains("t relative error"));

	NextGI::HWRTSelfTestRecord missed = expected[NextGI::HWRT_SELFTEST_HIT_COUNT];
	missed.hit = 1;
	NextGI::HWRTSelfTestRecord flipped[NextGI::HWRT_SELFTEST_RAY_COUNT];
	for (int i = 0; i < NextGI::HWRT_SELFTEST_RAY_COUNT; i++) {
		flipped[i] = expected[i];
	}
	flipped[NextGI::HWRT_SELFTEST_HIT_COUNT] = missed;
	CHECK_FALSE(NextGI::hwrt_selftest_records_match(flipped, expected, NextGI::HWRT_SELFTEST_RAY_COUNT, false, &reason));
	CHECK(reason.contains("hit"));
}

TEST_CASE("[NextGI] HWRT fault file latches, clears on a driver change, and stays under Force") {
	TestNextGIHWRTSafety::TempFaultFile file;
	REQUIRE(NextGI::write_hwrt_fault_file(file.path, "vendor|device|1.0", 42));
	const NextGI::HWRTFaultFile record = NextGI::read_hwrt_fault_file(file.path);
	CHECK(record.valid);
	CHECK_EQ(record.driver_key, "vendor|device|1.0");
	CHECK_EQ(record.time, 42);

	const NextGI::HWRTFaultLatch latched = NextGI::resolve_hwrt_fault_latch(file.path, "vendor|device|1.0", NextGI::HARDWARE_RAY_TRACING_AUTO);
	CHECK(latched.latched);
	CHECK(latched.file_present);
	CHECK(FileAccess::exists(file.path));

	const NextGI::HWRTFaultLatch forced = NextGI::resolve_hwrt_fault_latch(file.path, "vendor|device|1.0", NextGI::HARDWARE_RAY_TRACING_FORCE);
	CHECK_FALSE(forced.latched);
	CHECK(forced.file_present);
	CHECK(FileAccess::exists(file.path));

	const NextGI::HWRTFaultLatch empty_key = NextGI::resolve_hwrt_fault_latch(file.path, String(), NextGI::HARDWARE_RAY_TRACING_AUTO);
	CHECK_FALSE(empty_key.latched);
	CHECK(FileAccess::exists(file.path));

	NextGI::TierInputs selftest_first = TestNextGIHWRTSafety::hwrt_inputs();
	selftest_first.selftest_passed = false;
	selftest_first.fault_latched = latched.latched;
	CHECK_EQ(NextGI::evaluate_tier(selftest_first).reason, NextGI::SELFTEST_FAILED);

	NextGI::TierInputs faulted = TestNextGIHWRTSafety::hwrt_inputs();
	faulted.fault_latched = latched.latched;
	CHECK_EQ(NextGI::evaluate_tier(faulted).reason, NextGI::HWRT_FAULTED);

	NextGI::TierInputs forced_inputs = TestNextGIHWRTSafety::hwrt_inputs();
	forced_inputs.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_FORCE;
	forced_inputs.fault_latched = forced.latched;
	CHECK_EQ(NextGI::evaluate_tier(forced_inputs).tier, NextGI::TIER_HWRT);

	const NextGI::HWRTFaultLatch cleared = NextGI::resolve_hwrt_fault_latch(file.path, "other|device|2.0", NextGI::HARDWARE_RAY_TRACING_AUTO);
	CHECK_FALSE(cleared.latched);
	CHECK_FALSE(cleared.file_present);
	CHECK_FALSE(FileAccess::exists(file.path));

	Ref<FileAccess> garbage = FileAccess::open(file.path, FileAccess::WRITE);
	REQUIRE(garbage.is_valid());
	garbage->store_string("not a latch\n");
	garbage->flush();
	garbage.unref();
	const NextGI::HWRTFaultLatch invalid = NextGI::resolve_hwrt_fault_latch(file.path, "vendor|device|1.0", NextGI::HARDWARE_RAY_TRACING_AUTO);
	CHECK_FALSE(invalid.latched);
	CHECK(FileAccess::exists(file.path));
}

TEST_CASE("[NextGI] Device-lost callback writes the fault file and does not touch RenderingDevice") {
	TestNextGIHWRTSafety::TempFaultFile file;
	NextGI::HWRTFaultCallbackState state;
	state.path = file.path;
	state.driver_key = "callback-key";
	NextGI::hwrt_device_lost_callback(&state);
	const NextGI::HWRTFaultFile record = NextGI::read_hwrt_fault_file(file.path);
	CHECK(record.valid);
	CHECK_EQ(record.driver_key, "callback-key");
	CHECK(record.time > 0);

	NextGI::hwrt_device_lost_callback(nullptr);
	CHECK(FileAccess::exists(file.path));
}

TEST_CASE("[NextGI] Each tier reason prints exactly one NextGI line") {
	const NextGI::TierReason reasons[10] = {
		NextGI::NO_RAY_QUERY,
		NextGI::NOT_HW_ACCELERATED,
		NextGI::CPU_DEVICE,
		NextGI::D3D12_NO_DXR,
		NextGI::METAL_SYNC_MODE,
		NextGI::METAL_NO_ARGUMENT_BUFFERS,
		NextGI::SETTING_OFF,
		NextGI::AS_MEMORY,
		NextGI::SELFTEST_FAILED,
		NextGI::HWRT_FAULTED,
	};
	for (NextGI::TierReason reason : reasons) {
		NextGI next_gi;
		String line;
		REQUIRE(next_gi.update_tier(TestNextGIHWRTSafety::inputs_for_reason(reason), NextGI::QUALITY_AUTO, line));
		CHECK_EQ(next_gi.get_tier_result().reason, reason);
		TestNextGIHWRTSafety::PrintCapture capture;
		next_gi.log_tier_selection(line);
		CHECK_EQ(capture.nextgi_lines(), 1);
		CHECK_EQ(capture.lines[0], line);
		CHECK_EQ(TestNextGIHWRTSafety::count_occurrences(line, NextGI::get_reason_name(reason)), 1);

		String again;
		CHECK_FALSE(next_gi.update_tier(TestNextGIHWRTSafety::inputs_for_reason(reason), NextGI::QUALITY_AUTO, again));
		CHECK_EQ(capture.nextgi_lines(), 1);
	}
}

// GodotNext: ps_tier_rule (#187). The auto quality choice and its reason ride in the one tier
// line (plan §3.1), so startup on the software tier prints exactly one NextGI line.
TEST_CASE("[NextGI] Software-tier startup prints one NextGI line that carries the auto reason") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	struct Override {
		ProjectSettings *ps = nullptr;
		String name;
		Variant previous;
		Override(ProjectSettings *p_ps, const String &p_name, const Variant &p_value) :
				ps(p_ps), name(p_name), previous(p_ps->get_setting(p_name)) {
			ps->set_setting(name, p_value);
		}
		~Override() {
			ps->set_setting(name, previous);
		}
	};
	// auto_quality and auto_reason are process-wide; other cases expect them unresolved.
	struct AutoRestore {
		NextGI::Quality quality = NextGI::auto_quality;
		String reason = NextGI::auto_reason;
		~AutoRestore() {
			NextGI::auto_quality = quality;
			NextGI::auto_reason = reason;
		}
	} auto_restore;
	NextGI::auto_quality = NextGI::QUALITY_AUTO;
	NextGI::auto_reason = String();

	Override enabled(ps, "rendering/global_illumination/nextgi/enabled", true);
	Override hwrt(ps, "rendering/global_illumination/nextgi/hardware_ray_tracing", int(NextGI::HARDWARE_RAY_TRACING_OFF));
	Override quality(ps, "rendering/global_illumination/nextgi/quality", int(NextGI::QUALITY_AUTO));

	NextGI next_gi;
	TestNextGIHWRTSafety::PrintCapture capture;
	next_gi.init(true);
	CHECK_EQ(next_gi.get_tier_result().tier, NextGI::TIER_SOFTWARE);
	CHECK_EQ(next_gi.get_tier_result().reason, NextGI::SETTING_OFF);
	REQUIRE_FALSE(NextGI::auto_reason.is_empty());
	CHECK_NE(NextGI::auto_quality, NextGI::QUALITY_AUTO);
	CHECK_EQ(capture.nextgi_lines(), 1);
	String tier_line;
	for (const String &line : capture.lines) {
		if (line.begins_with("NextGI:")) {
			tier_line = line;
		}
	}
	CHECK(tier_line.begins_with("NextGI: tier=SWRT reason=SETTING_OFF quality="));
	CHECK(tier_line.ends_with("(" + NextGI::auto_reason + ")"));
}

TEST_CASE("[NextGI] HWRT self-test source follows the ray-query rules") {
	const String source = NextGI::build_hwrt_selftest_source();
	REQUIRE_FALSE(source.is_empty());
	REQUIRE(source.contains("nextgi_trace_opaque"));
	REQUIRE(source.contains("#version 460"));
	REQUIRE(source.contains("GL_EXT_ray_query"));
	CHECK_EQ(TestNextGIHWRTSafety::count_occurrences(source, "rayQueryProceedEXT"), 1);
	CHECK_FALSE(source.contains("while"));
	CHECK_FALSE(source.contains("rayQueryGetIntersectionTriangleVertexPositionsEXT"));
	CHECK_FALSE(source.contains("ShaderBindingTableRecordOffset"));
	CHECK_FALSE(source.contains("ray_tracing_position_fetch"));
}

TEST_CASE("[NextGI] diffuse gather source follows the ray-query rules") {
	const String source = RendererRD::NextGIGather::build_shader_source();
	REQUIRE_FALSE(source.is_empty());
	REQUIRE(source.contains("local_size_x = 8"));
	REQUIRE(source.contains("local_size_y = 8"));
	REQUIRE(source.contains("nextgi_trace("));
	CHECK_EQ(TestNextGIHWRTSafety::count_occurrences(source, "rayQueryProceedEXT("), 1);
	CHECK_FALSE(source.contains("shared uint"));
	CHECK_FALSE(source.contains("shared float"));
	CHECK_FALSE(source.contains("shared int"));
	CHECK_FALSE(source.contains("shared vec"));
	CHECK_FALSE(source.contains("rayQueryGetIntersectionTriangleVertexPositionsEXT"));
	CHECK_FALSE(source.contains("GL_EXT_buffer_reference"));
	CHECK_FALSE(source.contains("while"));

	const String diagnostic = RendererRD::NextGIGather::build_shader_source(4);
	REQUIRE_FALSE(diagnostic.is_empty());
	REQUIRE(diagnostic.contains("nextgi_pt_radiance("));
	CHECK_EQ(TestNextGIHWRTSafety::count_occurrences(diagnostic, "rayQueryProceedEXT("), 1);
	CHECK_FALSE(diagnostic.contains("while"));
}

#ifdef MODULE_GLSLANG_ENABLED
TEST_CASE("[NextGI] HWRT self-test compiles through glslang to SPIR-V 1.3") {
	const String source = NextGI::build_hwrt_selftest_source();
	REQUIRE_FALSE(source.is_empty());

	String error;
	const Vector<uint8_t> spirv = compile_glslang_shader(
			RenderingDeviceCommons::SHADER_STAGE_COMPUTE,
			source,
			RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1,
			RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_3,
			&error);
	if (spirv.is_empty()) {
		FAIL_CHECK(("glslang failed to compile the HWRT self-test:\n" + error).utf8().get_data());
	}
	REQUIRE_GE(spirv.size(), 20);
	CHECK_EQ(TestNextGIHWRTSafety::spirv_word_at(spirv, 0), 0x07230203u);
	CHECK_EQ(TestNextGIHWRTSafety::spirv_word_at(spirv, 1), 0x00010300u);
}

TEST_CASE("[NextGI] diffuse gather compiles through glslang to SPIR-V 1.3") {
	const String source = RendererRD::NextGIGather::build_shader_source();
	REQUIRE_FALSE(source.is_empty());

	String error;
	const Vector<uint8_t> spirv = compile_glslang_shader(
			RenderingDeviceCommons::SHADER_STAGE_COMPUTE,
			source,
			RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1,
			RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_3,
			&error);
	if (spirv.is_empty()) {
		print_line(error);
		FAIL_CHECK("glslang failed to compile the NextGI gather");
	}
	REQUIRE_GE(spirv.size(), 20);
	CHECK_EQ(TestNextGIHWRTSafety::spirv_word_at(spirv, 0), 0x07230203u);
}
#endif // MODULE_GLSLANG_ENABLED

#endif // RD_ENABLED
