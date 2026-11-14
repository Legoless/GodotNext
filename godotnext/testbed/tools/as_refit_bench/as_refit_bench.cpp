/**************************************************************************/
/*  as_refit_bench.cpp                                                    */
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

// GodotNext: AS calibration refit tool (#9, G0-6; plan §2.1 line 1a, §13.1).
//
// A minimal standalone Vulkan program, kept under the testbed and not part of the engine build.
// It measures BLAS refit throughput with VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR (source
// = destination) and checks itself against the RD benchmark (tests/as_calibration) by timing the
// same 100k-instance TLAS and the same PREFER_FAST_TRACE BLAS set. The derivation
// (godotnext_metrics.as_calibration) compares the two and logs both refit figures.
//
// Every measurement mirrors the RD benchmark: the same meshes (64 grid meshes from 1k to 64k
// triangles, 2,137,242 in total; refit meshes of 10k to 200k triangles), float32 positions and
// uint32 indices in device-local build-input buffers, opaque geometry, instances in host-visible
// memory as RD keeps them, one submit and wait per iteration, a barrier after every build, and
// nearest-rank percentiles. Timestamps come from vkCmdWriteTimestamp2.
//
// Usage: as_refit_bench [--device <name>] [--gpu-index <n>] [--warmup 20] [--iters 200] [--runs 1]
//                       [--refit 10000,50000,100000,200000] [--out <dir>] [--validation]
//                       [--list-devices] [--self-test]
// Prints one "GODOTNEXT_RESULT <json>" line; with --out it also writes <dir>/as_refit_bench.json.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

static constexpr int SCHEMA = 1;
static constexpr uint32_t SEED = 0x41534331;
static constexpr uint32_t BLAS_COUNT = 64;
static constexpr uint32_t BLAS_MIN_TRIANGLES = 1024;
static constexpr uint32_t BLAS_MAX_TRIANGLES = 65536;
static constexpr float TILE_SIZE = 10.0f;
static constexpr float DISPLACEMENT = 0.25f;
static constexpr float SCENE_RADIUS = 200.0f;
static constexpr uint32_t TLAS_INSTANCES = 100000;
// Line 1a's skinned-refit budget; the refit figure is taken at this size, else at the largest.
static constexpr uint32_t LINE_1A_REFIT_TRIANGLES = 200000;
static constexpr double PI = 3.14159265358979323846;

/* Options */

struct Options {
	std::string device;
	int gpu_index = -1;
	int warmup = 20;
	int iters = 200;
	int runs = 1;
	std::vector<uint32_t> refit_sizes = { 10000, 50000, 100000, 200000 };
	std::string out_dir;
	bool validation = false;
	bool list_devices = false;
	bool self_test = false;
	bool help = false;
};

static const char *USAGE = "Usage: as_refit_bench [--device <name>] [--gpu-index <n>] [--warmup 20] [--iters 200] [--runs 1]\n"
						   "                      [--refit 10000,50000,100000,200000] [--out <dir>] [--validation]\n"
						   "                      [--list-devices] [--self-test]\n";

static bool parse_int(const std::string &p_text, int p_min, int &r_value) {
	if (p_text.empty()) {
		return false;
	}
	char *end = nullptr;
	const long value = std::strtol(p_text.c_str(), &end, 10);
	if (*end != '\0' || value < p_min || value > 1000000000L) {
		return false;
	}
	r_value = int(value);
	return true;
}

static bool parse_sizes(const std::string &p_text, std::vector<uint32_t> &r_sizes) {
	std::vector<uint32_t> sizes;
	size_t start = 0;
	while (start <= p_text.size()) {
		const size_t comma = std::min(p_text.find(',', start), p_text.size());
		int value = 0;
		if (!parse_int(p_text.substr(start, comma - start), 1, value)) {
			return false;
		}
		sizes.push_back(uint32_t(value));
		start = comma + 1;
	}
	r_sizes = sizes;
	return !sizes.empty();
}

// Accepts "--name value" and "--name=value".
static bool parse_options(int p_argc, const char *const *p_argv, Options &r_options, std::string &r_error) {
	for (int i = 1; i < p_argc; i++) {
		std::string name = p_argv[i];
		std::string value;
		bool has_value = false;
		const size_t equals = name.find('=');
		if (name.rfind("--", 0) == 0 && equals != std::string::npos) {
			value = name.substr(equals + 1);
			name = name.substr(0, equals);
			has_value = true;
		}
		if (name == "--validation" || name == "--list-devices" || name == "--self-test" || name == "--help") {
			if (has_value) {
				r_error = name + " takes no value";
				return false;
			}
			r_options.validation |= name == "--validation";
			r_options.list_devices |= name == "--list-devices";
			r_options.self_test |= name == "--self-test";
			r_options.help |= name == "--help";
			continue;
		}
		if (!has_value) {
			if (i + 1 >= p_argc) {
				r_error = name + " needs a value";
				return false;
			}
			value = p_argv[++i];
		}
		bool ok = true;
		if (name == "--device") {
			r_options.device = value;
			ok = !value.empty();
		} else if (name == "--gpu-index") {
			ok = parse_int(value, 0, r_options.gpu_index);
		} else if (name == "--warmup") {
			ok = parse_int(value, 0, r_options.warmup);
		} else if (name == "--iters") {
			ok = parse_int(value, 1, r_options.iters);
		} else if (name == "--runs") {
			ok = parse_int(value, 1, r_options.runs);
		} else if (name == "--refit") {
			ok = parse_sizes(value, r_options.refit_sizes);
		} else if (name == "--out") {
			r_options.out_dir = value;
			ok = !value.empty();
		} else {
			r_error = "unknown option " + name;
			return false;
		}
		if (!ok) {
			r_error = "bad value for " + name + ": " + value;
			return false;
		}
	}
	return true;
}

/* Geometry, the same as the RD benchmark's */

// Seeded and portable: std::mt19937 is specified bit for bit, and the conversions are ours, so
// every platform builds the same meshes and instances.
class Rng {
	std::mt19937 engine;

public:
	explicit Rng(uint32_t p_seed) :
			engine(p_seed) {}

	// Uniform in [0, 1).
	double randf() { return double(engine() >> 8) * (1.0 / 16777216.0); }
	float randf_range(float p_from, float p_to) { return p_from + float(randf()) * (p_to - p_from); }
	// Standard normal (Box-Muller).
	double randfn() {
		const double u1 = 1.0 - randf();
		const double u2 = randf();
		return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * PI * u2);
	}
};

struct Mesh {
	uint32_t triangles = 0;
	std::vector<float> vertices; // x, y, z per vertex.
	std::vector<uint32_t> indices;

	uint32_t vertex_count() const { return uint32_t(vertices.size() / 3); }
};

// One grid mesh: columns x rows quads, two triangles each, z displaced by a seeded value per
// vertex (as _grid_mesh in tests/as_calibration/as_calibration.gd).
static Mesh grid_mesh(uint32_t p_target_triangles, Rng &r_rng) {
	const uint32_t quads = std::max<uint32_t>(1, p_target_triangles / 2);
	const uint32_t columns = uint32_t(std::ceil(std::sqrt(double(quads))));
	const uint32_t rows = uint32_t(std::ceil(double(quads) / columns));
	Mesh mesh;
	mesh.vertices.resize(size_t(columns + 1) * (rows + 1) * 3);
	const float step_x = TILE_SIZE / columns;
	const float step_y = TILE_SIZE / rows;
	size_t o = 0;
	for (uint32_t y = 0; y <= rows; y++) {
		for (uint32_t x = 0; x <= columns; x++) {
			mesh.vertices[o] = x * step_x;
			mesh.vertices[o + 1] = y * step_y;
			mesh.vertices[o + 2] = r_rng.randf_range(-DISPLACEMENT, DISPLACEMENT);
			o += 3;
		}
	}
	mesh.indices.resize(size_t(columns) * rows * 6);
	o = 0;
	const uint32_t stride = columns + 1;
	for (uint32_t y = 0; y < rows; y++) {
		for (uint32_t x = 0; x < columns; x++) {
			const uint32_t a = y * stride + x;
			mesh.indices[o] = a;
			mesh.indices[o + 1] = a + 1;
			mesh.indices[o + 2] = a + stride;
			mesh.indices[o + 3] = a + 1;
			mesh.indices[o + 4] = a + stride + 1;
			mesh.indices[o + 5] = a + stride;
			o += 6;
		}
	}
	mesh.triangles = columns * rows * 2;
	return mesh;
}

// 64 meshes with triangle counts spread evenly from 1k to 64k (2,137,242 in total).
static std::vector<Mesh> make_meshes() {
	Rng rng(SEED);
	std::vector<Mesh> meshes;
	for (uint32_t i = 0; i < BLAS_COUNT; i++) {
		const double weight = double(i) / (BLAS_COUNT - 1);
		const uint32_t target = uint32_t(BLAS_MIN_TRIANGLES + (double(BLAS_MAX_TRIANGLES) - BLAS_MIN_TRIANGLES) * weight);
		meshes.push_back(grid_mesh(target, rng));
	}
	return meshes;
}

