extends Node

## Async compute overlap fixture (#174 step 6; plan §21.1, GR-4).
##
## One frame holds two passes that share no resource, so with async compute active they can run
## at the same time:
##
## - compute: an ALU-bound chain of integer hashes, --rounds rounds on each of --threads threads.
##   Each thread's chain is dependent, so the pass keeps a few warps busy for a long time and
##   leaves room on the GPU.
## - raster: --layers full-screen triangles blended additively into a --size x --size RGBA16F
##   target. The fragment shader is trivial, so blending bounds the pass, not ALU.
##
## A frame is a timestamp, the compute list, the draw list and a closing timestamp, nothing else.
## A timestamp or a readback between the passes would consume the async work and put the draw
## list after the join. With async compute active, the render graph forks after the first
## timestamp, records the draw list on the main queue while the compute list runs on the async
## queue, and joins before the closing timestamp.
##
## Standalone (the default) runs on a local RenderingDevice, and waits for every frame:
## 1. Compute-only, raster-only and overlap (both passes) frames, --warmup untimed and --frames
##    timed in each phase. A frame's time is the GPU time between its two timestamps, or the
##    submit-to-sync wall time on a device without timestamps.
## 2. The device's "async:<submitted>/<fell back>" counters over each phase. When forks are
##    expected, every compute-only and overlap frame must fork and none may fall back; otherwise
##    the counters stay 0. Raster-only frames never fork.
## 3. After the overlap frames, the compute output and the raster target are read back and checked
##    against the CPU: a few compute elements, and sampled pixels against the closed form (exact
##    in FP16 while --layers is at most 2047). Their SHA-256 digests let the runner compare
##    launches with async compute off, on and forced.
## It prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure. compute_ms,
## raster_ms and frame_ms are p50 times, and "overlap" is frame_ms / (compute_ms + raster_ms). They
## are recorded, never gated; GR-4 compares frame_ms with async compute on and off in PQ.
##
## Forks are expected when async compute is active (rendering/rendering_device/async_compute on and
## SUPPORTS_ASYNC_COMPUTE) and either the compute list asks for the async queue (--queue=async, the
## default), or a debug build moves it there: --queue=main with GODOT_RG_FORCE_ASYNC_COMPUTE=1.
## Other strides never move a frame's only compute list.
##
## Harness timing mode (#12): when the user arguments hold --mode, the harness drives this scene.
## Every process frame then records the overlap frame on the main RenderingDevice, between the
## timestamps "async_overlap" and "async_overlap_end" (timing.csv column async_overlap_ms). The
## harness rejects arguments it does not know, so this mode uses the default workload;
## --variant=main puts the compute list on COMPUTE_QUEUE_MAIN. At exit the script prints a
## GODOTNEXT_RESULT line with the fork counters; the harness owns the exit code.
##
## Standalone user arguments: --queue=async|main --threads=N --rounds=N --layers=N --size=N
## --frames=N --warmup=N. tests/async_compute/run_async_compute_check.sh runs it with async compute
## off, on and forced.

const SceneArgs = preload("res://scenes/scene_args.gd")

# Issue step 6 asks for a 4 ms compute pass and a 4 ms raster pass. These defaults take about that
# on the RTX 4080 SUPER; on other GPUs, pass --rounds and --layers (compute_ms and raster_ms show
# the standalone times).
const DEFAULT_THREADS := 16384
const DEFAULT_ROUNDS := 225000
const DEFAULT_LAYERS := 560
const DEFAULT_SIZE := 1024
const DEFAULT_FRAMES := 60
const DEFAULT_WARMUP := 10
# Every layer adds 1/1024 to a channel at most once. FP16 holds every multiple of 1/1024 below
# 2048/1024 exactly, so the blended sums are exact up to 2047 layers.
const MAX_LAYERS := 2047
const UNITS_PER_ONE := 1024.0
const SALT := 0x5a17c0de
const SEED := 0x4f564c50
const SPOT_PIXELS := 64
const TIMESTAMP := "async_overlap"
const TIMESTAMP_END := "async_overlap_end"

