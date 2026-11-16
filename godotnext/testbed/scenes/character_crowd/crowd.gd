extends Node3D
## PN Character Crowd (#148, plan §21.4.3): 100 skinned walkers of the P3
## CC0 rig at 500k triangles each, looping the walk cycle at seeded phases.
##
## The testbed generator (`generators/build_pn_content.gd`) creates the
## actors under the `Crowd` node and the cameras; this script only advances
## the animation so the scene renders as a crowd. Animation is frame-indexed
## as in the other dynamic scenes (`SceneArgs.step_frame`): a driving harness
## publishes the frame. `--frames=N` quits after N frames when nothing drives
## the scene.

const Character = preload("res://scenes/character_walk/character.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")

const CROWD_COUNT := 100

var _local_frame := 0
var _saw := false
var _limit := -1


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	process_priority = 1
	_limit = SceneArgs.int_value("--frames", -1)


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, -1)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	var frame := int(stepped["frame"])
	for index in CROWD_COUNT:
		var actor := get_node_or_null("Crowd/Crowd%03d" % index) as Node3D
		if actor != null:
			Character.apply_walk_frame(actor, frame, index * 3)
	if _limit >= 0 and _local_frame >= _limit:
		get_tree().quit(0)
