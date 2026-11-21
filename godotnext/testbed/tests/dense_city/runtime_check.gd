extends SceneTree
## Headless deterministic checks for Dense City's flight and GV-3 runtime (#231).
## Run after the generator and --import; add -- --storm=1 for the moving-sun pass.

const Flight := preload("res://scenes/slow_io_flight/flight.gd")
const Runtime := preload("res://generators/dense_city/dense_city_runtime.gd")

var _failures: Array[String] = []
var _checks := 0


func _initialize() -> void:
	_run.call_deferred()


func _check(p_condition: bool, p_message: String) -> void:
	_checks += 1
	if not p_condition:
		_failures.append(p_message)


func _load(p_file: String) -> Node3D:
	var packed := load("res://generated/dense_city/" + p_file) as PackedScene
	_check(packed != null, "%s loads" % p_file)
	if packed == null:
		return null
	var city := packed.instantiate() as Node3D
	get_root().add_child(city)
	city.set_process(false)
	return city


func _run() -> void:
	if OS.get_cmdline_user_args().has("--storm=1"):
		_check_storm()
	else:
		_check_default_static()
		_check_flight()
		_check_gv3()
	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({"test": "dense_city_runtime", "storm": OS.get_cmdline_user_args().has("--storm=1"), "ok": ok, "checks": _checks, "failures": _failures}))
	quit(0 if ok else 1)


func _check_default_static() -> void:
	var city := _load("dense_city.tscn")
	if city == null:
		return
	_check(city.get_script() == null, "the original timed city keeps its static scene without the runtime script")
	var camera := city.get_node("cam_main") as Camera3D
	var pose := camera.transform
	var sun := (city.get_node("Sun") as DirectionalLight3D).transform
	for frame in [0, 8, 300, 599, 3599]:
		_check(camera.transform.is_equal_approx(pose), "the default camera is static at row %d" % frame)
		_check((city.get_node("Sun") as DirectionalLight3D).transform.is_equal_approx(sun), "the default sun is static at row %d" % frame)
	_check(city.get_node_or_null("FlightCam") == null, "the original city content is unchanged")
	city.free()


func _check_flight() -> void:
	var city := _load("dense_city_flight.tscn")
	if city == null:
		return
	var camera := city.get_node("FlightCam") as Camera3D
	_check(camera.current and not (city.get_node("cam_main") as Camera3D).current, "the flight camera is active")
	for frame in [0, 8, 300, 599, 3599]:
		city.call("_apply", frame)
		_check(camera.global_transform.is_equal_approx(Flight.camera_transform(frame)), "FlightCam matches the canonical transform at row %d" % frame)
		_check(int(camera.get_meta("flight_frame", -1)) == frame, "FlightCam records row %d" % frame)
	var bounds := AABB()
	var bounds_set := false
	for value: Node in city.find_children("*", "MeshInstance3D", true, false):
		var instance := value as MeshInstance3D
		var box := instance.global_transform * instance.get_aabb()
		bounds = box if not bounds_set else bounds.merge(box)
		bounds_set = true
	_check(bounds_set, "the flight has city bounds")
	var furthest := 0.0
	if bounds_set:
		for frame in Flight.FLIGHT_FRAMES:
			var origin := Flight.camera_transform(frame).origin
			for corner in 8:
				var point := bounds.position + bounds.size * Vector3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1)
				furthest = maxf(furthest, origin.distance_to(point))
	_check(furthest <= camera.far, "every city-bounds corner is inside FlightCam's far plane on all 3,600 rows (worst %.3f m, far %.3f m)" % [furthest, camera.far])
	city.free()


func _check_gv3() -> void:
	var city := _load("dense_city_gv3.tscn")
	if city == null:
		return
	_check_local_lights(city)
	var sun := city.get_node("Sun") as DirectionalLight3D
	var start := sun.transform
	city.call("_process", 0.0)
	city.call("_process", 0.0)
	_check(sun.transform.is_equal_approx(start), "the steady-state GV-3 sun holds still")
	_check((city.get_node("cam_main") as Camera3D).current, "GV-3 keeps the static street camera by default")
	city.free()


func _check_storm() -> void:
	var city := _load("dense_city_gv3.tscn")
	if city == null:
		return
	_check_local_lights(city)
	var sun := city.get_node("Sun") as DirectionalLight3D
	var start := sun.transform
	_check(sun.sky_mode == DirectionalLight3D.SKY_MODE_LIGHT_ONLY, "the storm keeps the exported panorama independent of the moving direct light")
	for frame in [0, 8, 300, 599, 3599]:
		city.call("_apply", frame)
		var expected := Transform3D(Basis(Vector3.UP, deg_to_rad(0.5) * float(frame)) * start.basis, start.origin)
		_check(sun.transform.is_equal_approx(expected), "the storm sun turns exactly 0.5 degrees per row at %d" % frame)
		_check(sun.transform.is_equal_approx(Runtime.sun_transform_at_frame(frame, start)), "the exported runtime helper agrees at row %d" % frame)
	city.free()


func _check_local_lights(p_city: Node3D) -> void:
	var shadowed := 0
	var omnis := 0
	var spots := 0
	for child in p_city.get_children():
		if child is OmniLight3D or child is SpotLight3D:
			if (child as Light3D).shadow_enabled:
				shadowed += 1
				if child is OmniLight3D:
					omnis += 1
				else:
					spots += 1
	_check(shadowed == 16 and omnis == 8 and spots == 8, "GV-3 has exactly 16 shadowed local lights (8 omni, 8 spot)")
