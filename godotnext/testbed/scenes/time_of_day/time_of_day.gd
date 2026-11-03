extends Node3D
## Applies Time of Day variants from the frame index (#33).

const Motion = preload("res://scenes/time_of_day/motion.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")

const VARIANTS: Array[String] = ["sweep", "sun_jump"]

var _variant := "sweep"
var _hold := -1
var _local_frame := 0
var _saw := false
var _sun_origin := Vector3.ZERO
var _marked: Dictionary = {}


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	process_priority = 1
	var requested := SceneArgs.value("--variant", "sweep")
	if requested in VARIANTS:
		_variant = requested
	else:
		push_error("time_of_day: unknown variant '%s', using sweep" % requested)
		_variant = "sweep"
	_hold = SceneArgs.int_value("--hold-frame", -1)
	var sun := get_node_or_null("Sun") as Node3D
	if sun != null:
		_sun_origin = sun.position
	print("time_of_day: variant=%s hold_frame=%d" % [_variant, _hold])


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, _hold)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	_apply(int(stepped["frame"]))


func _apply(p_frame: int) -> void:
	var elevation := Motion.elevation_deg(_variant, p_frame)
	var sun := get_node_or_null("Sun") as DirectionalLight3D
	if sun != null:
		sun.transform = Transform3D(Motion.sun_basis(elevation), _sun_origin)
		sun.set_meta("elevation_deg", elevation)
	if _variant == "sun_jump":
		_mark_once(0, "time_of_day: sun_jump elevation delta=%.6f deg (base=%.6f applied=%.6f)" % [
			Motion.jump_delta_deg(), Motion.BASE_ELEVATION_DEG, elevation])
	if _variant == "sweep" and (p_frame == 0 or p_frame == 300 or p_frame == Motion.SWEEP_END_FRAME):
		_mark_once(p_frame, "time_of_day: sweep frame %d elevation_deg=%.6f" % [p_frame, elevation])


func _mark_once(p_key: int, p_line: String) -> void:
	if _marked.has(p_key):
		return
	_marked[p_key] = true
	print(p_line)
