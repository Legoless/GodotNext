extends SceneTree
## Structural check for Slow IO Flight (#74).
##
## The spline, the 60 s frame count, the wall-time delay record and the
## Dense City working-set ratio are checked here. Page-upload samples and
## the Q12 percentiles stay null until a timing run writes them. This script
## does not sleep and does not load the Dense City scene.
##
## [code]godot --headless --path godotnext/testbed --script res://scenes/slow_io_flight/check_slow_io.gd[/code]

const Flight := preload("res://scenes/slow_io_flight/flight.gd")
const Scaffold := preload("res://generators/scaffold.gd")

const SCENE_PATH := "res://scenes/slow_io_flight/slow_io_flight.tscn"

var _failures: Array[String] = []
var _checks := 0


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var contract := Flight.contract_error()
	if not contract.is_empty():
		_fail(contract)
	var packed := load(SCENE_PATH) as PackedScene
	if packed == null:
		printerr("check_slow_io FAIL: cannot load " + SCENE_PATH)
		quit(1)
		return
	var root := packed.instantiate()
	get_root().add_child(root)
	_check_scene(root)
	_check_sources()
	_check_working_set()
	root.free()
	if _failures.is_empty():
		print("check_slow_io: all %d assertions passed." % _checks)
		quit(0)
		return
	for failure in _failures:
		printerr("check_slow_io FAIL: " + failure)
	quit(1)


func _check_scene(p_root: Node) -> void:
	_ok(int(p_root.get_meta("seed", 0)) == Scaffold.stream_seed(75), "scene seed must be stream 75.")
	_ok(String(p_root.get_meta("variant", "")) == "delayed", "default variant is delayed.")
	var camera := p_root.get_node_or_null("FlightCam") as Camera3D
	_ok(camera != null and camera.current, "FlightCam must be the current camera.")
	if camera != null:
		var expected := Flight.camera_transform(0)
		_ok(camera.global_position.distance_to(expected.origin) < 0.05, "FlightCam starts on frame 0.")
	var path := p_root.get_node_or_null("FlightPath") as Path3D
	_ok(path != null and path.curve != null and path.curve.point_count == Flight.CONTROL_POINTS.size(),
			"FlightPath must carry the 8 Dense City control points.")
	if path != null and path.curve != null and path.curve.point_count == Flight.CONTROL_POINTS.size():
		_ok(path.curve.get_point_position(0).distance_to(Flight.CONTROL_POINTS[0]) < 1e-3, "first control point.")
		var last := Flight.CONTROL_POINTS.size() - 1
		_ok(path.curve.get_point_position(last).distance_to(Flight.CONTROL_POINTS[last]) < 1e-3, "last control point.")
	var stand_in := p_root.get_node_or_null("StandIn")
	_ok(stand_in != null and stand_in.get_child_count() == Flight.CONTROL_POINTS.size(),
			"stand-in blocks stand in for Dense City during capture.")
	_ok(p_root.get_node_or_null("DenseCity") == null, "the saved scene must not instance Dense City.")
	var scene_text := FileAccess.get_file_as_string(SCENE_PATH)
	_ok(not scene_text.contains("dense_city.tscn"), "the saved scene must not reference the city scene.")


func _check_sources() -> void:
	var flight_text := FileAccess.get_file_as_string("res://scenes/slow_io_flight/flight.gd")
	var runtime_text := FileAccess.get_file_as_string("res://scenes/slow_io_flight/slow_io_flight.gd")
	_ok(not flight_text.contains("OS.delay_msec") and not runtime_text.contains("OS.delay_msec"),
			"IO delay is recorded as wall time and is not slept in this scene.")
	var delayed: Dictionary = Flight.delay_record("delayed")
	var nodelay: Dictionary = Flight.delay_record("nodelay")
	_ok(String(delayed["applied_in"]) == "wall_time" and String(nodelay["applied_in"]) == "wall_time",
			"both variants record applied_in wall_time.")
	_ok(int(delayed["min_ms"]) == 50 and int(delayed["max_ms"]) == 250, "delayed range is 50-250 ms.")
	_ok(int(nodelay["min_ms"]) == 0 and int(nodelay["max_ms"]) == 0, "nodelay range is 0 ms.")
	_ok(Flight.FLIGHT_FRAMES == 3600 and Flight.FLIGHT_FPS == 60, "flight is 3600 frames at 60 fps.")


func _check_working_set() -> void:
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(Flight.EXPECTED_JSON))
	if not _ok(parsed is Dictionary, "expected.json must load."):
		return
	var body: Dictionary = parsed
	var variants: Dictionary = body.get("variants", {})
	var virtual: Dictionary = variants.get("virtual", {})
	var unique_bytes := float(virtual.get("unique_bytes", 0))
	var budget := float(body.get("geometry_cache_budget_bytes", 0))
	if not _ok(budget > 0.0 and unique_bytes > 0.0, "virtual unique bytes and the cache budget must be set."):
		return
	var ratio := unique_bytes / budget
	_ok(ratio >= 2.0, "working set must be at least 2x geometry_cache_mb (%.3f)." % ratio)
	_ok(is_equal_approx(ratio, float(virtual.get("working_set_ratio", 0.0))),
			"working_set_ratio must match unique_bytes / budget.")
	_ok(int(budget) == Flight.GEOMETRY_CACHE_MB * 1024 * 1024, "budget must be 128 MiB.")


func _ok(p_condition: bool, p_message: String) -> bool:
	_checks += 1
	if not p_condition:
		_failures.append(p_message)
	return p_condition


func _fail(p_message: String) -> void:
	_checks += 1
	_failures.append(p_message)
