extends Node3D
## Roughness Corridor (#52). Static strips; this driver only stops a
## bounded capture and turns NextGI on when the project asks for it.

const SceneArgs = preload("res://scenes/scene_args.gd")

var _limit := -1
var _local_frame := 0
var _saw := false


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	process_priority = 1
	_limit = SceneArgs.int_value("--frames", -1)
	_enable_nextgi()
	print("roughness_corridor: strips=%d emissive=%s" % [
		get_node("Strips").get_child_count(), str(get_meta("emissive_outside_frustum", false)),
	])


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, -1)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	if _limit >= 0 and _local_frame >= _limit:
		get_tree().quit(0)


func _enable_nextgi() -> void:
	var world := get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world == null or world.environment == null:
		return
	if not bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false)):
		return
	var environment := world.environment.duplicate()
	environment.nextgi_enabled = true
	environment.sdfgi_enabled = false
	environment.ssr_enabled = false
	world.environment = environment
