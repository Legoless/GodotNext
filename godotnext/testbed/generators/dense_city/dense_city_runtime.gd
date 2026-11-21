extends Node3D
## Deterministic Dense City flight and GV-3 lighting runtime (#231).
## The generated flight variant enables the camera by default. GV-3 enables
## its directional-light storm with --storm=1; either variant accepts
## --flight=1 and --hold-frame=<row>.

const Flight := preload("res://scenes/slow_io_flight/flight.gd")
const SceneArgs := preload("res://scenes/scene_args.gd")

const SUN_DEGREES_PER_FRAME := 0.5

var _flight := false
var _storm := false
var _hold := -1
var _local_frame := 0
var _saw := false
var _camera: Camera3D
var _sun: DirectionalLight3D
var _sun_start := Transform3D.IDENTITY


static func sun_transform_at_frame(p_frame: int, p_start: Transform3D) -> Transform3D:
	var basis := Basis(Vector3.UP, deg_to_rad(SUN_DEGREES_PER_FRAME) * float(maxi(p_frame, 0))) * p_start.basis
	return Transform3D(basis, p_start.origin)


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	process_priority = 1
	_flight = bool(get_meta("flight", false)) or SceneArgs.value("--flight", "0") == "1"
	_storm = SceneArgs.value("--storm", "0") == "1"
	_hold = SceneArgs.int_value("--hold-frame", -1)
	_camera = get_node_or_null("FlightCam") as Camera3D
	_sun = get_node_or_null("Sun") as DirectionalLight3D
	if _sun != null:
		_sun_start = _sun.transform
		if _storm:
			# The export carries a static sky panorama and the direct light's exact track.
			_sun.sky_mode = DirectionalLight3D.SKY_MODE_LIGHT_ONLY
	_apply(0)


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, _hold)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	_apply(int(stepped["frame"]))


func _apply(p_frame: int) -> void:
	if _flight and _camera != null:
		_camera.global_transform = Flight.camera_transform(p_frame)
		_camera.current = true
		var main_camera := get_node_or_null("cam_main") as Camera3D
		if main_camera != null:
			main_camera.current = false
		_camera.set_meta("flight_frame", p_frame)
	if _storm and _sun != null:
		_sun.transform = sun_transform_at_frame(p_frame, _sun_start)
		_sun.set_meta("storm_frame", p_frame)