const COMPUTE_SOURCE := """#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer Source {
	uint data[];
}
source_buffer;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Destination {
	uint data[];
}
destination_buffer;

layout(push_constant, std430) uniform Params {
	uint count;
	uint rounds;
	uint salt;
	uint pad;
}
params;

uint hash(uint x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x2c1b3c6du;
	x ^= x >> 16;
	return x;
}

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.count) {
		return;
	}
	uint value = source_buffer.data[i] ^ params.salt;
	for (uint round = 0u; round < params.rounds; round++) {
		value = hash(value + round);
	}
	destination_buffer.data[i] = value;
}
"""

# One full-screen triangle per instance.
const RASTER_VERTEX_SOURCE := """#version 450

layout(location = 0) flat out uint layer_index;

void main() {
	vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
	layer_index = uint(gl_InstanceIndex);
}
"""

# Adds 1/1024 to red, to green on pixels where x ^ y is odd, to blue where bit 3 of x ^ y is set,
# and to alpha on odd layers.
const RASTER_FRAGMENT_SOURCE := """#version 450

layout(location = 0) flat in uint layer_index;
layout(location = 0) out vec4 frag_color;

void main() {
	uvec2 pixel = uvec2(gl_FragCoord.xy);
	uint pattern = pixel.x ^ pixel.y;
	const float unit = 1.0 / 1024.0;
	frag_color = vec4(unit, float(pattern & 1u) * unit, float((pattern >> 3u) & 1u) * unit, float(layer_index & 1u) * unit);
}
"""

var _failures := PackedStringArray()

# Harness timing mode, on the main RenderingDevice.
var _harness_device: RenderingDevice
var _harness_resources := {}
var _harness_frames := 0
var _harness_counters_before: Array[int] = [0, 0]
var _harness_active := false
var _harness_queue := "async"


func _ready() -> void:
	if not SceneArgs.value("--mode", "").is_empty():
		RenderingServer.call_on_render_thread(_harness_begin)
		return
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _process(_delta: float) -> void:
	if not _harness_resources.is_empty():
		RenderingServer.call_on_render_thread(_harness_frame)


func _exit_tree() -> void:
	if not _harness_resources.is_empty():
		RenderingServer.call_on_render_thread(_harness_end)


func _run() -> Dictionary:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		return { "test": "async_overlap", "mode": "standalone", "pass": false, "reason": "NO_RENDERING_DEVICE" }

	var setting := bool(ProjectSettings.get_setting("rendering/rendering_device/async_compute", false))
	var feature := rd.has_feature(RenderingDevice.SUPPORTS_ASYNC_COMPUTE)
	var active := setting and feature
	var queue_name := SceneArgs.value("--queue", "async")
	_check(queue_name == "async" or queue_name == "main", "--queue is async or main, not '%s'" % queue_name)
	var force_text := OS.get_environment("GODOT_RG_FORCE_ASYNC_COMPUTE")
	var force_stride := int(force_text) if force_text.is_valid_int() else 0
	var debug_build := OS.is_debug_build()
	var forks_expected := active and (queue_name == "async" or (debug_build and force_stride == 1))
	var params := _params(queue_name)
	var warmup := maxi(0, SceneArgs.int_value("--warmup", DEFAULT_WARMUP))
	var frames := maxi(1, SceneArgs.int_value("--frames", DEFAULT_FRAMES))
	params["warmup"] = warmup
	params["frames"] = frames

	var result := {
		"test": "async_overlap",
		"mode": "standalone",
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": rd.get_device_name(),
		"setting": setting,
		"feature": feature,
		"active": active,
		"queue": queue_name,
		"force_stride": force_stride,
		"debug_build": debug_build,
		"forks_expected": forks_expected,
		"params": params,
	}

	var input := _make_input(params["threads"])
	var resources := _create(rd, params, input)
	if not resources.is_empty():
		var compute := _phase(rd, resources, true, false, warmup, frames)
		var raster := _phase(rd, resources, false, true, warmup, frames)
		var overlap := _phase(rd, resources, true, true, warmup, frames)
		_check_forks("compute-only", compute, warmup + frames if forks_expected else 0)
		_check_forks("raster-only", raster, 0)
		_check_forks("overlap", overlap, warmup + frames if forks_expected else 0)

		var use_gpu: bool = compute["gpu"].size() == frames and raster["gpu"].size() == frames and overlap["gpu"].size() == frames
		var key := "gpu" if use_gpu else "wall"
		var compute_ms := _percentile(compute[key], 50.0)
		var raster_ms := _percentile(raster[key], 50.0)
		var frame_ms := _percentile(overlap[key], 50.0)
		result["timing_source"] = "gpu_timestamps" if use_gpu else "wall"
		result["compute_ms"] = compute_ms
		result["raster_ms"] = raster_ms
		result["frame_ms"] = frame_ms
		result["frame_wall_ms"] = _percentile(overlap["wall"], 50.0)
		result["overlap"] = frame_ms / (compute_ms + raster_ms) if compute_ms + raster_ms > 0.0 else 0.0
		result["phases"] = { "compute": _phase_summary(compute), "raster": _phase_summary(raster), "overlap": _phase_summary(overlap) }
		result["submitted"] = overlap["submitted"]
		result["fell_back"] = overlap["fell_back"]
		result["forked"] = active and overlap["submitted"] == warmup + frames and overlap["fell_back"] == 0

		# The overlap frames ran last, so both outputs hold their results.
		var compute_data := rd.buffer_get_data(resources["output"])
		var raster_data := rd.texture_get_data(resources["color"], 0)
		result["compute_digest"] = _digest(compute_data)
		result["raster_digest"] = _digest(raster_data)
		result["compute_values"] = _check_compute(compute_data, input, params["threads"], params["rounds"])
		result["raster_values"] = _check_raster(raster_data, params["size"], params["layers"])
		_free_resources(rd, resources)
	rd.free()

	result["pass"] = _failures.is_empty()
	result["failures"] = _failures
	return result