// The refit mesh of a size and the second vertex set it alternates with, so every update moves
// every vertex (as _time_refit in the RD benchmark).
static Mesh refit_mesh(uint32_t p_triangles, std::vector<float> &r_other_vertices) {
	Rng rng(SEED + p_triangles);
	Mesh mesh = grid_mesh(p_triangles, rng);
	r_other_vertices = mesh.vertices;
	for (size_t i = 2; i < r_other_vertices.size(); i += 3) {
		r_other_vertices[i] = rng.randf_range(-DISPLACEMENT, DISPLACEMENT);
	}
	return mesh;
}

struct Transform {
	float rows[3][4]; // Row-major 3x4, the layout of VkTransformMatrixKHR.
};

static void multiply3(const double p_a[3][3], const double p_b[3][3], double r_out[3][3]) {
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			r_out[r][c] = p_a[r][0] * p_b[0][c] + p_a[r][1] * p_b[1][c] + p_a[r][2] * p_b[2][c];
		}
	}
}

// Seeded instance transforms, uniform in a 200 m sphere, with random rotations composed Y, then X,
// then Z, the default Euler order of Basis.from_euler.
static std::vector<Transform> make_transforms(uint32_t p_count) {
	Rng rng(SEED + p_count);
	std::vector<Transform> transforms(p_count);
	for (Transform &transform : transforms) {
		double direction[3] = { rng.randfn(), rng.randfn(), rng.randfn() };
		const double length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
		for (double &axis : direction) {
			axis = length > 0.0 ? axis / length : 0.0;
		}
		const double distance = SCENE_RADIUS * std::cbrt(rng.randf());
		const double ax = rng.randf() * 2.0 * PI;
		const double ay = rng.randf() * 2.0 * PI;
		const double az = rng.randf() * 2.0 * PI;
		const double rx[3][3] = { { 1, 0, 0 }, { 0, std::cos(ax), -std::sin(ax) }, { 0, std::sin(ax), std::cos(ax) } };
		const double ry[3][3] = { { std::cos(ay), 0, std::sin(ay) }, { 0, 1, 0 }, { -std::sin(ay), 0, std::cos(ay) } };
		const double rz[3][3] = { { std::cos(az), -std::sin(az), 0 }, { std::sin(az), std::cos(az), 0 }, { 0, 0, 1 } };
		double yx[3][3];
		double basis[3][3];
		multiply3(ry, rx, yx);
		multiply3(yx, rz, basis);
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) {
				transform.rows[r][c] = float(basis[r][c]);
			}
			transform.rows[r][3] = float(direction[r] * distance);
		}
	}
	return transforms;
}

/* Statistics */

// Nearest-rank percentile, as the RD benchmark computes it; 0 for an empty list.
static double percentile(std::vector<double> p_values, double p_fraction) {
	if (p_values.empty()) {
		return 0.0;
	}
	std::sort(p_values.begin(), p_values.end());
	const int last = int(p_values.size()) - 1;
	const int rank = std::clamp(int(std::ceil(p_fraction * p_values.size())) - 1, 0, last);
	return p_values[rank];
}

// The median of runs, as Python's statistics.median; 0 for an empty list.
static double median(std::vector<double> p_values) {
	if (p_values.empty()) {
		return 0.0;
	}
	std::sort(p_values.begin(), p_values.end());
	const size_t middle = p_values.size() / 2;
	return p_values.size() % 2 ? p_values[middle] : (p_values[middle - 1] + p_values[middle]) * 0.5;
}

// GPU milliseconds between two raw timestamps; the counter wraps at its valid bits.
static double timestamp_ms(uint64_t p_begin, uint64_t p_end, uint32_t p_valid_bits, double p_period_ns) {
	const uint64_t mask = p_valid_bits >= 64 ? ~uint64_t(0) : ((uint64_t(1) << p_valid_bits) - 1);
	return double((p_end - p_begin) & mask) * p_period_ns / 1e6;
}

/* JSON output */

class JsonWriter {
	std::string text;
	std::vector<bool> has_items;

	void append_string(const std::string &p_value) {
		text += '"';
		for (const char ch : p_value) {
			const unsigned char c = (unsigned char)ch;
			if (c == '"' || c == '\\') {
				text += '\\';
				text += ch;
			} else if (c < 0x20) {
				char escaped[8];
				std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
				text += escaped;
			} else {
				text += ch;
			}
		}
		text += '"';
	}

	void key(const char *p_key) {
		if (!has_items.empty()) {
			if (has_items.back()) {
				text += ',';
			}
			has_items.back() = true;
		}
		if (p_key != nullptr) {
			append_string(p_key);
			text += ':';
		}
	}

public:
	void begin_object(const char *p_key = nullptr) {
		key(p_key);
		text += '{';
		has_items.push_back(false);
	}
	void end_object() {
		text += '}';
		has_items.pop_back();
	}
	void begin_array(const char *p_key = nullptr) {
		key(p_key);
		text += '[';
		has_items.push_back(false);
	}
	void end_array() {
		text += ']';
		has_items.pop_back();
	}
	void number(const char *p_key, double p_value) {
		key(p_key);
		char buffer[40];
		std::snprintf(buffer, sizeof(buffer), "%.9g", std::isfinite(p_value) ? p_value : 0.0);
		text += buffer;
	}
	void integer(const char *p_key, long long p_value) {
		key(p_key);
		text += std::to_string(p_value);
	}
	void boolean(const char *p_key, bool p_value) {
		key(p_key);
		text += p_value ? "true" : "false";
	}
	void string(const char *p_key, const std::string &p_value) {
		key(p_key);
		append_string(p_value);
	}
	const std::string &get() const { return text; }
};

/* Results */

struct TimedPoint {
	uint32_t count = 0; // Triangles, or instances for the TLAS.
	double p50_ms = 0.0;
	double p95_ms = 0.0;
	double triangles_per_ms = 0.0;
	uint64_t memory_bytes = 0;
};

struct BlasSetResult {
	uint64_t triangles = 0;
	double p50_ms_total = 0.0;
	double p95_ms_total = 0.0;
	double triangles_per_ms = 0.0;
	uint64_t memory_bytes = 0;
};

struct RunResult {
	BlasSetResult blas_fast_trace;
	TimedPoint tlas;
	std::vector<TimedPoint> refit; // One per refit size, in order.
};

struct ResultInfo {
	std::string adapter;
	std::string driver_version;
	std::string api_version;
	uint32_t vendor_id = 0;
	uint32_t device_id = 0;
	bool validation = false;
	bool sync_validation = false;
	bool nv_ray_tracing_validation = false;
	int validation_errors = 0;
	int validation_warnings = 0;
	std::vector<std::string> failures;
};

static void write_point(JsonWriter &r_json, const char *p_key, const char *p_count_key, const TimedPoint &p_point, bool p_throughput) {
	r_json.begin_object(p_key);
	r_json.integer(p_count_key, p_point.count);
	r_json.number("p50_ms", p_point.p50_ms);
	r_json.number("p95_ms", p_point.p95_ms);
	if (p_throughput) {
		r_json.number("triangles_per_ms", p_point.triangles_per_ms);
	}
	r_json.integer("memory_bytes", (long long)p_point.memory_bytes);
	r_json.end_object();
}

