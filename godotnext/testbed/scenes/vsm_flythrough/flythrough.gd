extends Node3D
## VSM Flythrough (#213). Moves the 2 m actor across the street at 5 m/s,
## optionally sweeps the sun 0.5° per frame for the time-of-day variant
## (`--tod`), and quits on `--frames=N`.

const SceneArgs = preload("res://scenes/scene_args.gd")

const STREET_START_X := -280.0
const STREET_END_X := 280.0
const ACTOR_SPEED_MPS := 5.0

var _frame := 0
var _limit := -1
var _time_of_day := false
var _sun_start := Basis.IDENTITY


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	_limit = SceneArgs.int_value("--frames", -1)
	_time_of_day = SceneArgs.value("--tod", "") != ""
	var sun := get_node_or_null("Sun") as DirectionalLight3D
	if sun != null:
		_sun_start = sun.transform.basis


func _process(delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var actor := get_node_or_null("Actor") as Node3D
	if actor != null:
		actor.position.x += ACTOR_SPEED_MPS * delta
		if actor.position.x > STREET_END_X:
			actor.position.x = STREET_START_X
	if _time_of_day:
		var sun := get_node_or_null("Sun") as DirectionalLight3D
		if sun != null:
			sun.transform.basis = Basis(Vector3.UP, deg_to_rad(0.5) * float(_frame)) * _sun_start
	_frame += 1
	if _limit >= 0 and _frame >= _limit:
		get_tree().quit(0)
