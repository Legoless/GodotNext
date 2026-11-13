extends Node

## The ray-query substrate each driver reports (#22): SUPPORTS_RAY_QUERY,
## SUPPORTS_RAY_QUERY_HARDWARE, get_ray_query_unsupported_reason(), the five
## acceleration-structure limits and the BLAS vertex-format query.
##
## After "--":
##   --expect=ray-query-hardware   ray query on an RT-core device (the RTX 4080 SUPER on Vulkan,
##                                 the M5 Max on Metal)
##   --expect=no-ray-query         no ray query (D3D12 until DXR, #118): both flags false and every
##                                 AS limit 0
##   --limits=I,G,P,S,A            the expected instance, geometry, primitive, per-stage and scratch
##                                 alignment limits (with ray-query-hardware; for Vulkan, the
##                                 device's vulkaninfo values)
##   --reason=NAME                 the expected reason (default RAY_QUERY_SUPPORTED with ray query,
##                                 RAY_QUERY_REASON_NO_RAY_QUERY without)
##
## With ray query it also builds 1000 BLAS over 100 frames on a local RenderingDevice: the
## MEMORY_ACCELERATION_STRUCTURES delta must equal the sum of their sizes exactly, and the counter
## must return to its baseline once they are freed. Without ray query the counter must stay 0.
##
##   godot --path godotnext/testbed --rendering-driver vulkan --gpu-index 0 --gpu-validation \
##     res://tests/rd_substrate/rd_substrate.tscn -- --expect=ray-query-hardware \
##     --limits=16777215,16777215,536870911,1048576,128
##
## Prints one "GODOTNEXT_RESULT <json>" line and quits with 0 on pass, 1 on failure.

const AS_LIMITS := [
	RenderingDevice.LIMIT_MAX_ACCELERATION_STRUCTURE_INSTANCES,
	RenderingDevice.LIMIT_MAX_ACCELERATION_STRUCTURE_GEOMETRIES,
	RenderingDevice.LIMIT_MAX_ACCELERATION_STRUCTURE_PRIMITIVES,
	RenderingDevice.LIMIT_MAX_PER_STAGE_ACCELERATION_STRUCTURES,
	RenderingDevice.LIMIT_MIN_ACCELERATION_STRUCTURE_SCRATCH_OFFSET_ALIGNMENT,
]
const LIMIT_NAMES := ["instances", "geometries", "primitives", "per_stage", "scratch_alignment"]
const REASON_NAMES := {
	RenderingDevice.RAY_QUERY_SUPPORTED: "RAY_QUERY_SUPPORTED",
	RenderingDevice.RAY_QUERY_REASON_NO_RAY_QUERY: "RAY_QUERY_REASON_NO_RAY_QUERY",
	RenderingDevice.RAY_QUERY_REASON_D3D12_NO_DXR: "RAY_QUERY_REASON_D3D12_NO_DXR",
	RenderingDevice.RAY_QUERY_REASON_METAL_SYNC_MODE: "RAY_QUERY_REASON_METAL_SYNC_MODE",
	RenderingDevice.RAY_QUERY_REASON_METAL_NO_ARGUMENT_BUFFERS: "RAY_QUERY_REASON_METAL_NO_ARGUMENT_BUFFERS",
}
## The plan's floor for the TLAS instance count (§9).
const MIN_INSTANCES := 100000
const ACCOUNTING_FRAMES := 100
const ACCOUNTING_PER_FRAME := 10
## Frames to let deferred frees run on the local device.
const DISPOSE_FRAMES := 4

var _failures := PackedStringArray()


