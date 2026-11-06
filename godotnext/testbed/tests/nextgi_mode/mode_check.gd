extends Node3D
## NextGI mode check (#132). Loads Color Box Room A with NextGI on and verifies
## that rendering/global_illumination/nextgi/mode selects reflections-only: the
## status reports the mode and the diffuse gather dispatches drop to zero while
## reflections keep rendering.

const CBR := "res://scenes/color_box_room/cbr_a.tscn"
const WAIT_FRAMES := 20

var _requested := 0
var _frames := 0


func _ready() -> void:
	_requested = int(ProjectSettings.get_setting("rendering/global_illumination/nextgi/mode", 0))
	var packed: PackedScene = load(CBR)
	var scene: Node3D = packed.instantiate()
	add_child(scene)
	var world := scene.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		var environment := world.environment.duplicate()
		environment.nextgi_enabled = true
		# Exercise the reflection fog path as well.
		environment.fog_enabled = true
		environment.fog_light_color = Color(0.6, 0.65, 0.7)
		environment.fog_density = 0.02
		environment.fog_height = 2.0
		environment.fog_height_density = 0.02
		world.environment = environment
	var camera := scene.get_node_or_null("CaptureCam") as Camera3D
	if camera != null:
		camera.current = true


func _process(_delta: float) -> void:
	_frames += 1
	if _frames < WAIT_FRAMES:
		return
	var status: Dictionary = RenderingServer.nextgi_get_status()
	var counters: Dictionary = RenderingServer.nextgi_get_tier_info()
	var mode := str(status.get("mode", ""))
	var diffuse := int(counters.get("gather_diffuse_dispatches", -1))
	print("NEXTGI_MODE requested=%d mode=%s diffuse=%d" % [_requested, mode, diffuse])
	var expected := "reflections_only" if _requested == 1 else "gi_and_reflections"
	var ok := mode == expected
	if _requested == 1 and diffuse != 0:
		ok = false
	if _requested == 0 and diffuse <= 0:
		ok = false
	print("NEXTGI_MODE_RESULT pass=%s" % str(ok))
	get_tree().quit(0 if ok else 1)