func _params(queue_name: String) -> Dictionary:
	# Whole workgroups of 64 threads.
	var threads := ceili(maxi(64, SceneArgs.int_value("--threads", DEFAULT_THREADS)) / 64.0) * 64
	return {
		"queue": queue_name,
		"threads": threads,
		"rounds": maxi(1, SceneArgs.int_value("--rounds", DEFAULT_ROUNDS)),
		"layers": clampi(SceneArgs.int_value("--layers", DEFAULT_LAYERS), 1, MAX_LAYERS),
		"size": maxi(16, SceneArgs.int_value("--size", DEFAULT_SIZE)),
	}


# Creates the shaders, pipelines, buffers, the raster target and its framebuffer. Returns an empty
# dictionary, after freeing what it made, when any of them fails.
func _create(rd: RenderingDevice, params: Dictionary, input: PackedByteArray) -> Dictionary:
	var resources := {
		"queue": RenderingDevice.COMPUTE_QUEUE_ASYNC if params["queue"] == "async" else RenderingDevice.COMPUTE_QUEUE_MAIN,
		"threads": params["threads"],
		"rounds": params["rounds"],
		"layers": params["layers"],
	}

	var compute_source := RDShaderSource.new()
	compute_source.source_compute = COMPUTE_SOURCE
	var compute_spirv := rd.shader_compile_spirv_from_source(compute_source)
	_check(compute_spirv.compile_error_compute.is_empty(), "the compute kernel compiles: " + compute_spirv.compile_error_compute)
	if compute_spirv.compile_error_compute.is_empty():
		resources["compute_shader"] = rd.shader_create_from_spirv(compute_spirv, "async_overlap_compute")

	var raster_source := RDShaderSource.new()
	raster_source.source_vertex = RASTER_VERTEX_SOURCE
	raster_source.source_fragment = RASTER_FRAGMENT_SOURCE
	var raster_spirv := rd.shader_compile_spirv_from_source(raster_source)
	var raster_errors := raster_spirv.compile_error_vertex + raster_spirv.compile_error_fragment
	_check(raster_errors.is_empty(), "the raster shaders compile: " + raster_errors)
	if raster_errors.is_empty():
		resources["raster_shader"] = rd.shader_create_from_spirv(raster_spirv, "async_overlap_raster")

	if not _valid(resources, "compute_shader") or not _valid(resources, "raster_shader"):
		_check(false, "the shaders are created")
		_free_resources(rd, resources)
		return {}

	resources["compute_pipeline"] = rd.compute_pipeline_create(resources["compute_shader"])
	resources["input"] = rd.storage_buffer_create(input.size(), input)
	resources["output"] = rd.storage_buffer_create(input.size())
	var uniforms: Array[RDUniform] = []
	var bindings: Array[RID] = [resources["input"], resources["output"]]
	for i in 2:
		var uniform := RDUniform.new()
		uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		uniform.binding = i
		uniform.add_id(bindings[i])
		uniforms.append(uniform)
	resources["uniform_set"] = rd.uniform_set_create(uniforms, resources["compute_shader"], 0)

	var format := RDTextureFormat.new()
	format.format = RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT
	format.width = params["size"]
	format.height = params["size"]
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT
	resources["color"] = rd.texture_create(format, RDTextureView.new())
	var framebuffer_textures: Array[RID] = [resources["color"]]
	resources["framebuffer"] = rd.framebuffer_create(framebuffer_textures)
	if _valid(resources, "framebuffer"):
		var blend_attachment := RDPipelineColorBlendStateAttachment.new()
		blend_attachment.enable_blend = true
		blend_attachment.src_color_blend_factor = RenderingDevice.BLEND_FACTOR_ONE
		blend_attachment.dst_color_blend_factor = RenderingDevice.BLEND_FACTOR_ONE
		blend_attachment.color_blend_op = RenderingDevice.BLEND_OP_ADD
		blend_attachment.src_alpha_blend_factor = RenderingDevice.BLEND_FACTOR_ONE
		blend_attachment.dst_alpha_blend_factor = RenderingDevice.BLEND_FACTOR_ONE
		blend_attachment.alpha_blend_op = RenderingDevice.BLEND_OP_ADD
		var blend_attachments: Array[RDPipelineColorBlendStateAttachment] = [blend_attachment]
		var blend := RDPipelineColorBlendState.new()
		blend.attachments = blend_attachments
		resources["raster_pipeline"] = rd.render_pipeline_create(resources["raster_shader"], rd.framebuffer_get_format(resources["framebuffer"]), RenderingDevice.INVALID_FORMAT_ID, RenderingDevice.RENDER_PRIMITIVE_TRIANGLES, RDPipelineRasterizationState.new(), RDPipelineMultisampleState.new(), RDPipelineDepthStencilState.new(), blend)

	for key in ["compute_pipeline", "input", "output", "uniform_set", "color", "framebuffer", "raster_pipeline"]:
		if not _valid(resources, key):
			_check(false, "%s is created" % key)
			_free_resources(rd, resources)
			return {}
	return resources


