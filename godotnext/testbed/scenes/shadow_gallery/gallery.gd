extends Node3D
## Shadow Gallery (#213): alternates the 0.5° and 2° suns every two seconds
## so the harness can capture both penumbra rows, and quits on `--frames=N`.

const SceneArgs = preload("res://scenes/scene_args.gd")

const SWITCH_FRAMES := 120

var _frame := 0
var _limit := -1


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	_limit = SceneArgs.int_value("--frames", -1)


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var sun_a := get_node_or_null("SunA") as DirectionalLight3D
	var sun_b := get_node_or_null("SunB") as DirectionalLight3D
	if sun_a != null and sun_b != null:
		var use_a := (_frame / SWITCH_FRAMES) % 2 == 0
		sun_a.light_energy = 1.6 if use_a else 0.0
		sun_b.light_energy = 0.0 if use_a else 1.6
	_frame += 1
	if _limit >= 0 and _frame >= _limit:
		get_tree().quit(0)
