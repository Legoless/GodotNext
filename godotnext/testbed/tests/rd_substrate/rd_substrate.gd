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
		"no-ray-query":
			_expect(not ray_query, "SUPPORTS_RAY_QUERY is true")
			_expect(not hardware, "SUPPORTS_RAY_QUERY_HARDWARE is true")
			_expect_reason(reason, reason_name if not reason_name.is_empty() else "RAY_QUERY_REASON_NO_RAY_QUERY")
			for name in LIMIT_NAMES:
				_expect(int(values[name]) == 0, "%s limit is %d, expected 0" % [name, values[name]])
			_expect(not vertex_float3, "a device without ray query reports a BLAS vertex format")
		_:
			_failures.append("--expect must be ray-query-hardware or no-ray-query")
	_finish(result)


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
