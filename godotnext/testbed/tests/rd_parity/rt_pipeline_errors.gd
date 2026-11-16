extends Node

## RT pipeline failure paths on a GPU (#179; plan §21.1 C5).
##
## Run it on a GPU, with a window:
##   godot --path godotnext/testbed --rendering-driver vulkan res://tests/rd_parity/rt_pipeline_errors.tscn
## It prints one "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.
##
## Each check makes one call that RenderingDevice must refuse with one named error, before the driver works:
## a library given a hit shader binding table or bound to a list, the indirect trace buffer rules, a launch-size
## buffer that the raytracing list also reads from a uniform set or through a bindless table that has not read it
## since it was last written, each link rule, and a library whose payload is over 64 bytes. A logger captures the errors: each check passes only
## when its call fails and prints exactly one error with the expected text, and the run fails if anything
## else prints an error. A library of the 16-byte payload shader and a link of a matching library are the
## controls: they print nothing. Because the errors are printed on purpose, "expected_errors" in the result lists
## them; tests/rd_parity/rt_pipeline.tscn, which runs the pipelines, prints none.

const RTPipeline = preload("res://tests/rd_parity/rt_pipeline.gd")

# A closest-hit shader without uniform sets: a library of it has another layout than the base's.
const SMALL_HIT := """#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT uvec4 payload;

void main() {
	payload = uvec4(uint(gl_PrimitiveID));
}
"""

# A ray generation shader that reads a storage buffer in set 0 and a bindless storage-buffer table in set 1, for
# the indirect traces that get through the trace's set binds. It never runs: the graph refuses those traces.
const LAUNCH_RAYGEN := """#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0, std430) restrict buffer Data {
	uint data[];
}
data_buffer;
layout(set = 1, binding = 0, std430) restrict readonly buffer Table {
	uint data[];
}
table[];

void main() {
	data_buffer.data[1] = table[nonuniformEXT(data_buffer.data[0])].data[0];
}
"""

const LAUNCH_MISS := """#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT uvec4 payload;

void main() {
	payload = uvec4(0u);
}
"""

# The same with an 80-byte payload, over the 64 bytes a library declares.
const LARGE_PAYLOAD_HIT := """#version 460
#extension GL_EXT_ray_tracing : require

struct Payload {
	uvec4 data[5];
};
layout(location = 0) rayPayloadInEXT Payload payload;

void main() {
	payload.data[4] = uvec4(uint(gl_PrimitiveID));
}
"""


class ErrorLog:
	extends Logger

	var _mutex := Mutex.new()
	var _messages := PackedStringArray()

	func _log_error(_function: String, _file: String, _line: int, code: String, rationale: String, _editor_notify: bool, error_type: int, _script_backtraces: Array[ScriptBacktrace]) -> void:
		if error_type == ERROR_TYPE_WARNING:
			return
		_mutex.lock()
		_messages.append(rationale if not rationale.is_empty() else code)
		_mutex.unlock()

	func _log_message(_message: String, _error: bool) -> void:
		pass

	func take() -> PackedStringArray:
		_mutex.lock()
		var messages := _messages
		_messages = PackedStringArray()
		_mutex.unlock()
		return messages


var _log := ErrorLog.new()
# rt_pipeline.gd's helpers, on an instance that is never added to the tree.
var _base: RTPipeline = RTPipeline.new()
var _expected_errors := PackedStringArray()
var _checks := {}


func _ready() -> void:
	OS.add_logger(_log)
	var result := _run()
	OS.remove_logger(_log)
	_base.free()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var result := {
		"test": "rt_pipeline_errors",
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"checks": _checks,
		"expected_errors": _expected_errors,
	}
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		result["pass"] = false
		result["reason"] = "NO_RENDERING_DEVICE"
		return result
	_base._rd = rd
	result["adapter"] = rd.get_device_name()
	var needs := [
		["raytracing_pipeline", RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE, "NO_RAYTRACING_PIPELINE"],
		["bindless", RenderingDevice.SUPPORTS_BINDLESS, "NO_BINDLESS"],
		["libraries", RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE_LIBRARY, "NO_LIBRARIES"],
		["indirect", RenderingDevice.SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT, "NO_INDIRECT"],
	]
	var supported := {}
	for need in needs:
		supported[need[0]] = rd.has_feature(need[1])
		if not supported[need[0]] and not result.has("reason"):
			result["reason"] = need[2]
			_base._check(false, "the device reports " + need[0])
	result["supported"] = supported
	if not result.has("reason"):
		_run_checks(rd)

	for i in range(_base._freed.size() - 1, -1, -1):
		if _base._freed[i].is_valid():
			rd.free_rid(_base._freed[i])
	rd.free()
	# Every error the run printed belongs to a check.
	var stray := _log.take()
	_base._check(stray.is_empty(), "no other errors (%d: %s)" % [stray.size(), "; ".join(stray)])
	result["pass"] = _base._failures.is_empty()
	result["failures"] = _base._failures
	return result


