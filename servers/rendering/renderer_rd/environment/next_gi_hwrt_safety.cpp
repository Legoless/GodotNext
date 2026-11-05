/**************************************************************************/
/*  next_gi_hwrt_safety.cpp                                               */
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

#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/math/transform_3d.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/shader_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_selftest.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

#include <cstddef>
#include <cstring>

using namespace RendererRD;

static_assert(sizeof(NextGI::HWRTSelfTestRay) == 32, "The self-test ray is one std430 vec4 pair.");
static_assert(sizeof(NextGI::HWRTSelfTestRecord) == 32, "The self-test record is 32 bytes.");
static_assert(offsetof(NextGI::HWRTSelfTestRecord, bary_u) == 16, "Barycentrics follow the four ID words.");
static_assert(offsetof(NextGI::HWRTSelfTestRecord, hit) == 28, "The hit flag is the last word.");

// Cached per RenderingDevice. A faulted driver never reaches this: the latch is checked first.
struct NextGISelfTestCache {
	RenderingDevice *device = nullptr;
	bool passed = false;
	uint64_t usec = 0;
	bool valid = false;
};

static NextGISelfTestCache nextgi_selftest_cache;

static bool _nextgi_earlier_hwrt_checks_pass(const NextGI::TierInputs &p_inputs) {
	if (!p_inputs.forward_plus || !p_inputs.project_enabled) {
		return false;
	}
	if (p_inputs.hwrt_setting == NextGI::HARDWARE_RAY_TRACING_OFF) {
		return false;
	}
	if (!p_inputs.ray_query) {
		return false;
	}
	if (p_inputs.hwrt_setting != NextGI::HARDWARE_RAY_TRACING_FORCE) {
		if (p_inputs.cpu_device || !p_inputs.ray_query_hw) {
			return false;
		}
	}
	return true;
}

#ifdef DEBUG_ENABLED
static bool _nextgi_selftest_corrupt_enabled() {
	return OS::get_singleton() != nullptr && OS::get_singleton()->get_environment("GODOTNEXT_NEXTGI_SELFTEST_CORRUPT") == "1";
}
#endif

String NextGI::hwrt_fault_file_path() {
	return "user://nextgi_hwrt_fault.cfg";
}

bool NextGI::write_hwrt_fault_file(const String &p_path, const String &p_driver_key, int64_t p_time) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}
	file->store_line("driver_key=" + p_driver_key);
	file->store_line("time=" + itos(p_time));
	file->flush();
	return true;
}

NextGI::HWRTFaultFile NextGI::read_hwrt_fault_file(const String &p_path) {
	HWRTFaultFile record;
	if (!FileAccess::exists(p_path)) {
		return record;
	}
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return record;
	}
	bool saw_key = false;
	bool saw_time = false;
	while (file->get_position() < file->get_length()) {
		const String line = file->get_line().strip_edges();
		if (line.begins_with("driver_key=")) {
			record.driver_key = line.trim_prefix("driver_key=");
			saw_key = true;
		} else if (line.begins_with("time=")) {
			const String text = line.trim_prefix("time=");
			if (!text.is_valid_int()) {
				return HWRTFaultFile();
			}
			record.time = text.to_int();
			saw_time = true;
		}
	}
	record.valid = saw_key && saw_time && !record.driver_key.is_empty();
	return record;
}

void NextGI::remove_hwrt_fault_file(const String &p_path) {
	String path = p_path;
	if (p_path.contains("://") && ProjectSettings::get_singleton() != nullptr) {
		path = ProjectSettings::get_singleton()->globalize_path(p_path);
	}
	DirAccess::remove_absolute(path);
}

NextGI::HWRTFaultLatch NextGI::resolve_hwrt_fault_latch(const String &p_path, const String &p_driver_key, HardwareRayTracing p_setting) {
	HWRTFaultLatch latch;
	const HWRTFaultFile record = read_hwrt_fault_file(p_path);
	if (!record.valid || p_driver_key.is_empty()) {
		return latch;
	}
	if (record.driver_key != p_driver_key) {
		remove_hwrt_fault_file(p_path);
		return latch;
	}
	latch.file_present = true;
	// Force ignores the latch for tier selection and leaves the file in place.
	if (p_setting != HARDWARE_RAY_TRACING_FORCE) {
		latch.latched = true;
	}
	return latch;
}