func _free_resources(rd: RenderingDevice, resources: Dictionary) -> void:
	# Dependents first: freeing a texture, buffer or shader also frees what was made from it.
	for key in ["framebuffer", "raster_pipeline", "uniform_set", "compute_pipeline", "color", "input", "output", "raster_shader", "compute_shader"]:
		if _valid(resources, key):
			rd.free_rid(resources[key])
	resources.clear()


func _valid(resources: Dictionary, key: String) -> bool:
	return resources.has(key) and RID(resources[key]).is_valid()


# Records one frame: a timestamp, the passes, and a closing timestamp.
func _record(rd: RenderingDevice, resources: Dictionary, compute: bool, raster: bool) -> void:
	rd.capture_timestamp(TIMESTAMP)
	if compute:
		var list := rd.compute_list_begin(resources["queue"])
		rd.compute_list_bind_compute_pipeline(list, resources["compute_pipeline"])
		rd.compute_list_bind_uniform_set(list, resources["uniform_set"], 0)
		var push := PackedInt32Array([resources["threads"], resources["rounds"], SALT, 0]).to_byte_array()
		rd.compute_list_set_push_constant(list, push, push.size())
		rd.compute_list_dispatch(list, ceili(float(resources["threads"]) / 64.0), 1, 1)
		rd.compute_list_end()
	if raster:
		var draw_list := rd.draw_list_begin(resources["framebuffer"], RenderingDevice.DRAW_CLEAR_COLOR_0, PackedColorArray([Color(0, 0, 0, 0)]))
		rd.draw_list_bind_render_pipeline(draw_list, resources["raster_pipeline"])
		rd.draw_list_draw(draw_list, false, resources["layers"], 3)
		rd.draw_list_end()
	rd.capture_timestamp(TIMESTAMP_END)


