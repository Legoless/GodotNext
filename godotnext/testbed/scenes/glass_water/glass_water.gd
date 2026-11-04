extends Node3D
## Glass & Water (#52). The omni light steps from 0 to 8 at frame 0.
## fence_scissor and fence_opaque select the alpha-scissor fence.

const SceneArgs = preload("res://scenes/scene_args.gd")

const VARIANTS: Array[String] = ["light_toggle", "fence_scissor", "fence_opaque"]

var _variant := "light_toggle"
var _hold := -1
var _limit := -1
var _local_frame := 0
var _saw := false
var _marked := false


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	process_priority = 1
	var requested := SceneArgs.value("--variant", "light_toggle")
	if requested in VARIANTS:
		_variant = requested
	else:
		push_error("glass_water: unknown variant '%s', using light_toggle" % requested)
	_hold = SceneArgs.int_value("--hold-frame", -1)
	_limit = SceneArgs.int_value("--frames", -1)
	_enable_nextgi()
	_show_fence()
	print("glass_water: variant=%s" % _variant)
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
	var light := get_node_or_null("Omni") as OmniLight3D
	if light != null:
		light.light_energy = 8.0 if p_frame >= 0 else 0.0
	if p_frame == 0 and not _marked:
		_marked = true
		print("glass_water: light_toggle at frame 0 (energy 0 -> 8)")


func _show_fence() -> void:
	var scissor := get_node_or_null("FenceScissor") as Node3D
	var opaque := get_node_or_null("FenceOpaque") as Node3D
	var use_opaque := _variant == "fence_opaque"
	if scissor != null:
		scissor.visible = not use_opaque
	if opaque != null:
		opaque.visible = use_opaque


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
