extends Node

## NextGI tier selection check (#15; plan §3.1 and gate G0-2).
##
## The tier is chosen once, at RenderingDevice init, from rendering/global_illumination/nextgi/*.
## The testbed keeps NextGI off, so run_tier_check.sh writes res://override.cfg (git ignores it)
## for the run and removes it afterwards:
##   godotnext/testbed/tests/tier/run_tier_check.sh [--nextgi=on|off] [--hwrt=auto|off|force] -- \
##     <godot> --path godotnext/testbed --rendering-driver <driver> \
##     res://tests/tier/tier_check.tscn -- --expect=<NAME>
## On Windows, write the same file by hand before the run:
##   [rendering]
##   global_illumination/nextgi/enabled=true
##
## --expect=<NAME> names a tier (HWRT, SWRT, DISABLED, UNSUPPORTED_RENDERER) or a tier reason
## (NO_RAY_QUERY, D3D12_NO_DXR, CPU_DEVICE, ...). Outside the HWRT tier the check also requires
## ray_query_shaders_initialized == 0. Under Auto quality on an active tier, the quality reason
## must name that tier's benchmark (#259: the software tier times the WRC update) and not a
## failed one; --expect-quality=<QUALITY> (HIGH, MEDIUM, ...) also checks the pick. It prints one
## "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.

const TIERS: Array[String] = ["DISABLED", "HWRT", "SWRT", "UNSUPPORTED_RENDERER"]
const AUTO_BENCHMARKS := {"HWRT": "auto: trace benchmark ", "SWRT": "auto: wrc benchmark "}


func _ready() -> void:
	var result := run(OS.get_cmdline_user_args())
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func run(args: PackedStringArray) -> Dictionary:
	var expect := ""
	var expect_quality := ""
	for arg in args:
		if arg.begins_with("--expect="):
			expect = arg.trim_prefix("--expect=")
		elif arg.begins_with("--expect-quality="):
			expect_quality = arg.trim_prefix("--expect-quality=")

	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var failures := PackedStringArray()
	if expect.is_empty():
		failures.append("--expect=<tier or reason> is required")
	elif expect in TIERS:
		if info["tier"] != expect:
			failures.append("tier is %s, expected %s" % [info["tier"], expect])
	elif info["reason"] != expect:
		failures.append("reason is %s, expected %s" % [info["reason"], expect])

	if info["tier"] != "HWRT" and int(info["ray_query_shaders_initialized"]) != 0:
		failures.append("ray-query ShaderRDs were initialized outside the HWRT tier")

	var status: Dictionary = RenderingServer.nextgi_get_status()
	var quality_reason := str(status.get("quality_reason", ""))
	if AUTO_BENCHMARKS.has(info["tier"]) and quality_reason != "setting":
		var prefix: String = AUTO_BENCHMARKS[info["tier"]]
		if not quality_reason.begins_with(prefix) or quality_reason.begins_with(prefix + "failed"):
			failures.append("auto quality reason is \"%s\", expected a passed \"%s...\"" % [quality_reason, prefix])
	if not expect_quality.is_empty() and info["quality"] != expect_quality:
		failures.append("quality is %s, expected %s" % [info["quality"], expect_quality])

	return {
		"pass": failures.is_empty(),
		"expect": expect,
		"expect_quality": expect_quality,
		"quality_reason": quality_reason,
		"device": RenderingServer.get_video_adapter_name(),
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"nextgi_enabled": ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false),
		"tier_info": info,
		"failures": failures,
	}
