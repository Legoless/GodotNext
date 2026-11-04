extends Node3D
## Character Walk variants (#52). The character crosses the sun/shadow
## threshold over 4 seconds at 60 fps. Poses hold a path frame. vacated
## and empty leave the character out. soak_20 places 20 copies.

const Character = preload("res://scenes/character_walk/character.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")

const VARIANTS: Array[String] = [
	"walk", "pose_0", "pose_1", "pose_2", "pose_3", "pose_4", "vacated", "empty", "soak_20",
]

var _variant := "walk"
var _hold := -1
var _limit := -1
var _local_frame := 0
var _saw := false
var _actors: Array[Node3D] = []


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	process_priority = 1
	var requested := SceneArgs.value("--variant", "walk")
	if requested in VARIANTS:
		_variant = requested
	else:
		push_error("character_walk: unknown variant '%s', using walk" % requested)
	_hold = SceneArgs.int_value("--hold-frame", -1)
	_limit = SceneArgs.int_value("--frames", -1)
	_enable_nextgi()
	var template := get_node_or_null("Character") as Node3D
	if template == null:
		push_error("character_walk: Character node is missing")
		return
	if _variant == "vacated" or _variant == "empty":
		template.visible = false
	elif _variant == "soak_20":
		_actors.append(template)
		for index in range(1, Character.SOAK_COUNT):
			var copy := template.duplicate() as Node3D
			copy.name = "Character%d" % index
			add_child(copy)
			_actors.append(copy)
	else:
		_actors.append(template)
	print("character_walk: variant=%s actors=%d triangles=%d" % [
		_variant, _actors.size() if _variant != "vacated" and _variant != "empty" else 0, Character.triangle_count(),
	])
	_apply(0 if _hold < 0 else _hold)


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, _hold)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	_apply(int(stepped["frame"]))
	if _limit >= 0 and _local_frame >= _limit:
		get_tree().quit(0)


func _apply(p_frame: int) -> void:
	var frame := p_frame
	if _variant.begins_with("pose_"):
		frame = Character.pose_frame(int(_variant.substr(5)))
	elif _variant == "empty":
		frame = Character.PATH_FRAMES
	elif _variant == "vacated":
		frame = 0
	if _variant == "soak_20":
		var origin := Character.path_position(frame)
		for index in _actors.size():
			var actor := _actors[index]
			var angle := TAU * float(index) / float(_actors.size())
			actor.position = origin + Vector3(cos(angle) * 2.0, 0.0, sin(angle) * 2.0)
			Character.apply_walk_frame(actor, frame, index * 3)
		return
	if _actors.is_empty():
		return
	_actors[0].position = Character.path_position(frame)
	Character.apply_walk_frame(_actors[0], frame, 0)


func _enable_nextgi() -> void:
	var world := get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world == null or world.environment == null:
		return
	if not bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false)):
		return
	var environment := world.environment.duplicate()
	environment.nextgi_enabled = true
	environment.sdfgi_enabled = false
	world.environment = environment