# Runs warmup + frames frames of one kind, each submitted and waited for. Returns the timed
# frames' GPU and wall times in ms and the async counter deltas over every frame of the phase.
func _phase(rd: RenderingDevice, resources: Dictionary, compute: bool, raster: bool, warmup: int, frames: int) -> Dictionary:
	var gpu := PackedFloat64Array()
	var wall := PackedFloat64Array()
	var before := _counters(rd)
	for i in warmup + frames:
		_record(rd, resources, compute, raster)
		var start := Time.get_ticks_usec()
		rd.submit()
		rd.sync()
		var elapsed_ms := float(Time.get_ticks_usec() - start) / 1000.0
		if i < warmup:
			continue
		wall.append(elapsed_ms)
		var gpu_ms := _timestamp_ms(rd)
		if gpu_ms > 0.0:
			gpu.append(gpu_ms)
	var after := _counters(rd)
	return { "gpu": gpu, "wall": wall, "submitted": after[0] - before[0], "fell_back": after[1] - before[1] }


func _phase_summary(phase: Dictionary) -> Dictionary:
	return {
		"gpu_p50_ms": _percentile(phase["gpu"], 50.0),
		"gpu_p95_ms": _percentile(phase["gpu"], 95.0),
		"gpu_samples": phase["gpu"].size(),
		"wall_p50_ms": _percentile(phase["wall"], 50.0),
		"async_submitted": phase["submitted"],
		"async_fell_back": phase["fell_back"],
	}


# The GPU time between the frame's two timestamps, or 0 when the device has none.
func _timestamp_ms(rd: RenderingDevice) -> float:
	var begin := -1
	var end := -1
	for i in rd.get_captured_timestamps_count():
		var stamp := rd.get_captured_timestamp_name(i)
		if stamp == TIMESTAMP:
			begin = i
		elif stamp == TIMESTAMP_END:
			end = i
	if begin < 0 or end < 0:
		return 0.0
	var delta := rd.get_captured_timestamp_gpu_time(end) - rd.get_captured_timestamp_gpu_time(begin)
	return float(delta) / 1000000.0 if delta > 0 else 0.0


func _check_forks(label: String, phase: Dictionary, submitted: int) -> void:
	_check(phase["submitted"] == submitted, "%s frames forked %d times, expected %d" % [label, phase["submitted"], submitted])
	_check(phase["fell_back"] == 0, "%s frames fell back %d times, expected 0" % [label, phase["fell_back"]])


func _make_input(count: int) -> PackedByteArray:
	var values := PackedInt32Array()
	values.resize(count)
	for i in count:
		values[i] = _hash((i * 2654435761 + SEED) & 0xFFFFFFFF)
	return values.to_byte_array()


# GLSL uint arithmetic on 64-bit GDScript ints. Both multipliers are below 2^31, so every product
# of a 32-bit value fits in 63 bits.
static func _hash(x: int) -> int:
	x ^= x >> 16
	x = (x * 0x7feb352d) & 0xFFFFFFFF
	x ^= x >> 15
	x = (x * 0x2c1b3c6d) & 0xFFFFFFFF
	x ^= x >> 16
	return x


static func expected_element(input_value: int, rounds: int) -> int:
	var value := input_value ^ SALT
	for round in rounds:
		value = _hash((value + round) & 0xFFFFFFFF)
	return value


# The first, a middle and the last element. The full chain is too long to recompute on the CPU for
# every thread.
func _check_compute(data: PackedByteArray, input: PackedByteArray, threads: int, rounds: int) -> bool:
	if data.size() != threads * 4:
		_check(false, "the compute output reads back %d bytes, expected %d" % [data.size(), threads * 4])
		return false
	for i in [0, threads >> 1, threads - 1]:
		var expected := expected_element(input.decode_u32(i * 4), rounds)
		if data.decode_u32(i * 4) != expected:
			_check(false, "compute element %d is %d, expected %d" % [i, data.decode_u32(i * 4), expected])
			return false
	return true


