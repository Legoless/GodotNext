extends Node

## Async compute fork/join check on a local RenderingDevice (#174).
##
## Every case dispatches async_compute_hash.glsl and compares each readback with values the CPU
## recomputes. It also reads the device's "async:<submitted>/<fell back>" frame counters from
## get_perf_report() before and after each case, so a run that never reached the async queue
## cannot pass as one that did:
##
## - fork_join: an async list feeds a readback, next to an independent main list. With async
##   compute active the frame forks and joins once (submitted +1).
## - fallback: main -> async -> main -> async -> readback cannot use one fork and one join, so the
##   frame runs on the main queue (fell back +1).
## - barrier: compute_list_add_barrier() inside an async list, then a readback. Both halves stay on
##   the async queue (submitted +1).
##
## With async compute inactive (the project setting off, or a device without
## SUPPORTS_ASYNC_COMPUTE) every counter must stay 0 and every value must still match.
##
## Run through run_async_compute_check.sh, which sets rendering/rendering_device/async_compute.
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const COUNT := 4096
const ROUNDS := 64
const SHADER_PATH := "res://tests/async_compute/async_compute_hash.glsl"

var _failures := PackedStringArray()


func _ready() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		return { "test": "async_compute", "pass": false, "reason": "NO_RENDERING_DEVICE" }

	var setting := bool(ProjectSettings.get_setting("rendering/rendering_device/async_compute", false))
	var feature := rd.has_feature(RenderingDevice.SUPPORTS_ASYNC_COMPUTE)
	var active := setting and feature
	var shader := _load_shader(rd)
	var cases := {}
	if shader.is_valid():
		var pipeline := rd.compute_pipeline_create(shader)
		_check(pipeline.is_valid(), "compute pipeline is created")
		if pipeline.is_valid():
			cases["fork_join"] = _case_fork_join(rd, shader, pipeline, active)
			cases["fallback"] = _case_fallback(rd, shader, pipeline, active)
			cases["barrier"] = _case_barrier(rd, shader, pipeline, active)
			rd.free_rid(pipeline)
		rd.free_rid(shader)
	var counters := _counters(rd)
	rd.free()
	return {
		"test": "async_compute",
		"setting": setting,
		"feature": feature,
		"active": active,
		"cases": cases,
		"counters": counters,
		"pass": _failures.is_empty(),
		"failures": _failures,
	}


func _load_shader(rd: RenderingDevice) -> RID:
	var file := load(SHADER_PATH) as RDShaderFile
	_check(file != null, "shader file loads")
	if file == null:
		return RID()
	var spirv := file.get_spirv()
	_check(spirv != null and spirv.compile_error_compute.is_empty(), "kernel compiles")
	if spirv == null or not spirv.compile_error_compute.is_empty():
		return RID()
	var shader := rd.shader_create_from_spirv(spirv, "async_compute_hash")
	_check(shader.is_valid(), "shader is created")
	return shader


# main -> async -> readback, plus an independent main list.
func _case_fork_join(rd: RenderingDevice, shader: RID, pipeline: RID, active: bool) -> Dictionary:
	var input := _make_input(1)
	var source := rd.storage_buffer_create(input.size(), input)
	var async_out := rd.storage_buffer_create(input.size())
	var main_out := rd.storage_buffer_create(input.size())
	var async_set := _uniform_set(rd, shader, source, async_out)
	var main_set := _uniform_set(rd, shader, source, main_out)

	var before := _counters(rd)
	_dispatch(rd, pipeline, async_set, 11, RenderingDevice.COMPUTE_QUEUE_ASYNC)
	_dispatch(rd, pipeline, main_set, 12, RenderingDevice.COMPUTE_QUEUE_MAIN)
	# The readback ends the frame that holds both lists, so it is the async list's main consumer.
	var async_data := rd.buffer_get_data(async_out)
	var after := _counters(rd)
	var main_data := rd.buffer_get_data(main_out)

	var async_ok := _matches(async_data, _expected(input, [11]), "fork_join async list")
	var main_ok := _matches(main_data, _expected(input, [12]), "fork_join main list")
	var delta := _delta(before, after)
	_check_delta("fork_join", delta, 1 if active else 0, 0)
	_free_all(rd, [async_set, main_set, source, async_out, main_out])
	return { "async_values": async_ok, "main_values": main_ok, "submitted": delta[0], "fell_back": delta[1] }


# main -> async -> main -> async -> readback: the producer and the consumer levels overlap.
func _case_fallback(rd: RenderingDevice, shader: RID, pipeline: RID, active: bool) -> Dictionary:
	var input := _make_input(2)
	var buffers: Array[RID] = [rd.storage_buffer_create(input.size(), input)]
	for i in 4:
		buffers.append(rd.storage_buffer_create(input.size()))
	var sets: Array[RID] = []
	for i in 4:
		sets.append(_uniform_set(rd, shader, buffers[i], buffers[i + 1]))

	var before := _counters(rd)
	_dispatch(rd, pipeline, sets[0], 21, RenderingDevice.COMPUTE_QUEUE_MAIN)
	_dispatch(rd, pipeline, sets[1], 22, RenderingDevice.COMPUTE_QUEUE_ASYNC)
	_dispatch(rd, pipeline, sets[2], 23, RenderingDevice.COMPUTE_QUEUE_MAIN)
	_dispatch(rd, pipeline, sets[3], 24, RenderingDevice.COMPUTE_QUEUE_ASYNC)
	var data := rd.buffer_get_data(buffers[4])
	var after := _counters(rd)

	var values_ok := _matches(data, _expected(input, [21, 22, 23, 24]), "fallback chain")
	var delta := _delta(before, after)
	_check_delta("fallback", delta, 0, 1 if active else 0)
	var to_free: Array[RID] = []
	to_free.append_array(sets)
	to_free.append_array(buffers)
	_free_all(rd, to_free)
	return { "values": values_ok, "submitted": delta[0], "fell_back": delta[1] }