void NextGI::hwrt_device_lost_callback(void *p_userdata) {
	const HWRTFaultCallbackState *state = static_cast<const HWRTFaultCallbackState *>(p_userdata);
	if (state == nullptr || state->path.is_empty() || state->driver_key.is_empty()) {
		return;
	}
	// The callback runs before the driver aborts. It writes a file and does not touch RenderingDevice.
	const int64_t now = OS::get_singleton() != nullptr ? int64_t(OS::get_singleton()->get_unix_time()) : 0;
	write_hwrt_fault_file(state->path, state->driver_key, now);
}

void NextGI::fill_hwrt_selftest(HWRTSelfTestRay (&r_rays)[HWRT_SELFTEST_RAY_COUNT], HWRTSelfTestRecord (&r_expected)[HWRT_SELFTEST_RAY_COUNT]) {
	// One triangle in the z = 0 plane, counter-clockwise from +z: (0,0,0), (1,0,0), (0,1,0).
	// Hits travel along -z, so t is the origin's z and the barycentrics are the origin's x and y.
	// That is the analytic case of the #18 one-triangle fixture, expanded to 64 rays.
	for (int i = 0; i < HWRT_SELFTEST_RAY_COUNT; i++) {
		HWRTSelfTestRay ray;
		HWRTSelfTestRecord expected;
		ray.tmin = 0.001f;
		ray.tmax = 100.0f;
		ray.dir_x = 0.0f;
		ray.dir_y = 0.0f;
		ray.dir_z = -1.0f;
		if (i < HWRT_SELFTEST_HIT_COUNT) {
			const int column = i % 6;
			const int row = i / 6;
			const float u = 0.08f + float(column) * 0.08f;
			const float v = 0.08f + float(row) * 0.06f;
			const float t = 1.0f + float(i) * 0.05f;
			ray.origin_x = u;
			ray.origin_y = v;
			ray.origin_z = t;
			expected.t = t;
			expected.custom_index = 7;
			expected.geometry_index = 0;
			expected.primitive = 0;
			expected.bary_u = u;
			expected.bary_v = v;
			expected.front_face = 1;
			expected.hit = 1;
		} else {
			const int miss = i - HWRT_SELFTEST_HIT_COUNT;
			ray.origin_x = 2.0f + float(miss) * 0.1f;
			ray.origin_y = -1.0f;
			ray.origin_z = 4.0f;
			expected.hit = 0;
		}
		r_rays[i] = ray;
		r_expected[i] = expected;
	}
}

bool NextGI::hwrt_selftest_records_match(const HWRTSelfTestRecord *p_got, const HWRTSelfTestRecord *p_expected, int p_count, bool p_corrupt_expected_t, String *r_reason) {
	if (p_got == nullptr || p_expected == nullptr || p_count != HWRT_SELFTEST_RAY_COUNT) {
		if (r_reason != nullptr) {
			*r_reason = "self-test record count is not 64";
		}
		return false;
	}

	HWRTSelfTestRecord corrupted[HWRT_SELFTEST_RAY_COUNT];
	const HWRTSelfTestRecord *expected = p_expected;
	if (p_corrupt_expected_t) {
		for (int i = 0; i < HWRT_SELFTEST_RAY_COUNT; i++) {
			corrupted[i] = p_expected[i];
		}
		corrupted[0].t += 1.0f;
		expected = corrupted;
	}

	int hits = 0;
	int misses = 0;
	for (int i = 0; i < p_count; i++) {
		const HWRTSelfTestRecord &got = p_got[i];
		const HWRTSelfTestRecord &want = expected[i];
		if (want.hit == 0) {
			misses++;
			if (got.hit != 0) {
				if (r_reason != nullptr) {
					*r_reason = vformat("ray %d was a hit, analytic miss", i);
				}
				return false;
			}
			continue;
		}
		hits++;
		if (got.hit != 1 || got.custom_index != want.custom_index || got.geometry_index != want.geometry_index || got.primitive != want.primitive || got.front_face != want.front_face) {
			if (r_reason != nullptr) {
				*r_reason = vformat("ray %d id mismatch (hit %d custom %d geometry %d primitive %d front %d)", i, got.hit, got.custom_index, got.geometry_index, got.primitive, got.front_face);
			}
			return false;
		}
		const double denominator = Math::abs(double(want.t));
		const double relative_t = denominator == 0.0 ? Math::abs(double(got.t)) : Math::abs(double(got.t) - double(want.t)) / denominator;
		if (relative_t > HWRT_SELFTEST_REL_T) {
			if (r_reason != nullptr) {
				*r_reason = vformat("ray %d t relative error %.8f", i, relative_t);
			}
			return false;
		}
		const double bary_u = Math::abs(double(got.bary_u) - double(want.bary_u));
		const double bary_v = Math::abs(double(got.bary_v) - double(want.bary_v));
		const double barycentric = bary_u > bary_v ? bary_u : bary_v;
		if (barycentric > HWRT_SELFTEST_ABS_BARY) {
			if (r_reason != nullptr) {
				*r_reason = vformat("ray %d barycentric error %.8f", i, barycentric);
			}
			return false;
		}
	}
	if (hits != HWRT_SELFTEST_HIT_COUNT || misses != HWRT_SELFTEST_MISS_COUNT) {
		if (r_reason != nullptr) {
			*r_reason = vformat("self-test counted %d hits and %d misses", hits, misses);
		}
		return false;
	}
	return true;
}