## The blended RGBA of pixel (x, y) after `layers` layers, in units of 1/1024.
static func expected_pixel_units(x: int, y: int, layers: int) -> Array[int]:
	var pattern := x ^ y
	return [layers, layers * (pattern & 1), layers * ((pattern >> 3) & 1), layers >> 1]


# The four corners and seeded samples, against the closed form.
func _check_raster(data: PackedByteArray, size: int, layers: int) -> bool:
	if data.size() != size * size * 8:
		_check(false, "the raster target reads back %d bytes, expected %d" % [data.size(), size * size * 8])
		return false
	var rng := RandomNumberGenerator.new()
	rng.seed = SEED
	var pixels: Array[Vector2i] = [Vector2i(0, 0), Vector2i(size - 1, 0), Vector2i(0, size - 1), Vector2i(size - 1, size - 1)]
	while pixels.size() < SPOT_PIXELS:
		pixels.append(Vector2i(rng.randi_range(0, size - 1), rng.randi_range(0, size - 1)))
	for pixel in pixels:
		var expected := expected_pixel_units(pixel.x, pixel.y, layers)
		var offset := (pixel.y * size + pixel.x) * 8
		for channel in 4:
			var value := data.decode_half(offset + channel * 2) * UNITS_PER_ONE
			if value != float(expected[channel]):
				_check(false, "raster pixel %s channel %d is %s/1024, expected %d/1024" % [pixel, channel, value, expected[channel]])
				return false
	return true


static func _digest(data: PackedByteArray) -> String:
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update(data)
	return context.finish().hex_encode()


# Linear interpolation between closest ranks, as the harness summary does.
static func _percentile(values: PackedFloat64Array, percent: float) -> float:
	if values.is_empty():
		return 0.0
	var sorted := values.duplicate()
	sorted.sort()
	var rank := percent / 100.0 * float(sorted.size() - 1)
	var low := floori(rank)
	var high := mini(low + 1, sorted.size() - 1)
	return lerpf(sorted[low], sorted[high], rank - float(low))


# The device's running "async:<submitted>/<fell back>" frame counters.
func _counters(rd: RenderingDevice) -> Array[int]:
	var report := rd.get_perf_report()
	var at := report.find(" async:")
	if at < 0:
		_check(false, "get_perf_report() has an async: counter")
		return [0, 0]
	var parts := report.substr(at + 7).get_slice(" ", 0).split("/")
	return [int(parts[0]), int(parts[1])]


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


# --- Harness timing mode -----------------------------------------------------------------------

func _harness_begin() -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		print("GODOTNEXT_RESULT " + JSON.stringify({ "test": "async_overlap", "mode": "harness", "reason": "NO_RENDERING_DEVICE" }))
		return
	_harness_queue = "main" if SceneArgs.value("--variant", "") == "main" else "async"
	var params := {
		"queue": _harness_queue,
		"threads": DEFAULT_THREADS,
		"rounds": DEFAULT_ROUNDS,
		"layers": DEFAULT_LAYERS,
		"size": DEFAULT_SIZE,
	}
	_harness_resources = _create(rd, params, _make_input(DEFAULT_THREADS))
	if _harness_resources.is_empty():
		print("GODOTNEXT_RESULT " + JSON.stringify({ "test": "async_overlap", "mode": "harness", "failures": _failures }))
		return
	_harness_device = rd
	_harness_active = bool(ProjectSettings.get_setting("rendering/rendering_device/async_compute", false)) and rd.has_feature(RenderingDevice.SUPPORTS_ASYNC_COMPUTE)
	_harness_counters_before = _counters(rd)


func _harness_frame() -> void:
	_record(_harness_device, _harness_resources, true, true)
	_harness_frames += 1


func _harness_end() -> void:
	var after := _counters(_harness_device)
	# The frame recorded last may not have been submitted when the harness quit.
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "async_overlap",
		"mode": "harness",
		"active": _harness_active,
		"queue": _harness_queue,
		"frames": _harness_frames,
		"submitted": after[0] - _harness_counters_before[0],
		"fell_back": after[1] - _harness_counters_before[1],
	}))
	_free_resources(_harness_device, _harness_resources)