# An async list with a barrier between its two dispatches, then a readback.
func _case_barrier(rd: RenderingDevice, shader: RID, pipeline: RID, active: bool) -> Dictionary:
	var input := _make_input(3)
	var source := rd.storage_buffer_create(input.size(), input)
	var middle := rd.storage_buffer_create(input.size())
	var result := rd.storage_buffer_create(input.size())
	var first_set := _uniform_set(rd, shader, source, middle)
	var second_set := _uniform_set(rd, shader, middle, result)

	var before := _counters(rd)
	var list := rd.compute_list_begin(RenderingDevice.COMPUTE_QUEUE_ASYNC)
	rd.compute_list_bind_compute_pipeline(list, pipeline)
	rd.compute_list_bind_uniform_set(list, first_set, 0)
	_push(rd, list, 31)
	rd.compute_list_dispatch(list, ceili(COUNT / 64.0), 1, 1)
	rd.compute_list_add_barrier(list)
	rd.compute_list_bind_uniform_set(list, second_set, 0)
	_push(rd, list, 32)
	rd.compute_list_dispatch(list, ceili(COUNT / 64.0), 1, 1)
	rd.compute_list_end()
	var data := rd.buffer_get_data(result)
	var after := _counters(rd)

	var values_ok := _matches(data, _expected(input, [31, 32]), "barrier inside an async list")
	var delta := _delta(before, after)
	_check_delta("barrier", delta, 1 if active else 0, 0)
	_free_all(rd, [first_set, second_set, source, middle, result])
	return { "values": values_ok, "submitted": delta[0], "fell_back": delta[1] }


func _uniform_set(rd: RenderingDevice, shader: RID, source: RID, destination: RID) -> RID:
	var uniforms: Array[RDUniform] = []
	var bindings: Array[RID] = [source, destination]
	for i in 2:
		var uniform := RDUniform.new()
		uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		uniform.binding = i
		uniform.add_id(bindings[i])
		uniforms.append(uniform)
	var uniform_set := rd.uniform_set_create(uniforms, shader, 0)
	_check(uniform_set.is_valid(), "uniform set is created")
	return uniform_set


func _push(rd: RenderingDevice, list: int, salt: int) -> void:
	var push := PackedInt32Array([COUNT, ROUNDS, salt, 0]).to_byte_array()
	rd.compute_list_set_push_constant(list, push, push.size())


func _dispatch(rd: RenderingDevice, pipeline: RID, uniform_set: RID, salt: int, queue: RenderingDevice.ComputeQueue) -> void:
	var list := rd.compute_list_begin(queue)
	rd.compute_list_bind_compute_pipeline(list, pipeline)
	rd.compute_list_bind_uniform_set(list, uniform_set, 0)
	_push(rd, list, salt)
	rd.compute_list_dispatch(list, ceili(COUNT / 64.0), 1, 1)
	rd.compute_list_end()


func _make_input(seed_value: int) -> PackedByteArray:
	var values := PackedInt32Array()
	values.resize(COUNT)
	for i in COUNT:
		values[i] = _hash((i * 2654435761 + seed_value) & 0xFFFFFFFF)
	return values.to_byte_array()


# GLSL uint arithmetic on 64-bit GDScript ints. Both multipliers are below 2^31, so every product
# of a 32-bit value fits in 63 bits.
func _hash(x: int) -> int:
	x ^= x >> 16
	x = (x * 0x7feb352d) & 0xFFFFFFFF
	x ^= x >> 15
	x = (x * 0x2c1b3c6d) & 0xFFFFFFFF
	x ^= x >> 16
	return x


func _expected(input: PackedByteArray, salts: Array) -> PackedInt64Array:
	var out := PackedInt64Array()
	out.resize(COUNT)
	for i in COUNT:
		var value := input.decode_u32(i * 4)
		for salt: int in salts:
			value ^= salt
			for round in ROUNDS:
				value = _hash((value + round) & 0xFFFFFFFF)
		out[i] = value
	return out


func _matches(data: PackedByteArray, expected: PackedInt64Array, label: String) -> bool:
	if data.size() != COUNT * 4:
		_check(false, "%s reads back %d bytes" % [label, COUNT * 4])
		return false
	for i in COUNT:
		if data.decode_u32(i * 4) != expected[i]:
			_check(false, "%s value %d is %d, expected %d" % [label, i, data.decode_u32(i * 4), expected[i]])
			return false
	return true


# The device's running "async:<submitted>/<fell back>" frame counters.
func _counters(rd: RenderingDevice) -> Array[int]:
	var report := rd.get_perf_report()
	var at := report.find(" async:")
	if at < 0:
		_check(false, "get_perf_report() has an async: counter")
		return [0, 0]
	var parts := report.substr(at + 7).get_slice(" ", 0).split("/")
	return [int(parts[0]), int(parts[1])]


func _delta(before: Array[int], after: Array[int]) -> Array[int]:
	return [after[0] - before[0], after[1] - before[1]]


func _check_delta(label: String, delta: Array[int], submitted: int, fell_back: int) -> void:
	_check(delta[0] == submitted, "%s submitted %d async frames, expected %d" % [label, delta[0], submitted])
	_check(delta[1] == fell_back, "%s fell back in %d frames, expected %d" % [label, delta[1], fell_back])


func _free_all(rd: RenderingDevice, rids: Array) -> void:
	for rid: RID in rids:
		if rid.is_valid():
			rd.free_rid(rid)


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)
