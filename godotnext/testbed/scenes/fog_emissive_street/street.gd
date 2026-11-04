extends Node3D
## Fog + Emissive Street (#52). fog_off disables volumetric fog and the
## three FogVolumes. The signs and their lights stay.

const SceneArgs = preload("res://scenes/scene_args.gd")

const VARIANTS: Array[String] = ["fog_on", "fog_off"]

var _variant := "fog_on"
var _limit := -1
var _local_frame := 0
var _saw := false


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	process_priority = 1
	var requested := SceneArgs.value("--variant", "fog_on")
	if requested in VARIANTS:
		_variant = requested
	else:
		push_error("fog_emissive_street: unknown variant '%s', using fog_on" % requested)
	_limit = SceneArgs.int_value("--frames", -1)
	_enable_nextgi()
	var fog_on := _variant == "fog_on"
	var world := get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		world.environment.volumetric_fog_enabled = fog_on
	var volumes := get_node_or_null("FogVolumes")
	if volumes != null:
		volumes.visible = fog_on
	print("fog_emissive_street: variant=%s volumes=%d" % [
		_variant, 0 if volumes == null else volumes.get_child_count(),
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
	world.environment = environment
