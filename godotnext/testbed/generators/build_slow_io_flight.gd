extends SceneTree
## Slow IO Flight generator (#74).
##
## Writes the seeded spline, a flight camera and a handful of stand-in
## boxes. The Dense City scene stays out of this file: it is generated on
## demand and instanced only with `--with-city=1`. Two runs are byte-identical.
##
## [code]godot --headless --path godotnext/testbed --script res://generators/build_slow_io_flight.gd[/code]

const Scaffold := preload("res://generators/scaffold.gd")
const Flight := preload("res://scenes/slow_io_flight/flight.gd")

const SCENE_PATH := "res://scenes/slow_io_flight/slow_io_flight.tscn"
const SCRIPT_PATH := "res://scenes/slow_io_flight/slow_io_flight.gd"
const STREAM := 75


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var contract := Flight.contract_error()
	if not contract.is_empty():
		push_error("build_slow_io_flight: " + contract)
		quit(1)
		return
	var root := _build()
	var err := Scaffold.save_scene(root, SCENE_PATH)
	root.free()
	if err != OK:
		push_error("build_slow_io_flight: save failed with error %d." % err)
		quit(1)
		return
	print("build_slow_io_flight: wrote " + SCENE_PATH)
	quit(0)


func _build() -> Node3D:
	var root := Node3D.new()
	root.name = "SlowIoFlight"
	root.set_script(load(SCRIPT_PATH))
	root.set_meta("seed", Scaffold.stream_seed(STREAM))
	root.set_meta("issue", 74)
	root.set_meta("variant", "delayed")

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.45, 0.62, 0.78)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.55, 0.55, 0.55)
	environment.ambient_light_energy = 1.0
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	environment.tonemap_exposure = 1.0
	var world := WorldEnvironment.new()
	world.name = "WorldEnvironment"
	world.environment = environment
	Scaffold.add_owned(root, world, root)

	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.light_energy = 1.1
	sun.shadow_enabled = false
	sun.rotation = Vector3(deg_to_rad(-42.0), deg_to_rad(20.0), 0.0)
	Scaffold.add_owned(root, sun, root)

	var path := Path3D.new()
	path.name = "FlightPath"
	path.curve = Flight.make_curve()
	Scaffold.add_owned(root, path, root)

	var material := Scaffold.make_constant_material("StandIn", Color(0.32, 0.34, 0.36))
	var stand_in := Node3D.new()
	stand_in.name = "StandIn"
	Scaffold.add_owned(root, stand_in, root)
	for index in Flight.CONTROL_POINTS.size():
		var point: Vector3 = Flight.CONTROL_POINTS[index]
		var builder = Scaffold.ChunkedSurfaceBuilder.new(material)
		builder.add_box(Vector3.ZERO, Vector3(12.0, 24.0, 12.0))
		var instance := MeshInstance3D.new()
		instance.name = "Block%d" % index
		instance.mesh = builder.finish()
		instance.position = point + Vector3(18.0, -6.0, 14.0)
		instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		Scaffold.add_owned(stand_in, instance, root)

	var origin := Flight.position_at_frame(0)
	var look := Flight.position_at_frame(8)
	Scaffold.make_camera(root, root, "FlightCam", origin, look, 4000.0, 60.0, true)
	return root