// The result the derivation reads: the top-level cross-check and refit figures are medians over
// runs; "runs" keeps every run.
static std::string result_json(const ResultInfo &p_info, const Options &p_options, const std::vector<RunResult> &p_runs) {
	JsonWriter json;
	json.begin_object();
	json.string("test", "as_refit_bench");
	json.integer("schema", SCHEMA);
	json.string("driver", "vulkan");
	json.string("adapter", p_info.adapter);
	json.integer("vendor_id", p_info.vendor_id);
	json.integer("device_id", p_info.device_id);
	json.string("driver_version", p_info.driver_version);
	json.string("api_version", p_info.api_version);
	json.integer("seed", SEED);
	json.integer("warmup", p_options.warmup);
	json.integer("iters", p_options.iters);
	json.integer("blas_meshes", BLAS_COUNT);
	json.integer("tlas_instances", TLAS_INSTANCES);
	json.begin_array("refit_sizes");
	for (const uint32_t size : p_options.refit_sizes) {
		json.integer(nullptr, size);
	}
	json.end_array();

	std::vector<double> blas;
	std::vector<double> tlas;
	for (const RunResult &run : p_runs) {
		blas.push_back(run.blas_fast_trace.triangles_per_ms);
		tlas.push_back(run.tlas.p95_ms);
	}
	json.number("blas_fast_trace_triangles_per_ms", median(blas));
	json.number("tlas_100k_p95_ms", median(tlas));

	double refit_figure = 0.0;
	uint32_t refit_figure_size = 0;
	json.begin_object("refit");
	for (size_t i = 0; i < p_options.refit_sizes.size(); i++) {
		std::vector<double> throughput;
		std::vector<double> p50;
		std::vector<double> p95;
		uint32_t triangles = 0;
		for (const RunResult &run : p_runs) {
			if (i < run.refit.size()) {
				throughput.push_back(run.refit[i].triangles_per_ms);
				p50.push_back(run.refit[i].p50_ms);
				p95.push_back(run.refit[i].p95_ms);
				triangles = run.refit[i].count;
			}
		}
		const uint32_t size = p_options.refit_sizes[i];
		json.begin_object(std::to_string(size).c_str());
		json.integer("triangles", triangles);
		json.number("p50_ms", median(p50));
		json.number("p95_ms", median(p95));
		json.number("triangles_per_ms", median(throughput));
		json.end_object();
		// The skinned budget's size, else the largest size.
		const bool budget = refit_figure_size == LINE_1A_REFIT_TRIANGLES;
		if (size == LINE_1A_REFIT_TRIANGLES || (!budget && size > refit_figure_size)) {
			refit_figure = median(throughput);
			refit_figure_size = size;
		}
	}
	json.end_object();
	json.number("refit_triangles_per_ms", refit_figure);
	json.integer("refit_triangles_per_ms_size", refit_figure_size);

	json.begin_array("runs");
	for (const RunResult &run : p_runs) {
		json.begin_object();
		json.begin_object("blas_fast_trace");
		json.integer("triangles", (long long)run.blas_fast_trace.triangles);
		json.number("p50_ms_total", run.blas_fast_trace.p50_ms_total);
		json.number("p95_ms_total", run.blas_fast_trace.p95_ms_total);
		json.number("triangles_per_ms", run.blas_fast_trace.triangles_per_ms);
		json.integer("memory_bytes", (long long)run.blas_fast_trace.memory_bytes);
		json.end_object();
		write_point(json, "tlas", "instances", run.tlas, false);
		json.begin_object("refit");
		for (size_t i = 0; i < run.refit.size() && i < p_options.refit_sizes.size(); i++) {
			write_point(json, std::to_string(p_options.refit_sizes[i]).c_str(), "triangles", run.refit[i], true);
		}
		json.end_object();
		json.end_object();
	}
	json.end_array();

	json.boolean("validation", p_info.validation);
	json.boolean("sync_validation", p_info.sync_validation);
	json.boolean("nv_ray_tracing_validation", p_info.nv_ray_tracing_validation);
	json.integer("validation_errors", p_info.validation_errors);
	json.integer("validation_warnings", p_info.validation_warnings);
	json.begin_array("failures");
	for (const std::string &failure : p_info.failures) {
		json.string(nullptr, failure);
	}
	json.end_array();
	json.boolean("pass", p_info.failures.empty() && p_info.validation_errors == 0);
	json.end_object();
	return json.get();
}

static std::string short_result_json(bool p_fallback, const std::string &p_reason, const std::string &p_adapter, bool p_pass) {
	JsonWriter json;
	json.begin_object();
	json.string("test", "as_refit_bench");
	json.integer("schema", SCHEMA);
	json.string("driver", "vulkan");
	json.string("adapter", p_adapter);
	if (p_fallback) {
		json.boolean("fallback", true);
	}
	json.string("reason", p_reason);
	json.boolean("pass", p_pass);
	json.end_object();
	return json.get();
}

static void print_result(const std::string &p_json) {
	std::printf("GODOTNEXT_RESULT %s\n", p_json.c_str());
	std::fflush(stdout);
}

// Prints a failed result and exits 1. Used for Vulkan errors the benchmark cannot recover from.
[[noreturn]] static void fatal(const char *p_format, ...) {
	char message[1024];
	va_list args;
	va_start(args, p_format);
	std::vsnprintf(message, sizeof(message), p_format, args);
	va_end(args);
	std::fprintf(stderr, "as_refit_bench: error: %s\n", message);
	print_result(short_result_json(false, message, "", false));
	std::exit(1);
}

#define VK_CHECK(m_expr) \
	do { \
		const VkResult vk_check_result = (m_expr); \
		if (vk_check_result != VK_SUCCESS) { \
			fatal("%s failed (VkResult %d)", #m_expr, int(vk_check_result)); \
		} \
	} while (false)

/* Vulkan */

static int validation_errors = 0;
static int validation_warnings = 0;

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT p_severity, VkDebugUtilsMessageTypeFlagsEXT p_types, const VkDebugUtilsMessengerCallbackDataEXT *p_data, void *p_user_data) {
	(void)p_user_data;
	const char *id = p_data->pMessageIdName != nullptr ? p_data->pMessageIdName : "";
	const char *message = p_data->pMessage != nullptr ? p_data->pMessage : "";
	const bool validation = (p_types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) != 0;
	if (p_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
		if (validation) {
			validation_errors++;
			std::fprintf(stderr, "Validation Error: [ %s ] %s\n", id, message);
		} else {
			std::fprintf(stderr, "Vulkan error: %s\n", message);
		}
	} else if (p_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		if (validation) {
			validation_warnings++;
			std::fprintf(stderr, "Validation Warning: [ %s ] %s\n", id, message);
		} else {
			std::fprintf(stderr, "Vulkan warning: %s\n", message);
		}
	}
	return VK_FALSE;
}

struct Buffer {
	VkBuffer handle = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize size = 0;
	VkDeviceAddress address = 0;
	void *mapped = nullptr;
};

struct AccelerationStructure {
	VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
	Buffer buffer;
	VkDeviceAddress address = 0;
	VkDeviceSize size = 0;
	VkDeviceSize build_scratch = 0;
	VkDeviceSize update_scratch = 0;
};

struct Context {
	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties memory_properties = {};
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queue_family = 0;
	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkQueryPool query_pool = VK_NULL_HANDLE;
	uint32_t timestamp_valid_bits = 0;
	double timestamp_period_ns = 0.0;
	VkDeviceSize scratch_alignment = 1;

	PFN_vkCreateAccelerationStructureKHR CreateAccelerationStructureKHR = nullptr;
	PFN_vkDestroyAccelerationStructureKHR DestroyAccelerationStructureKHR = nullptr;
	PFN_vkGetAccelerationStructureBuildSizesKHR GetAccelerationStructureBuildSizesKHR = nullptr;
	PFN_vkGetAccelerationStructureDeviceAddressKHR GetAccelerationStructureDeviceAddressKHR = nullptr;
	PFN_vkCmdBuildAccelerationStructuresKHR CmdBuildAccelerationStructuresKHR = nullptr;
	PFN_vkGetBufferDeviceAddress GetBufferDeviceAddress = nullptr;
	PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2 = nullptr;
	PFN_vkCmdWriteTimestamp2 CmdWriteTimestamp2 = nullptr;
};

static bool has_extension(const std::vector<VkExtensionProperties> &p_extensions, const char *p_name) {
	for (const VkExtensionProperties &extension : p_extensions) {
		if (std::strcmp(extension.extensionName, p_name) == 0) {
			return true;
		}
	}
	return false;
}

static std::vector<VkExtensionProperties> instance_extensions(const char *p_layer) {
	uint32_t count = 0;
	vkEnumerateInstanceExtensionProperties(p_layer, &count, nullptr);
	std::vector<VkExtensionProperties> extensions(count);
	vkEnumerateInstanceExtensionProperties(p_layer, &count, extensions.data());
	extensions.resize(count);
	return extensions;
}