func _run_checks(rd: RenderingDevice) -> void:
	var common := RTPipeline.COMMON % ""
	var raygen := _base._create_shader(rd, { "raygen": common + RTPipeline.RAYGEN, "miss": common + RTPipeline.MISS_SHADER }, "rt raygen")
	var hit := _base._create_shader(rd, { "any_hit": common + RTPipeline.RECORD + RTPipeline.ANY_HIT, "closest_hit": common + RTPipeline.RECORD + RTPipeline.CLOSEST_HIT }, "rt hit")
	var small_hit := _base._create_shader(rd, { "closest_hit": SMALL_HIT }, "rt small hit")
	var large_hit := _base._create_shader(rd, { "closest_hit": LARGE_PAYLOAD_HIT }, "rt large payload hit")
	var launch_shader := _base._create_shader(rd, { "raygen": LAUNCH_RAYGEN, "miss": LAUNCH_MISS }, "rt launch raygen")
	if not (raygen.is_valid() and hit.is_valid() and small_hit.is_valid() and large_hit.is_valid() and launch_shader.is_valid()):
		return

	var library := RenderingDevice.RAYTRACING_PIPELINE_FLAG_LIBRARY
	var raygen_shader: RDPipelineShader = _base._pipeline_shader(raygen)
	var hit_group: RDHitGroup = _base._hit_group(hit, 0, 0, 128)
	var direct := _keep(rd.raytracing_pipeline_create([raygen_shader], [raygen_shader], [hit_group], 1))
	var base_library := _keep(rd.raytracing_pipeline_create([raygen_shader], [raygen_shader], [], 1, library))
	var other_base := _keep(rd.raytracing_pipeline_create([raygen_shader], [raygen_shader], [], 1, library))
	var hit_library := _keep(rd.raytracing_pipeline_create([], [], [hit_group], 1, library))
	var deep_library := _keep(rd.raytracing_pipeline_create([], [], [hit_group], 2, library))
	var small_group := RDHitGroup.new()
	small_group.closest_hit_shader = _base._pipeline_shader(small_hit)
	var small_library := _keep(rd.raytracing_pipeline_create([], [], [small_group], 1, library))
	var sbt := _keep(rd.hit_sbt_create(direct, 1))
	var plain_buffer := _keep(rd.storage_buffer_create(16))
	var no_address := _keep(rd.storage_buffer_create(16, PackedByteArray(), RenderingDevice.STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT))
	var launch := _keep(rd.storage_buffer_create(16, PackedByteArray(), RenderingDevice.STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT, RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT))
	# The pipeline, sets and tables of the launch-size checks: set 0 holds launch or another buffer, and set 1 a
	# table that holds launch or the other buffer. A table's new resident has not been read through it yet.
	var launch_stage: RDPipelineShader = _base._pipeline_shader(launch_shader)
	var launch_pipeline := _keep(rd.raytracing_pipeline_create([launch_stage], [launch_stage], [small_group], 1))
	var launch_sbt := _keep(rd.hit_sbt_create(launch_pipeline, 1))
	if launch_sbt.is_valid():
		rd.hit_sbt_range_update(launch_sbt, rd.hit_sbt_range_alloc(launch_sbt, 1), 0, PackedInt32Array([0]))
	var other := _keep(rd.storage_buffer_create(16))
	var other_table := _keep(rd.bindless_table_create(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 4))
	var launch_table := _keep(rd.bindless_table_create(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 4))
	var tables_filled := rd.bindless_table_allocate(other_table, other) == 0 and rd.bindless_table_allocate(launch_table, launch) == 0
	var launch_set := _keep(rd.uniform_set_create([RTPipeline._uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 0, launch)], launch_shader, 0))
	var other_set := _keep(rd.uniform_set_create([RTPipeline._uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 0, other)], launch_shader, 0))
	var other_table_set := _keep(_base._table_set(rd, launch_shader, other_table))
	var launch_table_set := _keep(_base._table_set(rd, launch_shader, launch_table))
	var setup := _log.take()
	_base._check(setup.is_empty(), "the pipelines, libraries and buffers the checks use are created without errors (%s)" % "; ".join(setup))
	_base._check(tables_filled, "the launch-size checks' tables each hold their buffer in slot 0")
	for rid in [direct, base_library, other_base, hit_library, deep_library, small_library, sbt, plain_buffer, no_address, launch, launch_pipeline, launch_sbt, other, other_table, launch_table, launch_set, other_set, other_table_set, launch_table_set]:
		if not rid.is_valid():
			_base._check(false, "every pipeline, library and buffer the checks use is created")
			return
	# The control: the base links with a library of its own layout, without an error.
	var control := _keep(rd.raytracing_pipeline_link(base_library, [hit_library]))
	_base._check(control.is_valid() and _log.take().is_empty(), "the base links with a matching library without an error")

	var not_bindable := "cannot be bound or given a hit shader binding table"
	_expect("library_hit_sbt_create", not_bindable, func() -> bool: return not rd.hit_sbt_create(base_library, 1).is_valid())
	_expect("library_hit_sbt_set_pipeline", not_bindable, func() -> bool: return rd.hit_sbt_set_pipeline(sbt, base_library) != OK)

	var list := rd.raytracing_list_begin()
	_expect("library_bind", not_bindable, func() -> bool:
		rd.raytracing_list_bind_raytracing_pipeline(list, base_library)
		return true
	)
	rd.raytracing_list_bind_raytracing_pipeline(list, direct)
	_expect("indirect_usage", "STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT", func() -> bool:
		rd.raytracing_list_trace_rays_indirect(list, 0, sbt, plain_buffer, 0)
		return true
	)
	_expect("indirect_device_address", "BUFFER_CREATION_DEVICE_ADDRESS_BIT", func() -> bool:
		rd.raytracing_list_trace_rays_indirect(list, 0, sbt, no_address, 0)
		return true
	)
	_expect("indirect_misaligned", "multiple of 4", func() -> bool:
		rd.raytracing_list_trace_rays_indirect(list, 0, sbt, launch, 2)
		return true
	)
	_expect("indirect_out_of_range", "inside the buffer", func() -> bool:
		rd.raytracing_list_trace_rays_indirect(list, 0, sbt, launch, 8)
		return true
	)
	rd.raytracing_list_end()

	# A buffer has one usage per raytracing list, so the launch size must not meet another read of the same
	# buffer there. These traces get through the set binds; the render graph refuses them.
	list = rd.raytracing_list_begin()
	rd.raytracing_list_bind_raytracing_pipeline(list, launch_pipeline)
	rd.raytracing_list_bind_uniform_set(list, launch_set, 0)
	rd.raytracing_list_bind_uniform_set(list, other_table_set, 1)
	_expect("indirect_in_uniform_set", "is also used in this raytracing list", func() -> bool:
		rd.raytracing_list_trace_rays_indirect(list, 0, launch_sbt, launch, 0)
		return true
	)
	rd.raytracing_list_end()
	list = rd.raytracing_list_begin()
	rd.raytracing_list_bind_raytracing_pipeline(list, launch_pipeline)
	rd.raytracing_list_bind_uniform_set(list, other_set, 0)
	rd.raytracing_list_bind_uniform_set(list, launch_table_set, 1)
	_expect("indirect_in_dirty_bindless_table", "is in a bindless table that this raytracing list binds", func() -> bool:
		rd.raytracing_list_trace_rays_indirect(list, 0, launch_sbt, launch, 0)
		return true
	)
	rd.raytracing_list_end()

	_expect("link_base_not_library", "The base of a link must be a pipeline created with RAYTRACING_PIPELINE_FLAG_LIBRARY", func() -> bool: return not rd.raytracing_pipeline_link(direct, []).is_valid())
	_expect("link_base_no_raygen", "at least one ray generation shader", func() -> bool: return not rd.raytracing_pipeline_link(hit_library, []).is_valid())
	_expect("link_duplicate", "only once", func() -> bool: return not rd.raytracing_pipeline_link(base_library, [hit_library, hit_library]).is_valid())
	_expect("link_recursion", "max_trace_recursion_depth", func() -> bool: return not rd.raytracing_pipeline_link(base_library, [deep_library]).is_valid())
	_expect("link_raygen_library", "adds hit groups only", func() -> bool: return not rd.raytracing_pipeline_link(base_library, [other_base]).is_valid())
	_expect("link_layout", "immutable samplers and push constant size", func() -> bool: return not rd.raytracing_pipeline_link(base_library, [small_library]).is_valid())

	var large_group := RDHitGroup.new()
	large_group.closest_hit_shader = _base._pipeline_shader(large_hit)
	_expect("library_payload", "80-byte ray payload in its closest-hit stage", func() -> bool: return not rd.raytracing_pipeline_create([], [], [large_group], 1, library).is_valid())


# Runs p_call, which returns true when its call failed as it should, and checks that it printed exactly one
# error containing p_fragment.
func _expect(p_name: String, p_fragment: String, p_call: Callable) -> void:
	_log.take()
	var failed: bool = p_call.call()
	var messages := _log.take()
	var ok := failed and messages.size() == 1 and messages[0].contains(p_fragment)
	_checks[p_name] = { "pass": ok, "errors": messages }
	_expected_errors.append(p_name + ": " + p_fragment)
	_base._check(ok, "%s: one error containing \"%s\" (the call failed: %s; errors: %s)" % [p_name, p_fragment, failed, "; ".join(messages)])


func _keep(p_rid: RID) -> RID:
	if p_rid.is_valid():
		_base._freed.append(p_rid)
	return p_rid
