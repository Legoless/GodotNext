extends SceneTree

## Headless check of the AS calibration fallback (#9) with mocked device
## capabilities: a device without ray query reports its reason and passes,
## and a device with ray query gets no fallback result.
##
## Usage: <godot> --headless --path godotnext/testbed --script res://tests/as_calibration/as_calibration_fallback_check.gd

const AsCalibration := preload("res://tests/as_calibration/as_calibration.gd")


func _initialize() -> void:
	var failures := PackedStringArray()
	var cases := {
		RenderingDevice.RAY_QUERY_REASON_NO_RAY_QUERY: "NO_RAY_QUERY",
		RenderingDevice.RAY_QUERY_REASON_D3D12_NO_DXR: "D3D12_NO_DXR",
		RenderingDevice.RAY_QUERY_REASON_METAL_SYNC_MODE: "METAL_SYNC_MODE",
		RenderingDevice.RAY_QUERY_REASON_METAL_NO_ARGUMENT_BUFFERS: "METAL_NO_ARGUMENT_BUFFERS",
	}
	for reason in cases:
		var result: Dictionary = AsCalibration.fallback_result(false, reason, "mock", "mock device")
		if not bool(result.get("fallback", false)) or not bool(result.get("pass", false)) or result.get("reason") != cases[reason]:
			failures.append("fallback for %s: %s" % [cases[reason], JSON.stringify(result)])
	var supported: Dictionary = AsCalibration.fallback_result(true, RenderingDevice.RAY_QUERY_SUPPORTED, "mock", "mock device")
	if not supported.is_empty():
		failures.append("a device with ray query got a fallback result")
	var contradictory: Dictionary = AsCalibration.fallback_result(false, RenderingDevice.RAY_QUERY_SUPPORTED, "mock", "mock device")
	if bool(contradictory.get("pass", true)):
		failures.append("no ray query with reason SUPPORTED must fail")
	print("GODOTNEXT_RESULT " + JSON.stringify({ "test": "as_calibration_fallback", "pass": failures.is_empty(), "failures": failures }))
	quit(0 if failures.is_empty() else 1)