String NextGI::build_hwrt_selftest_source() {
	NextgiSelftestShaderRD shader;
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

static bool _nextgi_dispatch_hwrt_selftest(RenderingDevice *p_device, String &r_failure);

bool NextGI::_run_hwrt_selftest(RenderingDevice *p_device, uint64_t &r_usec) {
	const uint64_t started_usec = OS::get_singleton()->get_ticks_usec();
	String failure;
	const bool passed = _nextgi_dispatch_hwrt_selftest(p_device, failure);
	r_usec = OS::get_singleton()->get_ticks_usec() - started_usec;
	if (!passed) {
		print_verbose(vformat("NextGI startup self-test failed: %s", failure));
	}
	return passed;
}

void NextGI::log_tier_selection(const String &p_line) {
	String line = p_line;
	if (selftest_report_time && (result.tier == TIER_HWRT || result.reason == SELFTEST_FAILED)) {
		line += " selftest=" + String::num(double(selftest_usec) / 1000.0, 3) + "ms";
	}
	_report(line, result);
}

void NextGI::_apply_safety_net(TierInputs &r_inputs) {
	r_inputs.fault_latched = false;
	r_inputs.selftest_passed = true;
	selftest_report_time = false;

	RenderingDevice *rd = RenderingDevice::get_singleton();
	const String driver_key = rd != nullptr ? rd->get_driver_version_key() : String();
	if (!driver_key.is_empty()) {
		const HWRTFaultLatch latch = resolve_hwrt_fault_latch(hwrt_fault_file_path(), driver_key, r_inputs.hwrt_setting);
		r_inputs.fault_latched = latch.latched;
	}

	// A latched fault is reported as HWRT_FAULTED and never traces. selftest_passed stays true
	// so the fault wins over SELFTEST_FAILED. Force clears the rule input and still traces.
	if (!_nextgi_earlier_hwrt_checks_pass(r_inputs) || r_inputs.fault_latched) {
		return;
	}
	if (rd == nullptr || rd->get_device_driver() == nullptr) {
		return;
	}

	if (nextgi_selftest_cache.valid && nextgi_selftest_cache.device == rd) {
		r_inputs.selftest_passed = nextgi_selftest_cache.passed;
		selftest_usec = nextgi_selftest_cache.usec;
		selftest_report_time = true;
		if (auto_quality == QUALITY_AUTO && selftest_usec > 0) {
			auto_quality = nextgi_auto_quality_from_usec(selftest_usec);
			auto_reason = vformat("auto: benchmark %.2f ms, %s", double(selftest_usec) / 1000.0, get_quality_name(auto_quality));
			if (quality_setting == QUALITY_AUTO) {
				print_line("NextGI: " + auto_reason);
			}
		}
		return;
	}

	uint64_t usec = 0;
	const bool passed = _run_hwrt_selftest(rd, usec);
	nextgi_selftest_cache.device = rd;
	nextgi_selftest_cache.passed = passed;
	nextgi_selftest_cache.usec = usec;
	nextgi_selftest_cache.valid = true;
	r_inputs.selftest_passed = passed;
	selftest_usec = usec;
	selftest_report_time = true;
	if (auto_quality == QUALITY_AUTO && selftest_usec > 0) {
		auto_quality = nextgi_auto_quality_from_usec(selftest_usec);
		auto_reason = vformat("auto: benchmark %.2f ms, %s", double(selftest_usec) / 1000.0, get_quality_name(auto_quality));
		if (quality_setting == QUALITY_AUTO) {
			print_line("NextGI: " + auto_reason);
		}
	}
}

void NextGI::_unregister_device_lost_callback() {
	if (!fault_callback_registered) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr) {
		rd->set_device_lost_callback(nullptr, nullptr);
	}
	fault_callback_registered = false;
}

