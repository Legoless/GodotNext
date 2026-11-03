extends Node3D
## Applies Moving Light + Door variants from the frame index (#33).

const Motion = preload("res://scenes/moving_light_door/motion.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")

const VARIANTS: Array[String] = ["door_open", "light_toggle", "door_teleport"]

var _variant := "door_open"
var _hold := -1
var _local_frame := 0
var _saw := false
var _door_origin := Vector3.ZERO
var _lit_center := Vector3.ZERO
var _leak_logged := false
var _marked: Dictionary = {}


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	process_priority = 1
	var requested := SceneArgs.value("--variant", "door_open")
	if requested in VARIANTS:
		_variant = requested
	else:
		push_error("moving_light_door: unknown variant '%s', using door_open" % requested)
		_variant = "door_open"
	_hold = SceneArgs.int_value("--hold-frame", -1)
	var door := get_node_or_null("Door") as Node3D
	if door != null:
		_door_origin = door.position
	_lit_center = get_meta("lit_center", Vector3.ZERO) as Vector3
	print("moving_light_door: variant=%s hold_frame=%d" % [_variant, _hold])


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, _hold)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	_apply(int(stepped["frame"]))


func _apply(p_frame: int) -> void:
	var gap := Motion.door_gap_m(_variant, p_frame)
	var door := get_node_or_null("Door") as Node3D
	if door != null:
		door.position = _door_origin + Vector3(0.0, 0.0, gap)
	var light := get_node_or_null("MovingLight") as OmniLight3D
	if light != null:
		light.position = Motion.light_position(_variant, p_frame, _lit_center)
		light.light_energy = Motion.light_energy(_variant, p_frame)
	if gap >= Motion.LEAK_GAP_M and not _leak_logged:
		_leak_logged = true
		print("moving_light_door: first gap >= 0.02 m at frame %d (gap=%.6f m)" % [p_frame, gap])
	if _variant == "door_open" and (p_frame == 0 or p_frame == 60 or p_frame == 120):
		_mark_once(p_frame, "moving_light_door: gap frame %d = %.6f m" % [p_frame, gap])
	if _variant == "light_toggle":
		_mark_once(0, "moving_light_door: light_toggle at frame 0 (energy 0 -> 1)")
	if _variant == "door_teleport":
		_mark_once(0, "moving_light_door: door_teleport at frame 0 (gap 0 -> 1.000000 m)")


func _mark_once(p_key: int, p_line: String) -> void:
	if _marked.has(p_key):
		return
	_marked[p_key] = true
	print(p_line)