func _ready() -> void:
	var expect := ""
	var limits := PackedInt64Array()
	var reason_name := ""
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--expect="):
			expect = arg.trim_prefix("--expect=")
		elif arg.begins_with("--limits="):
			for part in arg.trim_prefix("--limits=").split(","):
				limits.append(int(part))
		elif arg.begins_with("--reason="):
			reason_name = arg.trim_prefix("--reason=")
	var rd := RenderingServer.get_rendering_device()
	var result := {"test": "rd_substrate", "expect": expect}
	if rd == null:
		_failures.append("no RenderingDevice")
		_finish(result)
		return
	var ray_query := rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY)
	var hardware := rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY_HARDWARE)
	var reason := rd.get_ray_query_unsupported_reason()
	var values := {}
	for i in AS_LIMITS.size():
		values[LIMIT_NAMES[i]] = rd.limit_get(AS_LIMITS[i])
	var vertex_float3 := rd.acceleration_structure_is_vertex_format_supported(RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT)
	result["driver"] = RenderingServer.get_current_rendering_driver_name()
	result["adapter"] = RenderingServer.get_video_adapter_name()
	result["ray_query"] = ray_query
	result["ray_query_hardware"] = hardware
	result["reason"] = REASON_NAMES.get(reason, str(reason))
	result["limits"] = values
	result["vertex_r32g32b32_sfloat"] = vertex_float3

	match expect:
		"ray-query-hardware":
			_expect(ray_query, "SUPPORTS_RAY_QUERY is false")
			_expect(hardware, "SUPPORTS_RAY_QUERY_HARDWARE is false")
			_expect_reason(reason, reason_name if not reason_name.is_empty() else "RAY_QUERY_SUPPORTED")
			_expect(int(values["instances"]) >= MIN_INSTANCES, "the instance limit %d is below %d" % [values["instances"], MIN_INSTANCES])
			if limits.size() == AS_LIMITS.size():
				for i in AS_LIMITS.size():
					_expect(int(values[LIMIT_NAMES[i]]) == limits[i], "%s limit is %d, expected %d" % [LIMIT_NAMES[i], values[LIMIT_NAMES[i]], limits[i]])
			elif limits.size() != 0:
				_failures.append("--limits needs %d values" % AS_LIMITS.size())
			_expect(vertex_float3, "R32G32B32_SFLOAT is not a supported BLAS vertex format")
			result["accounting"] = _check_accounting()
		"no-ray-query":
			_expect(not ray_query, "SUPPORTS_RAY_QUERY is true")
			_expect(not hardware, "SUPPORTS_RAY_QUERY_HARDWARE is true")
			_expect_reason(reason, reason_name if not reason_name.is_empty() else "RAY_QUERY_REASON_NO_RAY_QUERY")
			for name in LIMIT_NAMES:
				_expect(int(values[name]) == 0, "%s limit is %d, expected 0" % [name, values[name]])
			_expect(not vertex_float3, "a device without ray query reports a BLAS vertex format")
			var memory := rd.get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES)
			result["acceleration_structure_memory"] = memory
			_expect(memory == 0, "MEMORY_ACCELERATION_STRUCTURES is %d without ray query" % memory)
		_:
			_failures.append("--expect must be ray-query-hardware or no-ray-query")
	_finish(result)


## 1000 BLAS over 100 frames on a local device; returns what it measured.
func _check_accounting() -> Dictionary:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		_failures.append("no local RenderingDevice for the accounting check")
		return {}
	var vertices := PackedFloat32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]).to_byte_array()
	var input_bits := RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT
	var vertex_buffer := rd.vertex_buffer_create(vertices.size(), vertices, input_bits)
	var geometry := RDAccelerationStructureGeometry.new()
	geometry.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
	geometry.vertex_buffer = vertex_buffer
	geometry.vertex_stride = 12
	geometry.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	geometry.vertex_count = 3
	_settle(rd)
	var baseline := rd.get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES)
	var blases: Array[RID] = []
	var sizes := 0
	var build_failures := 0
	for frame in ACCOUNTING_FRAMES:
		for _i in ACCOUNTING_PER_FRAME:
			var blas := rd.blas_create([geometry], 0)
			if not blas.is_valid() or rd.blas_build(blas) != OK:
				build_failures += 1
				continue
			blases.append(blas)
			sizes += rd.acceleration_structure_get_size_bytes(blas)
		_settle(rd)
	var delta := rd.get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES) - baseline
	_expect(build_failures == 0, "%d BLAS failed to create or build" % build_failures)
	_expect(blases.size() == ACCOUNTING_FRAMES * ACCOUNTING_PER_FRAME, "built %d BLAS, expected %d" % [blases.size(), ACCOUNTING_FRAMES * ACCOUNTING_PER_FRAME])
	_expect(sizes > 0, "the BLAS sizes add up to 0")
	_expect(delta == sizes, "MEMORY_ACCELERATION_STRUCTURES grew by %d, the BLAS sizes add up to %d" % [delta, sizes])
	for blas in blases:
		rd.free_rid(blas)
	for _i in DISPOSE_FRAMES:
		_settle(rd)
	var after := rd.get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES)
	_expect(after == baseline, "MEMORY_ACCELERATION_STRUCTURES is %d after freeing, the baseline was %d" % [after, baseline])
	rd.free_rid(vertex_buffer)
	_settle(rd)
	rd.free()
	return {"blas": blases.size(), "bytes": sizes, "delta": delta, "baseline": baseline, "after": after}


func _settle(p_rd: RenderingDevice) -> void:
	p_rd.submit()
	p_rd.sync()


func _expect(p_condition: bool, p_message: String) -> void:
	if not p_condition:
		_failures.append(p_message)


func _expect_reason(p_reason: int, p_expected: String) -> void:
	var name: String = REASON_NAMES.get(p_reason, str(p_reason))
	_expect(name == p_expected, "the reason is %s, expected %s" % [name, p_expected])


func _finish(p_result: Dictionary) -> void:
	p_result["pass"] = _failures.is_empty()
	p_result["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(p_result))
	for failure in _failures:
		push_error("RD_SUBSTRATE_FAIL %s" % failure)
	get_tree().quit(0 if _failures.is_empty() else 1)