void NextGI::_sync_device_lost_callback() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return;
	}
	if (result.tier != TIER_HWRT) {
		_unregister_device_lost_callback();
		return;
	}
	const String driver_key = rd->get_driver_version_key();
	if (driver_key.is_empty()) {
		_unregister_device_lost_callback();
		return;
	}
	fault_callback_state.path = hwrt_fault_file_path();
	fault_callback_state.driver_key = driver_key;
	rd->set_device_lost_callback(&NextGI::hwrt_device_lost_callback, &fault_callback_state);
	fault_callback_registered = true;
}

void NextGI::_maybe_simulate_device_lost() {
#ifdef DEBUG_ENABLED
	if (result.tier != TIER_HWRT || !fault_callback_registered || OS::get_singleton() == nullptr) {
		return;
	}
	const String value = OS::get_singleton()->get_environment("GODOTNEXT_NEXTGI_SIMULATE_DEVICE_LOST");
	if (value.is_empty() || !value.is_valid_int()) {
		return;
	}
	const int target_frame = value.to_int();
	if (target_frame <= 0) {
		return;
	}
	hwrt_frame_index++;
	if (hwrt_frame_index != target_frame) {
		return;
	}
	hwrt_device_lost_callback(&fault_callback_state);
	CRASH_NOW_MSG("NextGI simulated an HWRT device loss (GODOTNEXT_NEXTGI_SIMULATE_DEVICE_LOST).");
#endif
}

