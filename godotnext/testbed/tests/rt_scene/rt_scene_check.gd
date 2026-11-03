extends Node

## NextGI RT scene v0 check (#28; plan D13 and §8.2).
##
## Loads a testbed scene, turns NextGI on in its Environment at runtime, renders a few frames and
## reads the RT scene counters back through RenderingServer.nextgi_get_tier_info(). Run it through
## the tier script so the project setting is on for the run:
##   godotnext/testbed/tests/tier/run_tier_check.sh -- <godot> --path godotnext/testbed \
##     --rendering-driver metal res://tests/rt_scene/rt_scene_check.tscn -- \
##     --scene=res://scenes/color_box_room/cbr_a.tscn
## With --nextgi=off it only reports the buffer memory baseline.
##
## On a HWRT device it requires: at least one TLAS instance, BLAS builds on the first frames and
## none after that on the static scene, and one TLAS rebuild per rendered frame. It prints one
## "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.

const FRAMES := 6

var _failures := PackedStringArray()
var _max_blas_build_count := 0


func _ready() -> void:
	var result := await run(OS.get_cmdline_user_args())
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _check(condition: bool, what: String) -> void:
	if not condition:
		_failures.append(what)


func run(args: PackedStringArray) -> Dictionary:
	var scene_path := "res://scenes/color_box_room/cbr_a.tscn"
	for arg in args:
		if arg.begins_with("--scene="):
			scene_path = arg.trim_prefix("--scene=")

	var enabled: bool = ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false)
	var buffer_mem_before := int(RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_BUFFER_MEM_USED))

	var packed: PackedScene = load(scene_path)
	_check(packed != null, "scene loads: " + scene_path)
	if packed == null:
		return _result(enabled, buffer_mem_before, 0, {})

	var scene_root := packed.instantiate()
	add_child(scene_root)
	_enable_nextgi(scene_root)

	var last_info: Dictionary = {}
	for frame in FRAMES:
		await get_tree().process_frame
		var info: Dictionary = RenderingServer.nextgi_get_tier_info()
		_max_blas_build_count = maxi(_max_blas_build_count, int(info.get("rt_blas_build_count", 0)))
		last_info = info

	var buffer_mem_after := int(RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_BUFFER_MEM_USED))

	if not enabled:
		_check(not last_info.has("rt_instance_count"), "nextgi/enabled=false creates no RT scene")
		return _result(enabled, buffer_mem_before, buffer_mem_after, last_info)

	if last_info.get("tier", "") != "HWRT":
		# A device without ray query builds no chunk and no BLAS; that is the expected branch there.
		_check(not last_info.has("rt_instance_count"), "outside the HWRT tier there is no RT scene")
		return _result(enabled, buffer_mem_before, buffer_mem_after, last_info)

	_check(int(last_info.get("rt_fed_instance_count", 0)) > 0, "the v0 feed saw instances")
	_check(int(last_info.get("rt_instance_count", 0)) > 0, "the TLAS holds instances")
	_check(_max_blas_build_count > 0, "BLASes built on the first frames")
	_check(int(last_info.get("rt_blas_build_count", -1)) == 0, "a static scene builds no BLAS after frame 1")
	_check(int(last_info.get("rt_tlas_build_count", 0)) >= FRAMES - 1, "the TLAS rebuilds every frame")
	return _result(enabled, buffer_mem_before, buffer_mem_after, last_info)


func _enable_nextgi(node: Node) -> void:
	if node is WorldEnvironment:
		var world_environment := node as WorldEnvironment
		if world_environment.environment != null:
			world_environment.environment.nextgi_enabled = true
	for child in node.get_children():
		_enable_nextgi(child)


func _result(enabled: bool, buffer_mem_before: int, buffer_mem_after: int, info: Dictionary) -> Dictionary:
	return {
		"pass": _failures.is_empty(),
		"device": RenderingServer.get_video_adapter_name(),
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"nextgi_enabled": enabled,
		"buffer_mem_before": buffer_mem_before,
		"buffer_mem_after": buffer_mem_after,
		"max_blas_build_count": _max_blas_build_count,
		"tier_info": info,
		"failures": _failures,
	}