static std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice p_device) {
	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties(p_device, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> extensions(count);
	vkEnumerateDeviceExtensionProperties(p_device, nullptr, &count, extensions.data());
	extensions.resize(count);
	return extensions;
}

static std::string version_string(uint32_t p_version) {
	return std::to_string(VK_API_VERSION_MAJOR(p_version)) + "." + std::to_string(VK_API_VERSION_MINOR(p_version)) + "." + std::to_string(VK_API_VERSION_PATCH(p_version));
}

// NVIDIA packs its driver version as 10.8.8.6 bits; other vendors use the Vulkan packing.
static std::string driver_version_string(uint32_t p_vendor_id, uint32_t p_version) {
	if (p_vendor_id == 0x10DE) {
		return std::to_string((p_version >> 22) & 0x3FF) + "." + std::to_string((p_version >> 14) & 0xFF);
	}
	return version_string(p_version);
}

static const char *VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

static void create_instance(Context &r_ctx, const Options &p_options, ResultInfo &r_info) {
	std::vector<const char *> layers;
	std::vector<const char *> extensions;
	bool layer_settings = false;
	if (p_options.validation) {
		uint32_t count = 0;
		vkEnumerateInstanceLayerProperties(&count, nullptr);
		std::vector<VkLayerProperties> available(count);
		vkEnumerateInstanceLayerProperties(&count, available.data());
		available.resize(count);
		const bool found = std::any_of(available.begin(), available.end(), [](const VkLayerProperties &p_layer) { return std::strcmp(p_layer.layerName, VALIDATION_LAYER) == 0; });
		if (!found) {
			fatal("--validation needs %s (the Vulkan SDK's validation layer)", VALIDATION_LAYER);
		}
		layers.push_back(VALIDATION_LAYER);
		const std::vector<VkExtensionProperties> layer_extensions = instance_extensions(VALIDATION_LAYER);
		if (!has_extension(instance_extensions(nullptr), VK_EXT_DEBUG_UTILS_EXTENSION_NAME) && !has_extension(layer_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
			fatal("--validation needs %s", VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		layer_settings = has_extension(layer_extensions, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
		if (layer_settings) {
			extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
		}
	}

	VkApplicationInfo app_info = {};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "as_refit_bench";
	app_info.pEngineName = "GodotNext testbed";
	app_info.apiVersion = VK_API_VERSION_1_3;

	VkDebugUtilsMessengerCreateInfoEXT messenger_info = {};
	messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	messenger_info.pfnUserCallback = debug_callback;

	// Synchronization validation, as the Session-1 runner's validation jobs turn it on.
	const VkBool32 enable = VK_TRUE;
	VkLayerSettingEXT sync_setting = {};
	sync_setting.pLayerName = VALIDATION_LAYER;
	sync_setting.pSettingName = "validate_sync";
	sync_setting.type = VK_LAYER_SETTING_TYPE_BOOL32_EXT;
	sync_setting.valueCount = 1;
	sync_setting.pValues = &enable;
	VkLayerSettingsCreateInfoEXT settings_info = {};
	settings_info.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
	settings_info.pNext = &messenger_info;
	settings_info.settingCount = 1;
	settings_info.pSettings = &sync_setting;

	VkInstanceCreateInfo create_info = {};
	create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	create_info.pApplicationInfo = &app_info;
	create_info.enabledLayerCount = uint32_t(layers.size());
	create_info.ppEnabledLayerNames = layers.data();
	create_info.enabledExtensionCount = uint32_t(extensions.size());
	create_info.ppEnabledExtensionNames = extensions.data();
	if (p_options.validation) {
		create_info.pNext = layer_settings ? (const void *)&settings_info : (const void *)&messenger_info;
	}
	VK_CHECK(vkCreateInstance(&create_info, nullptr, &r_ctx.instance));

	if (p_options.validation) {
		PFN_vkCreateDebugUtilsMessengerEXT create_messenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(r_ctx.instance, "vkCreateDebugUtilsMessengerEXT");
		if (create_messenger == nullptr) {
			fatal("vkCreateDebugUtilsMessengerEXT is missing");
		}
		VK_CHECK(create_messenger(r_ctx.instance, &messenger_info, nullptr, &r_ctx.messenger));
		// Without layer settings, synchronization validation comes from the environment: the documented
		// VK_VALIDATION_VALIDATE_SYNC, or the full-prefix name the Session-1 runner sets.
		bool sync_env = false;
		for (const char *name : { "VK_VALIDATION_VALIDATE_SYNC", "VK_KHRONOS_VALIDATION_VALIDATE_SYNC" }) {
			const char *value = std::getenv(name);
			sync_env = sync_env || (value != nullptr && std::strcmp(value, "1") == 0);
		}
		r_info.sync_validation = layer_settings || sync_env;
		if (!r_info.sync_validation) {
			std::fprintf(stderr, "as_refit_bench: the validation layer has no %s; set VK_VALIDATION_VALIDATE_SYNC=1 for synchronization validation\n", VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
		}
	}
	r_info.validation = p_options.validation;
}

static std::vector<VkPhysicalDevice> physical_devices(const Context &p_ctx) {
	uint32_t count = 0;
	VK_CHECK(vkEnumeratePhysicalDevices(p_ctx.instance, &count, nullptr));
	std::vector<VkPhysicalDevice> devices(count);
	VK_CHECK(vkEnumeratePhysicalDevices(p_ctx.instance, &count, devices.data()));
	devices.resize(count);
	return devices;
}

static bool supports_acceleration_structures(VkPhysicalDevice p_device) {
	const std::vector<VkExtensionProperties> extensions = device_extensions(p_device);
	return has_extension(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && has_extension(extensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
}

static std::string lowercase(std::string p_text) {
	std::transform(p_text.begin(), p_text.end(), p_text.begin(), [](unsigned char p_c) { return char(std::tolower(p_c)); });
	return p_text;
}

static void list_devices(const Context &p_ctx) {
	const std::vector<VkPhysicalDevice> devices = physical_devices(p_ctx);
	for (size_t i = 0; i < devices.size(); i++) {
		VkPhysicalDeviceProperties properties = {};
		vkGetPhysicalDeviceProperties(devices[i], &properties);
		std::printf("%zu: %s (Vulkan %s, acceleration structures: %s)\n", i, properties.deviceName, version_string(properties.apiVersion).c_str(), supports_acceleration_structures(devices[i]) ? "yes" : "no");
	}
}

// --device matches a substring of the device name, case-insensitively; --gpu-index picks the
// enumeration index. Without either, the first discrete GPU with acceleration structures wins.
static VkPhysicalDevice select_device(const Context &p_ctx, const Options &p_options) {
	const std::vector<VkPhysicalDevice> devices = physical_devices(p_ctx);
	if (devices.empty()) {
		fatal("no Vulkan devices");
	}
	if (!p_options.device.empty()) {
		for (VkPhysicalDevice device : devices) {
			VkPhysicalDeviceProperties properties = {};
			vkGetPhysicalDeviceProperties(device, &properties);
			if (lowercase(properties.deviceName).find(lowercase(p_options.device)) != std::string::npos) {
				return device;
			}
		}
		fatal("no Vulkan device name contains \"%s\" (see --list-devices)", p_options.device.c_str());
	}
	if (p_options.gpu_index >= 0) {
		if (size_t(p_options.gpu_index) >= devices.size()) {
			fatal("--gpu-index %d is out of range (%zu devices)", p_options.gpu_index, devices.size());
		}
		return devices[p_options.gpu_index];
	}
	VkPhysicalDevice fallback = VK_NULL_HANDLE;
	for (VkPhysicalDevice device : devices) {
		if (!supports_acceleration_structures(device)) {
			continue;
		}
		VkPhysicalDeviceProperties properties = {};
		vkGetPhysicalDeviceProperties(device, &properties);
		if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			return device;
		}
		if (fallback == VK_NULL_HANDLE) {
			fallback = device;
		}
	}
	return fallback != VK_NULL_HANDLE ? fallback : devices[0];
}

template <typename T>
static void load_device_function(const Context &p_ctx, T &r_function, const char *p_name) {
	r_function = (T)vkGetDeviceProcAddr(p_ctx.device, p_name);
	if (r_function == nullptr) {
		fatal("%s is missing", p_name);
	}
}

// Returns an empty string on success, or the fallback reason when the device cannot build
// acceleration structures.
static std::string create_device(Context &r_ctx, const Options &p_options, ResultInfo &r_info) {
	VkPhysicalDeviceProperties properties = {};
	vkGetPhysicalDeviceProperties(r_ctx.physical_device, &properties);
	r_info.adapter = properties.deviceName;
	r_info.vendor_id = properties.vendorID;
	r_info.device_id = properties.deviceID;
	r_info.api_version = version_string(properties.apiVersion);
	r_info.driver_version = driver_version_string(properties.vendorID, properties.driverVersion);
	if (properties.apiVersion < VK_API_VERSION_1_3) {
		return "VULKAN_1_3_REQUIRED";
	}
	const std::vector<VkExtensionProperties> extensions = device_extensions(r_ctx.physical_device);
	if (!supports_acceleration_structures(r_ctx.physical_device)) {
		return "NO_ACCELERATION_STRUCTURE";
	}
	const bool nv_validation_extension = p_options.validation && has_extension(extensions, VK_NV_RAY_TRACING_VALIDATION_EXTENSION_NAME);

	VkPhysicalDeviceVulkan12Features features12 = {};
	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	VkPhysicalDeviceVulkan13Features features13 = {};
	features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	VkPhysicalDeviceAccelerationStructureFeaturesKHR as_features = {};
	as_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	VkPhysicalDeviceRayTracingValidationFeaturesNV nv_validation_features = {};
	nv_validation_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_VALIDATION_FEATURES_NV;
	features12.pNext = &features13;
	features13.pNext = &as_features;
	as_features.pNext = nv_validation_extension ? &nv_validation_features : nullptr;
	VkPhysicalDeviceFeatures2 features = {};
	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features.pNext = &features12;
	vkGetPhysicalDeviceFeatures2(r_ctx.physical_device, &features);
	if (!features12.bufferDeviceAddress || !features13.synchronization2 || !as_features.accelerationStructure) {
		return "NO_ACCELERATION_STRUCTURE";
	}

	// Enable only what the benchmark uses.
	VkPhysicalDeviceVulkan12Features enable12 = {};
	enable12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	enable12.bufferDeviceAddress = VK_TRUE;
	VkPhysicalDeviceVulkan13Features enable13 = {};
	enable13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	enable13.synchronization2 = VK_TRUE;
	VkPhysicalDeviceAccelerationStructureFeaturesKHR enable_as = {};
	enable_as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	enable_as.accelerationStructure = VK_TRUE;
	VkPhysicalDeviceRayTracingValidationFeaturesNV enable_nv = {};
	enable_nv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_VALIDATION_FEATURES_NV;
	enable_nv.rayTracingValidation = VK_TRUE;
	enable12.pNext = &enable13;
	enable13.pNext = &enable_as;
	r_info.nv_ray_tracing_validation = nv_validation_extension && nv_validation_features.rayTracingValidation;
	enable_as.pNext = r_info.nv_ray_tracing_validation ? &enable_nv : nullptr;

	std::vector<const char *> enabled_extensions = { VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME };
	if (r_info.nv_ray_tracing_validation) {
		enabled_extensions.push_back(VK_NV_RAY_TRACING_VALIDATION_EXTENSION_NAME);
	}

	// The main queue, as RD uses: the first graphics family with timestamps, else compute.
	uint32_t family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(r_ctx.physical_device, &family_count, nullptr);
	std::vector<VkQueueFamilyProperties> families(family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(r_ctx.physical_device, &family_count, families.data());
	int family = -1;
	for (const VkQueueFlags wanted : { VkQueueFlags(VK_QUEUE_GRAPHICS_BIT), VkQueueFlags(VK_QUEUE_COMPUTE_BIT) }) {
		for (uint32_t i = 0; i < family_count && family < 0; i++) {
			if ((families[i].queueFlags & wanted) && families[i].timestampValidBits > 0) {
				family = int(i);
			}
		}
	}
	if (family < 0 || properties.limits.timestampPeriod <= 0.0f) {
		fatal("%s has no queue with timestamps", properties.deviceName);
	}
	r_ctx.queue_family = uint32_t(family);
	r_ctx.timestamp_valid_bits = families[family].timestampValidBits;
	r_ctx.timestamp_period_ns = properties.limits.timestampPeriod;

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info = {};
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = r_ctx.queue_family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	VkDeviceCreateInfo device_info = {};
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.pNext = &enable12;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.enabledExtensionCount = uint32_t(enabled_extensions.size());
	device_info.ppEnabledExtensionNames = enabled_extensions.data();
	VK_CHECK(vkCreateDevice(r_ctx.physical_device, &device_info, nullptr, &r_ctx.device));
	vkGetDeviceQueue(r_ctx.device, r_ctx.queue_family, 0, &r_ctx.queue);
	vkGetPhysicalDeviceMemoryProperties(r_ctx.physical_device, &r_ctx.memory_properties);

	VkPhysicalDeviceAccelerationStructurePropertiesKHR as_properties = {};
	as_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
	VkPhysicalDeviceProperties2 properties2 = {};
	properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	properties2.pNext = &as_properties;
	vkGetPhysicalDeviceProperties2(r_ctx.physical_device, &properties2);
	r_ctx.scratch_alignment = std::max<VkDeviceSize>(1, as_properties.minAccelerationStructureScratchOffsetAlignment);
	if (as_properties.maxInstanceCount < TLAS_INSTANCES) {
		fatal("%s allows %llu TLAS instances, fewer than %u", properties.deviceName, (unsigned long long)as_properties.maxInstanceCount, TLAS_INSTANCES);
	}

	load_device_function(r_ctx, r_ctx.CreateAccelerationStructureKHR, "vkCreateAccelerationStructureKHR");
	load_device_function(r_ctx, r_ctx.DestroyAccelerationStructureKHR, "vkDestroyAccelerationStructureKHR");
	load_device_function(r_ctx, r_ctx.GetAccelerationStructureBuildSizesKHR, "vkGetAccelerationStructureBuildSizesKHR");
	load_device_function(r_ctx, r_ctx.GetAccelerationStructureDeviceAddressKHR, "vkGetAccelerationStructureDeviceAddressKHR");
	load_device_function(r_ctx, r_ctx.CmdBuildAccelerationStructuresKHR, "vkCmdBuildAccelerationStructuresKHR");
	load_device_function(r_ctx, r_ctx.GetBufferDeviceAddress, "vkGetBufferDeviceAddress");
	load_device_function(r_ctx, r_ctx.CmdPipelineBarrier2, "vkCmdPipelineBarrier2");
	load_device_function(r_ctx, r_ctx.CmdWriteTimestamp2, "vkCmdWriteTimestamp2");

	VkCommandPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = r_ctx.queue_family;
	VK_CHECK(vkCreateCommandPool(r_ctx.device, &pool_info, nullptr, &r_ctx.command_pool));
	VkCommandBufferAllocateInfo allocate_info = {};
	allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate_info.commandPool = r_ctx.command_pool;
	allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate_info.commandBufferCount = 1;
	VK_CHECK(vkAllocateCommandBuffers(r_ctx.device, &allocate_info, &r_ctx.command_buffer));
	VkFenceCreateInfo fence_info = {};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VK_CHECK(vkCreateFence(r_ctx.device, &fence_info, nullptr, &r_ctx.fence));
	VkQueryPoolCreateInfo query_info = {};
	query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	query_info.queryCount = BLAS_COUNT + 1;
	VK_CHECK(vkCreateQueryPool(r_ctx.device, &query_info, nullptr, &r_ctx.query_pool));
	return "";
}

static void destroy_context(Context &r_ctx) {
	if (r_ctx.device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(r_ctx.device);
		vkDestroyQueryPool(r_ctx.device, r_ctx.query_pool, nullptr);
		vkDestroyFence(r_ctx.device, r_ctx.fence, nullptr);
		vkDestroyCommandPool(r_ctx.device, r_ctx.command_pool, nullptr);
		vkDestroyDevice(r_ctx.device, nullptr);
		r_ctx.device = VK_NULL_HANDLE;
	}
	if (r_ctx.messenger != VK_NULL_HANDLE) {
		PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(r_ctx.instance, "vkDestroyDebugUtilsMessengerEXT");
		if (destroy_messenger != nullptr) {
			destroy_messenger(r_ctx.instance, r_ctx.messenger, nullptr);
		}
		r_ctx.messenger = VK_NULL_HANDLE;
	}
	if (r_ctx.instance != VK_NULL_HANDLE) {
		vkDestroyInstance(r_ctx.instance, nullptr);
		r_ctx.instance = VK_NULL_HANDLE;
	}
}

static uint32_t find_memory_type(const Context &p_ctx, uint32_t p_type_bits, VkMemoryPropertyFlags p_required, VkMemoryPropertyFlags p_avoided) {
	for (const bool strict : { true, false }) {
		for (uint32_t i = 0; i < p_ctx.memory_properties.memoryTypeCount; i++) {
			const VkMemoryPropertyFlags flags = p_ctx.memory_properties.memoryTypes[i].propertyFlags;
			if ((p_type_bits & (1u << i)) && (flags & p_required) == p_required && (!strict || (flags & p_avoided) == 0)) {
				return i;
			}
		}
	}
	fatal("no memory type with property flags 0x%x", unsigned(p_required));
}

// p_host: host-visible, coherent memory, preferring system memory as RD's CPU allocations do
// (the TLAS instances, staging). Otherwise device-local memory.
static Buffer create_buffer(Context &r_ctx, VkDeviceSize p_size, VkBufferUsageFlags p_usage, bool p_host) {
	Buffer buffer;
	buffer.size = p_size;
	VkBufferCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = p_size;
	info.usage = p_usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK(vkCreateBuffer(r_ctx.device, &info, nullptr, &buffer.handle));
	VkMemoryRequirements requirements = {};
	vkGetBufferMemoryRequirements(r_ctx.device, buffer.handle, &requirements);
	VkMemoryAllocateFlagsInfo flags_info = {};
	flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	VkMemoryAllocateInfo allocate_info = {};
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.pNext = (p_usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? &flags_info : nullptr;
	allocate_info.allocationSize = requirements.size;
	if (p_host) {
		allocate_info.memoryTypeIndex = find_memory_type(r_ctx, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	} else {
		allocate_info.memoryTypeIndex = find_memory_type(r_ctx, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	}
	VK_CHECK(vkAllocateMemory(r_ctx.device, &allocate_info, nullptr, &buffer.memory));
	VK_CHECK(vkBindBufferMemory(r_ctx.device, buffer.handle, buffer.memory, 0));
	if (p_host) {
		VK_CHECK(vkMapMemory(r_ctx.device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped));
	}
	if (p_usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
		VkBufferDeviceAddressInfo address_info = {};
		address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
		address_info.buffer = buffer.handle;
		buffer.address = r_ctx.GetBufferDeviceAddress(r_ctx.device, &address_info);
	}
	return buffer;
}

static void destroy_buffer(Context &r_ctx, Buffer &r_buffer) {
	if (r_buffer.handle != VK_NULL_HANDLE) {
		vkDestroyBuffer(r_ctx.device, r_buffer.handle, nullptr);
	}
	if (r_buffer.memory != VK_NULL_HANDLE) {
		vkFreeMemory(r_ctx.device, r_buffer.memory, nullptr);
	}
	r_buffer = Buffer();
}

static void memory_barrier(Context &r_ctx, VkPipelineStageFlags2 p_src_stages, VkAccessFlags2 p_src_access, VkPipelineStageFlags2 p_dst_stages, VkAccessFlags2 p_dst_access) {
	VkMemoryBarrier2 barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	barrier.srcStageMask = p_src_stages;
	barrier.srcAccessMask = p_src_access;
	barrier.dstStageMask = p_dst_stages;
	barrier.dstAccessMask = p_dst_access;
	VkDependencyInfo dependency = {};
	dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers = &barrier;
	r_ctx.CmdPipelineBarrier2(r_ctx.command_buffer, &dependency);
}

// After every build: the next build reads or writes acceleration structures and scratch.
static void build_barrier(Context &r_ctx) {
	memory_barrier(r_ctx, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
			VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
}

// Starts a command buffer. It opens with a full barrier against the previous submissions, and
// resets p_queries timestamp queries.
static void begin_commands(Context &r_ctx, uint32_t p_queries = 0) {
	VK_CHECK(vkResetCommandBuffer(r_ctx.command_buffer, 0));
	VkCommandBufferBeginInfo begin_info = {};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(vkBeginCommandBuffer(r_ctx.command_buffer, &begin_info));
	memory_barrier(r_ctx, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
	if (p_queries > 0) {
		vkCmdResetQueryPool(r_ctx.command_buffer, r_ctx.query_pool, 0, p_queries);
	}
}

static void write_timestamp(Context &r_ctx, uint32_t p_query) {
	r_ctx.CmdWriteTimestamp2(r_ctx.command_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, r_ctx.query_pool, p_query);
}

// Submits the command buffer, waits for it and returns the p_queries raw timestamps.
static std::vector<uint64_t> submit_and_wait(Context &r_ctx, uint32_t p_queries = 0) {
	VK_CHECK(vkEndCommandBuffer(r_ctx.command_buffer));
	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &r_ctx.command_buffer;
	VK_CHECK(vkQueueSubmit(r_ctx.queue, 1, &submit, r_ctx.fence));
	VK_CHECK(vkWaitForFences(r_ctx.device, 1, &r_ctx.fence, VK_TRUE, UINT64_MAX));
	VK_CHECK(vkResetFences(r_ctx.device, 1, &r_ctx.fence));
	std::vector<uint64_t> times(p_queries);
	if (p_queries > 0) {
		VK_CHECK(vkGetQueryPoolResults(r_ctx.device, r_ctx.query_pool, 0, p_queries, times.size() * sizeof(uint64_t), times.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
	}
	return times;
}

static double elapsed_ms(const Context &p_ctx, const std::vector<uint64_t> &p_times, size_t p_begin, size_t p_end) {
	return timestamp_ms(p_times[p_begin], p_times[p_end], p_ctx.timestamp_valid_bits, p_ctx.timestamp_period_ns);
}

static void upload(Context &r_ctx, const Buffer &p_destination, const void *p_data, VkDeviceSize p_size) {
	Buffer staging = create_buffer(r_ctx, p_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
	std::memcpy(staging.mapped, p_data, size_t(p_size));
	begin_commands(r_ctx);
	VkBufferCopy region = {};
	region.size = p_size;
	vkCmdCopyBuffer(r_ctx.command_buffer, staging.handle, p_destination.handle, 1, &region);
	submit_and_wait(r_ctx);
	destroy_buffer(r_ctx, staging);
}

// A build input in the RT-arena format: a device-local storage buffer with a device address,
// read-only to builds.
static Buffer create_input_buffer(Context &r_ctx, const void *p_data, VkDeviceSize p_size) {
	Buffer buffer = create_buffer(r_ctx, p_size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
	upload(r_ctx, buffer, p_data, p_size);
	return buffer;
}

static VkDeviceSize align_up(VkDeviceSize p_value, VkDeviceSize p_alignment) {
	return (p_value + p_alignment - 1) / p_alignment * p_alignment;
}

// Scratch for builds; offset() returns an aligned device address p_offset bytes in.
struct Scratch {
	Buffer buffer;
	VkDeviceAddress base = 0;

	VkDeviceAddress offset(VkDeviceSize p_offset) const { return base + p_offset; }
};

static Scratch create_scratch(Context &r_ctx, VkDeviceSize p_size) {
	Scratch scratch;
	scratch.buffer = create_buffer(r_ctx, std::max<VkDeviceSize>(p_size, 1) + r_ctx.scratch_alignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
	scratch.base = align_up(scratch.buffer.address, r_ctx.scratch_alignment);
	return scratch;
}

struct MeshInput {
	Buffer vertices;
	Buffer indices;
	uint32_t vertex_count = 0;
	uint32_t triangles = 0;
};

static MeshInput create_mesh_input(Context &r_ctx, const Mesh &p_mesh) {
	MeshInput input;
	input.vertices = create_input_buffer(r_ctx, p_mesh.vertices.data(), p_mesh.vertices.size() * sizeof(float));
	input.indices = create_input_buffer(r_ctx, p_mesh.indices.data(), p_mesh.indices.size() * sizeof(uint32_t));
	input.vertex_count = p_mesh.vertex_count();
	input.triangles = p_mesh.triangles;
	return input;
}

static void destroy_mesh_input(Context &r_ctx, MeshInput &r_input) {
	destroy_buffer(r_ctx, r_input.vertices);
	destroy_buffer(r_ctx, r_input.indices);
}

static VkAccelerationStructureGeometryKHR triangle_geometry(const MeshInput &p_input) {
	VkAccelerationStructureGeometryKHR geometry = {};
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	VkAccelerationStructureGeometryTrianglesDataKHR &triangles = geometry.geometry.triangles;
	triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	triangles.vertexData.deviceAddress = p_input.vertices.address;
	triangles.vertexStride = 3 * sizeof(float);
	triangles.maxVertex = p_input.vertex_count - 1;
	triangles.indexType = VK_INDEX_TYPE_UINT32;
	triangles.indexData.deviceAddress = p_input.indices.address;
	return geometry;
}

static VkAccelerationStructureGeometryKHR instance_geometry(const Buffer &p_instances) {
	VkAccelerationStructureGeometryKHR geometry = {};
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geometry.geometry.instances.data.deviceAddress = p_instances.address;
	return geometry;
}

static VkAccelerationStructureBuildGeometryInfoKHR build_info(VkAccelerationStructureTypeKHR p_type, VkBuildAccelerationStructureFlagsKHR p_flags, const VkAccelerationStructureGeometryKHR *p_geometry) {
	VkAccelerationStructureBuildGeometryInfoKHR info = {};
	info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	info.type = p_type;
	info.flags = p_flags;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = p_geometry;
	return info;
}

static AccelerationStructure create_acceleration_structure(Context &r_ctx, VkAccelerationStructureTypeKHR p_type, VkBuildAccelerationStructureFlagsKHR p_flags, const VkAccelerationStructureGeometryKHR &p_geometry, uint32_t p_primitives) {
	const VkAccelerationStructureBuildGeometryInfoKHR info = build_info(p_type, p_flags, &p_geometry);
	VkAccelerationStructureBuildSizesInfoKHR sizes = {};
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	r_ctx.GetAccelerationStructureBuildSizesKHR(r_ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &p_primitives, &sizes);
	AccelerationStructure as;
	as.size = sizes.accelerationStructureSize;
	as.build_scratch = sizes.buildScratchSize;
	as.update_scratch = sizes.updateScratchSize;
	as.buffer = create_buffer(r_ctx, as.size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
	VkAccelerationStructureCreateInfoKHR create_info = {};
	create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	create_info.buffer = as.buffer.handle;
	create_info.size = as.size;
	create_info.type = p_type;
	VK_CHECK(r_ctx.CreateAccelerationStructureKHR(r_ctx.device, &create_info, nullptr, &as.handle));
	VkAccelerationStructureDeviceAddressInfoKHR address_info = {};
	address_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	address_info.accelerationStructure = as.handle;
	as.address = r_ctx.GetAccelerationStructureDeviceAddressKHR(r_ctx.device, &address_info);
	return as;
}

static void destroy_acceleration_structure(Context &r_ctx, AccelerationStructure &r_as) {
	if (r_as.handle != VK_NULL_HANDLE) {
		r_ctx.DestroyAccelerationStructureKHR(r_ctx.device, r_as.handle, nullptr);
	}
	destroy_buffer(r_ctx, r_as.buffer);
	r_as = AccelerationStructure();
}

// Records one build. UPDATE refits in place: the structure is both the source and the destination.
static void record_build(Context &r_ctx, const AccelerationStructure &p_as, VkAccelerationStructureTypeKHR p_type, VkBuildAccelerationStructureFlagsKHR p_flags, const VkAccelerationStructureGeometryKHR &p_geometry, uint32_t p_primitives, VkBuildAccelerationStructureModeKHR p_mode, VkDeviceAddress p_scratch) {
	VkAccelerationStructureBuildGeometryInfoKHR info = build_info(p_type, p_flags, &p_geometry);
	info.mode = p_mode;
	info.srcAccelerationStructure = p_mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR ? p_as.handle : VK_NULL_HANDLE;
	info.dstAccelerationStructure = p_as.handle;
	info.scratchData.deviceAddress = p_scratch;
	VkAccelerationStructureBuildRangeInfoKHR range = {};
	range.primitiveCount = p_primitives;
	const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
	r_ctx.CmdBuildAccelerationStructuresKHR(r_ctx.command_buffer, 1, &info, &ranges);
}

/* Measurements */

// The FAST_TRACE BLAS set: one submit per iteration with a timestamp between builds, as RD's
// graph orders them. Returns the BLASes, which the TLAS instances reference.
static BlasSetResult time_blas_set(Context &r_ctx, const std::vector<MeshInput> &p_inputs, int p_warmup, int p_iters, std::vector<AccelerationStructure> &r_blases) {
	const VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	std::vector<VkAccelerationStructureGeometryKHR> geometries;
	BlasSetResult result;
	for (const MeshInput &input : p_inputs) {
		geometries.push_back(triangle_geometry(input));
		r_blases.push_back(create_acceleration_structure(r_ctx, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, flags, geometries.back(), input.triangles));
		result.triangles += input.triangles;
		result.memory_bytes += r_blases.back().size;
	}
	// Each build has its own scratch range, as RD's frame scratch ring gives them.
	std::vector<VkDeviceSize> offsets;
	VkDeviceSize scratch_size = 0;
	for (const AccelerationStructure &blas : r_blases) {
		offsets.push_back(scratch_size);
		scratch_size += align_up(blas.build_scratch, r_ctx.scratch_alignment);
	}
	Scratch scratch = create_scratch(r_ctx, scratch_size);
	const uint32_t queries = uint32_t(r_blases.size()) + 1;
	std::vector<std::vector<double>> per_mesh(r_blases.size());
	for (int iteration = 0; iteration < p_warmup + p_iters; iteration++) {
		begin_commands(r_ctx, queries);
		write_timestamp(r_ctx, 0);
		for (size_t i = 0; i < r_blases.size(); i++) {
			record_build(r_ctx, r_blases[i], VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, flags, geometries[i], p_inputs[i].triangles, VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, scratch.offset(offsets[i]));
			build_barrier(r_ctx);
			write_timestamp(r_ctx, uint32_t(i + 1));
		}
		const std::vector<uint64_t> times = submit_and_wait(r_ctx, queries);
		if (iteration < p_warmup) {
			continue;
		}
		for (size_t i = 0; i < r_blases.size(); i++) {
			per_mesh[i].push_back(elapsed_ms(r_ctx, times, i, i + 1));
		}
	}
	destroy_buffer(r_ctx, scratch.buffer);
	for (const std::vector<double> &samples : per_mesh) {
		result.p50_ms_total += percentile(samples, 0.50);
		result.p95_ms_total += percentile(samples, 0.95);
	}
	result.triangles_per_ms = result.p50_ms_total > 0.0 ? double(result.triangles) / result.p50_ms_total : 0.0;
	return result;
}

// The 100k-instance FAST_TRACE TLAS over the BLAS set: instances in host-visible memory, as RD's
// tlas_build writes them.
static TimedPoint time_tlas(Context &r_ctx, const std::vector<AccelerationStructure> &p_blases, int p_warmup, int p_iters) {
	const std::vector<Transform> transforms = make_transforms(TLAS_INSTANCES);
	Buffer instances = create_buffer(r_ctx, VkDeviceSize(TLAS_INSTANCES) * sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
	for (uint32_t i = 0; i < TLAS_INSTANCES; i++) {
		VkAccelerationStructureInstanceKHR instance = {};
		std::memcpy(instance.transform.matrix, transforms[i].rows, sizeof(instance.transform.matrix));
		instance.instanceCustomIndex = i;
		instance.mask = 0xFF;
		instance.instanceShaderBindingTableRecordOffset = 0;
		instance.flags = 0;
		instance.accelerationStructureReference = p_blases[i % p_blases.size()].address;
		// Filled on the stack and copied whole: the bit fields would read write-combined memory.
		std::memcpy(static_cast<uint8_t *>(instances.mapped) + size_t(i) * sizeof(instance), &instance, sizeof(instance));
	}
	const VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	const VkAccelerationStructureGeometryKHR geometry = instance_geometry(instances);
	AccelerationStructure tlas = create_acceleration_structure(r_ctx, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, flags, geometry, TLAS_INSTANCES);
	Scratch scratch = create_scratch(r_ctx, tlas.build_scratch);
	std::vector<double> samples;
	for (int iteration = 0; iteration < p_warmup + p_iters; iteration++) {
		begin_commands(r_ctx, 2);
		write_timestamp(r_ctx, 0);
		record_build(r_ctx, tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, flags, geometry, TLAS_INSTANCES, VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, scratch.offset(0));
		build_barrier(r_ctx);
		write_timestamp(r_ctx, 1);
		const std::vector<uint64_t> times = submit_and_wait(r_ctx, 2);
		if (iteration >= p_warmup) {
			samples.push_back(elapsed_ms(r_ctx, times, 0, 1));
		}
	}
	TimedPoint point;
	point.count = TLAS_INSTANCES;
	point.p50_ms = percentile(samples, 0.50);
	point.p95_ms = percentile(samples, 0.95);
	point.memory_bytes = tlas.size;
	destroy_buffer(r_ctx, scratch.buffer);
	destroy_acceleration_structure(r_ctx, tlas);
	destroy_buffer(r_ctx, instances);
	return point;
}

// Refit: a FAST_TRACE + ALLOW_UPDATE BLAS, built once, then refitted in UPDATE mode after every
// vertex change. Two vertex sets alternate, copied in before the first timestamp as RD's
// buffer_update does, so every refit moves every vertex.
static TimedPoint time_refit(Context &r_ctx, uint32_t p_triangles, int p_warmup, int p_iters) {
	std::vector<float> other;
	const Mesh mesh = refit_mesh(p_triangles, other);
	MeshInput input = create_mesh_input(r_ctx, mesh);
	const VkDeviceSize vertex_bytes = mesh.vertices.size() * sizeof(float);
	Buffer sets[2];
	const std::vector<float> *set_data[2] = { &mesh.vertices, &other };
	for (int i = 0; i < 2; i++) {
		sets[i] = create_buffer(r_ctx, vertex_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
		upload(r_ctx, sets[i], set_data[i]->data(), vertex_bytes);
	}
	const VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	const VkAccelerationStructureGeometryKHR geometry = triangle_geometry(input);
	AccelerationStructure blas = create_acceleration_structure(r_ctx, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, flags, geometry, mesh.triangles);
	Scratch scratch = create_scratch(r_ctx, std::max(blas.build_scratch, blas.update_scratch));
	begin_commands(r_ctx);
	record_build(r_ctx, blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, flags, geometry, mesh.triangles, VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, scratch.offset(0));
	submit_and_wait(r_ctx);
	std::vector<double> samples;
	for (int iteration = 0; iteration < p_warmup + p_iters; iteration++) {
		begin_commands(r_ctx, 2);
		VkBufferCopy region = {};
		region.size = vertex_bytes;
		vkCmdCopyBuffer(r_ctx.command_buffer, sets[(iteration + 1) % 2].handle, input.vertices.handle, 1, &region);
		// Build inputs are read as shader reads in the build stage.
		memory_barrier(r_ctx, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT);
		write_timestamp(r_ctx, 0);
		record_build(r_ctx, blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, flags, geometry, mesh.triangles, VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR, scratch.offset(0));
		build_barrier(r_ctx);
		write_timestamp(r_ctx, 1);
		const std::vector<uint64_t> times = submit_and_wait(r_ctx, 2);
		if (iteration >= p_warmup) {
			samples.push_back(elapsed_ms(r_ctx, times, 0, 1));
		}
	}
	TimedPoint point;
	point.count = mesh.triangles;
	point.p50_ms = percentile(samples, 0.50);
	point.p95_ms = percentile(samples, 0.95);
	point.triangles_per_ms = point.p50_ms > 0.0 ? mesh.triangles / point.p50_ms : 0.0;
	point.memory_bytes = blas.size;
	destroy_buffer(r_ctx, scratch.buffer);
	destroy_acceleration_structure(r_ctx, blas);
	for (Buffer &set : sets) {
		destroy_buffer(r_ctx, set);
	}
	destroy_mesh_input(r_ctx, input);
	return point;
}

static RunResult measure(Context &r_ctx, const std::vector<MeshInput> &p_inputs, const Options &p_options, ResultInfo &r_info) {
	RunResult run;
	std::vector<AccelerationStructure> blases;
	run.blas_fast_trace = time_blas_set(r_ctx, p_inputs, p_options.warmup, p_options.iters, blases);
	run.tlas = time_tlas(r_ctx, blases, p_options.warmup, p_options.iters);
	for (AccelerationStructure &blas : blases) {
		destroy_acceleration_structure(r_ctx, blas);
	}
	for (const uint32_t size : p_options.refit_sizes) {
		run.refit.push_back(time_refit(r_ctx, size, p_options.warmup, p_options.iters));
	}
	auto check = [&r_info](bool p_condition, const std::string &p_what) {
		if (!p_condition && std::find(r_info.failures.begin(), r_info.failures.end(), p_what) == r_info.failures.end()) {
			r_info.failures.push_back(p_what);
		}
	};
	check(run.blas_fast_trace.p50_ms_total > 0.0, "BLAS timestamps are nonzero");
	check(run.tlas.p95_ms > 0.0, "TLAS timestamps are nonzero");
	for (const TimedPoint &point : run.refit) {
		check(point.p50_ms > 0.0, "refit of " + std::to_string(point.count) + " triangles has nonzero timestamps");
	}
	return run;
}

/* Self-test: the CPU-side parts, without a GPU (CTest runs it) */

static int self_test_failures = 0;
static int self_test_checks = 0;

static void expect(bool p_condition, const char *p_what) {
	self_test_checks++;
	if (!p_condition) {
		self_test_failures++;
		std::fprintf(stderr, "as_refit_bench self-test: FAILED: %s\n", p_what);
	}
}

static int run_self_test() {
	// The meshes match the RD benchmark's counts (tests/as_calibration/README.md).
	const std::vector<Mesh> meshes = make_meshes();
	uint64_t total = 0;
	bool shapes = true;
	bool displacement = true;
	for (const Mesh &mesh : meshes) {
		total += mesh.triangles;
		shapes = shapes && mesh.indices.size() == size_t(mesh.triangles) * 3 && *std::max_element(mesh.indices.begin(), mesh.indices.end()) < mesh.vertex_count();
		for (size_t i = 2; i < mesh.vertices.size(); i += 3) {
			displacement = displacement && std::fabs(mesh.vertices[i]) <= DISPLACEMENT;
		}
	}
	expect(meshes.size() == BLAS_COUNT, "64 BLAS meshes");
	expect(total == 2137242, "the BLAS set has 2,137,242 triangles, as in the RD benchmark");
	expect(meshes.front().triangles == 1058 && meshes.back().triangles == 65884, "the smallest and largest meshes have 1,058 and 65,884 triangles");
	expect(shapes, "every mesh has 3 indices per triangle, all within its vertices");
	expect(displacement, "every displacement is within 0.25 m");
	expect(make_meshes()[7].vertices == meshes[7].vertices, "the meshes are deterministic");

	const uint32_t refit_sizes[4][2] = { { 10000, 10082 }, { 50000, 50244 }, { 100000, 100352 }, { 200000, 200344 } };
	for (const auto &size : refit_sizes) {
		std::vector<float> other;
		const Mesh mesh = refit_mesh(size[0], other);
		expect(mesh.triangles == size[1], "a refit mesh has the RD benchmark's triangle count");
		bool moved = other.size() == mesh.vertices.size();
		for (size_t i = 0; moved && i < other.size(); i++) {
			moved = (i % 3 == 2) ? true : other[i] == mesh.vertices[i];
		}
		expect(moved, "the second refit vertex set changes only z");
	}

	const std::vector<Transform> transforms = make_transforms(1000);
	bool orthonormal = true;
	bool inside = true;
	for (const Transform &transform : transforms) {
		for (int a = 0; a < 3; a++) {
			for (int b = 0; b < 3; b++) {
				const double dot = double(transform.rows[a][0]) * transform.rows[b][0] + double(transform.rows[a][1]) * transform.rows[b][1] + double(transform.rows[a][2]) * transform.rows[b][2];
				orthonormal = orthonormal && std::fabs(dot - (a == b ? 1.0 : 0.0)) < 1e-4;
			}
		}
		const double distance = std::sqrt(double(transform.rows[0][3]) * transform.rows[0][3] + double(transform.rows[1][3]) * transform.rows[1][3] + double(transform.rows[2][3]) * transform.rows[2][3]);
		inside = inside && distance <= SCENE_RADIUS + 1e-3;
	}
	expect(orthonormal, "instance rotations are orthonormal");
	expect(inside, "instances lie within 200 m");

	expect(percentile({ 5, 1, 4, 2, 3, 6, 7, 8, 9, 10 }, 0.50) == 5.0, "nearest-rank p50");
	expect(percentile({ 5, 1, 4, 2, 3, 6, 7, 8, 9, 10 }, 0.95) == 10.0, "nearest-rank p95");
	expect(percentile({}, 0.95) == 0.0 && percentile({ 3 }, 0.5) == 3.0, "percentile of empty and single lists");
	expect(median({ 3, 1, 2 }) == 2.0 && median({ 1, 2, 3, 4 }) == 2.5 && median({}) == 0.0, "median of runs");
	expect(std::fabs(timestamp_ms((uint64_t(1) << 36) - 10, 5, 36, 2.0) - 30e-6) < 1e-12, "timestamps wrap at their valid bits");

	JsonWriter json;
	json.begin_object();
	json.string("a", "q\"\\\n");
	json.begin_array("b");
	json.integer(nullptr, 1);
	json.number(nullptr, 0.5);
	json.end_array();
	json.boolean("c", true);
	json.end_object();
	expect(json.get() == "{\"a\":\"q\\\"\\\\\\u000a\",\"b\":[1,0.5],\"c\":true}", "JSON writer output and escaping");

	Options options;
	std::string error;
	const char *argv[] = { "as_refit_bench", "--device", "RTX 4080 SUPER", "--runs", "3", "--refit=10000,200000", "--validation" };
	expect(parse_options(7, argv, options, error) && options.device == "RTX 4080 SUPER" && options.runs == 3 && options.validation && options.refit_sizes == std::vector<uint32_t>{ 10000, 200000 }, "options parse");
	const char *bad[] = { "as_refit_bench", "--iters", "0" };
	Options rejected;
	expect(!parse_options(3, bad, rejected, error), "--iters 0 is rejected");

	// The keys godotnext_metrics.as_calibration reads (tool_missing_fields).
	RunResult run;
	run.blas_fast_trace.triangles_per_ms = 100.0;
	run.tlas.p95_ms = 1.0;
	for (const uint32_t size : options.refit_sizes) {
		TimedPoint point;
		point.count = size;
		point.p50_ms = 1.0;
		point.triangles_per_ms = size;
		run.refit.push_back(point);
	}
	ResultInfo info;
	info.adapter = "Test GPU";
	const std::string result = result_json(info, options, { run, run, run });
	for (const char *key : { "\"adapter\":\"Test GPU\"", "\"blas_fast_trace_triangles_per_ms\":100", "\"tlas_100k_p95_ms\":1", "\"refit_triangles_per_ms\":200000", "\"refit\":{\"10000\":{\"triangles\":10000", "\"validation_errors\":0", "\"pass\":true" }) {
		expect(result.find(key) != std::string::npos, key);
	}

	std::printf("as_refit_bench self-test: %d of %d checks passed\n", self_test_checks - self_test_failures, self_test_checks);
	return self_test_failures;
}

/* Main */

static void set_environment(const char *p_name, const char *p_value) {
#ifdef _WIN32
	_putenv_s(p_name, p_value);
#else
	setenv(p_name, p_value, 0);
#endif
}

int main(int p_argc, char **p_argv) {
	Options options;
	std::string error;
	if (!parse_options(p_argc, p_argv, options, error)) {
		std::fprintf(stderr, "as_refit_bench: %s\n%s", error.c_str(), USAGE);
		return 2;
	}
	if (options.help) {
		std::printf("%s", USAGE);
		return 0;
	}
	if (options.self_test) {
		return run_self_test() == 0 ? 0 : 1;
	}
	if (!options.device.empty() && options.gpu_index >= 0) {
		std::fprintf(stderr, "as_refit_bench: give --device or --gpu-index, not both\n");
		return 2;
	}
	// NVIDIA exposes VK_NV_ray_tracing_validation only with this set, as the runner's validation
	// jobs do. It must be set before the driver loads.
	if (options.validation && std::getenv("NV_ALLOW_RAYTRACING_VALIDATION") == nullptr) {
		set_environment("NV_ALLOW_RAYTRACING_VALIDATION", "1");
	}

	Context ctx;
	ResultInfo info;
	create_instance(ctx, options, info);
	if (options.list_devices) {
		list_devices(ctx);
		destroy_context(ctx);
		return 0;
	}
	ctx.physical_device = select_device(ctx, options);
	const std::string fallback = create_device(ctx, options, info);
	if (!fallback.empty()) {
		destroy_context(ctx);
		print_result(short_result_json(true, fallback, info.adapter, true));
		return 0;
	}
	std::printf("as_refit_bench: %s, driver %s, Vulkan %s; validation %s\n", info.adapter.c_str(), info.driver_version.c_str(), info.api_version.c_str(), options.validation ? "on" : "off");

	const std::vector<Mesh> meshes = make_meshes();
	std::vector<MeshInput> inputs;
	for (const Mesh &mesh : meshes) {
		inputs.push_back(create_mesh_input(ctx, mesh));
	}
	std::vector<RunResult> runs;
	for (int run = 0; run < options.runs; run++) {
		runs.push_back(measure(ctx, inputs, options, info));
		std::printf("as_refit_bench: run %d of %d done\n", run + 1, options.runs);
	}
	for (MeshInput &input : inputs) {
		destroy_mesh_input(ctx, input);
	}
	// Teardown first, so the validation counts include anything reported at device destruction.
	destroy_context(ctx);
	info.validation_errors = validation_errors;
	info.validation_warnings = validation_warnings;

	const std::string result = result_json(info, options, runs);
	if (!options.out_dir.empty()) {
		std::error_code fs_error;
		std::filesystem::create_directories(options.out_dir, fs_error);
		const std::string path = (std::filesystem::path(options.out_dir) / "as_refit_bench.json").string();
		FILE *file = std::fopen(path.c_str(), "wb");
		if (file == nullptr) {
			info.failures.push_back("result written to " + options.out_dir);
		} else {
			std::fwrite(result.data(), 1, result.size(), file);
			std::fclose(file);
		}
	}
	const std::string final_result = info.failures.empty() ? result : result_json(info, options, runs);
	print_result(final_result);
	return info.failures.empty() && info.validation_errors == 0 ? 0 : 1;
}