static bool _nextgi_dispatch_hwrt_selftest(RenderingDevice *p_device, String &r_failure) {
	if (p_device == nullptr || !p_device->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY)) {
		r_failure = "the device has no ray query";
		return false;
	}

	NextGI::HWRTSelfTestRay rays[NextGI::HWRT_SELFTEST_RAY_COUNT];
	NextGI::HWRTSelfTestRecord expected[NextGI::HWRT_SELFTEST_RAY_COUNT];
	NextGI::fill_hwrt_selftest(rays, expected);

	// The shader is created before the tier decision and freed before this function returns.
	// It does not call notify_ray_query_shader_initialized, so it is not counted as an HWRT ShaderRD.
	NextgiSelftestShaderRD shader;
	shader.initialize({ "" });

	struct VersionGuard {
		NextgiSelftestShaderRD &shader_rd;
		RID version;
		explicit VersionGuard(NextgiSelftestShaderRD &p_shader) :
				shader_rd(p_shader) {}
		~VersionGuard() {
			if (version.is_valid()) {
				shader_rd.version_free(version);
			}
		}
	} version_guard(shader);

	struct RIDGuard {
		RenderingDevice *device = nullptr;
		Vector<RID> rids;
		void keep(const RID &p_rid) {
			if (p_rid.is_valid()) {
				rids.push_back(p_rid);
			}
		}
		~RIDGuard() {
			if (device == nullptr) {
				return;
			}
			for (int i = rids.size() - 1; i >= 0; i--) {
				device->free_rid(rids[i]);
			}
		}
	} rid_guard;
	rid_guard.device = p_device;

	// Buffer creation and BLAS/TLAS builds record graph work. Flush that work before the RID
	// guard frees the resources, including on the failure returns below.
	struct FlushGuard {
		RenderingDevice *device = nullptr;
		RID buffer;
		bool done = false;
		void mark_done() { done = true; }
		~FlushGuard() {
			if (done || device == nullptr || !buffer.is_valid()) {
				return;
			}
			device->buffer_get_data(buffer);
		}
	} flush_guard;
	flush_guard.device = p_device;

	version_guard.version = shader.version_create(false);
	if (!version_guard.version.is_valid()) {
		r_failure = "the self-test shader failed to compile";
		return false;
	}
	const RID shader_rid = shader.version_get_shader(version_guard.version, 0);
	if (!shader_rid.is_valid()) {
		r_failure = "the self-test shader has no compute version";
		return false;
	}

	const float vertices[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
	Vector<uint8_t> vertex_bytes;
	vertex_bytes.resize(sizeof(vertices));
	memcpy(vertex_bytes.ptrw(), vertices, sizeof(vertices));
	BitField<RenderingDevice::BufferCreationBits> build_input_bits;
	build_input_bits.set_flag(RenderingDevice::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
	build_input_bits.set_flag(RenderingDevice::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
	const RID vertex_buffer = p_device->vertex_buffer_create(vertex_bytes.size(), vertex_bytes, build_input_bits);
	rid_guard.keep(vertex_buffer);
	flush_guard.buffer = vertex_buffer;
	if (!vertex_buffer.is_valid()) {
		r_failure = "the self-test vertex buffer was not created";
		return false;
	}

	RenderingDevice::AccelerationStructureGeometry geometry;
	geometry.flags.set_flag(RenderingDevice::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT);
	geometry.vertex_buffer = vertex_buffer;
	geometry.vertex_stride = sizeof(float) * 3;
	geometry.vertex_format = RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT;
	geometry.vertex_count = 3;
	const RID blas = p_device->blas_create(Span<RenderingDevice::AccelerationStructureGeometry>(&geometry, 1), 0);
	rid_guard.keep(blas);
	if (!blas.is_valid() || p_device->blas_build(blas) != OK) {
		r_failure = "the self-test BLAS was not built";
		return false;
	}

	RenderingDevice::AccelerationStructureInstance instance;
	instance.transform = Transform3D();
	instance.id = 7;
	instance.mask = 0xFF;
	instance.hit_sbt_range = 0;
	instance.blas = blas;
	const RID tlas = p_device->tlas_create(1, 0);
	rid_guard.keep(tlas);
	if (!tlas.is_valid() || p_device->tlas_build(tlas, Span<RenderingDevice::AccelerationStructureInstance>(&instance, 1)) != OK) {
		r_failure = "the self-test TLAS was not built";
		return false;
	}

	Vector<uint8_t> ray_bytes;
	ray_bytes.resize(NextGI::HWRT_SELFTEST_RAY_COUNT * sizeof(NextGI::HWRTSelfTestRay));
	memcpy(ray_bytes.ptrw(), rays, ray_bytes.size());
	const RID ray_buffer = p_device->storage_buffer_create(ray_bytes.size(), ray_bytes);
	rid_guard.keep(ray_buffer);
	const RID result_buffer = p_device->storage_buffer_create(NextGI::HWRT_SELFTEST_RAY_COUNT * sizeof(NextGI::HWRTSelfTestRecord));
	rid_guard.keep(result_buffer);
	if (!ray_buffer.is_valid() || !result_buffer.is_valid()) {
		r_failure = "the self-test ray buffers were not created";
		return false;
	}

	const RID pipeline = p_device->compute_pipeline_create(shader_rid);
	rid_guard.keep(pipeline);
	if (!pipeline.is_valid()) {
		r_failure = "the self-test compute pipeline was not created";
		return false;
	}

	RenderingDevice::Uniform uniforms[3] = {
		RenderingDevice::Uniform(RenderingDevice::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, tlas),
		RenderingDevice::Uniform(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, 1, ray_buffer),
		RenderingDevice::Uniform(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, 2, result_buffer),
	};
	const RID uniform_set = p_device->uniform_set_create(VectorView<RenderingDevice::Uniform>(uniforms, 3), shader_rid, 0);
	rid_guard.keep(uniform_set);
	if (!uniform_set.is_valid()) {
		r_failure = "the self-test uniform set was not created";
		return false;
	}

	{
		struct ComputeListGuard {
			RenderingDevice *device = nullptr;
			bool open = false;
			~ComputeListGuard() {
				if (open && device != nullptr) {
					device->compute_list_end();
				}
			}
		} compute_list;
		compute_list.device = p_device;
		const RenderingDevice::ComputeListID list = p_device->compute_list_begin();
		if (list == RenderingDevice::INVALID_ID) {
			r_failure = "the self-test compute list did not begin";
			return false;
		}
		compute_list.open = true;
		p_device->compute_list_bind_compute_pipeline(list, pipeline);
		p_device->compute_list_bind_uniform_set(list, uniform_set, 0);
		p_device->compute_list_dispatch(list, 1, 1, 1);
	}

	const Vector<uint8_t> data = p_device->buffer_get_data(result_buffer);
	flush_guard.mark_done();
	const int record_bytes = NextGI::HWRT_SELFTEST_RAY_COUNT * int(sizeof(NextGI::HWRTSelfTestRecord));
	if (data.size() != record_bytes) {
		r_failure = vformat("the self-test readback is %d bytes, expected %d", data.size(), record_bytes);
		return false;
	}
	NextGI::HWRTSelfTestRecord got[NextGI::HWRT_SELFTEST_RAY_COUNT];
	memcpy(got, data.ptr(), record_bytes);

	bool corrupt = false;
#ifdef DEBUG_ENABLED
	corrupt = _nextgi_selftest_corrupt_enabled();
#endif
	if (!NextGI::hwrt_selftest_records_match(got, expected, NextGI::HWRT_SELFTEST_RAY_COUNT, corrupt, &r_failure)) {
		return false;
	}
	return true;
}
