extends SceneTree
## Checks the Feature Parity Room streaming variant (#74).
##
## The room is instanced, both capture cameras are still reachable, and
## texture streaming lives only in this folder's override.cfg.
##
## [code]godot --headless --path godotnext/testbed --script res://scenes/feature_parity_room_streaming/check_streaming.gd[/code]

const SCENE_PATH := "res://scenes/feature_parity_room_streaming/feature_parity_room_streaming.tscn"
const OVERRIDE_PATH := "res://scenes/feature_parity_room_streaming/override.cfg"
const ROOM_OVERRIDE_PATH := "res://scenes/feature_parity_room/override.cfg"
const PROJECT_PATH := "res://project.godot"
const STREAMING_KEY := "textures/streaming/enabled"
const BOUNCE_KEY := "lights_and_shadows/multi_bounce_occlusion/enabled"

var _failures: Array[String] = []
var _checks := 0


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var packed := load(SCENE_PATH) as PackedScene
	if packed == null:
		printerr("check_streaming FAIL: cannot load " + SCENE_PATH)
		quit(1)
		return
	var root := packed.instantiate()
	_ok(root.get_node_or_null("Room") != null, "Room instance is missing.")
	for camera_name in ["cam_0", "cam_1", "cam_2"]:
		var camera := root.get_node_or_null("Room/" + camera_name)
		_ok(camera is Camera3D, "Room/%s must be a Camera3D." % camera_name)
	root.free()

	var override_text := FileAccess.get_file_as_string(OVERRIDE_PATH)
	_ok(override_text.contains(BOUNCE_KEY + "=true"), "streaming override must keep multi-bounce occlusion on.")
	_ok(override_text.contains(STREAMING_KEY + "=true"), "streaming override must enable texture streaming.")
	var room_override := FileAccess.get_file_as_string(ROOM_OVERRIDE_PATH)
	_ok(not room_override.contains(STREAMING_KEY), "the P4 room override must not enable texture streaming.")
	_ok(room_override.contains(BOUNCE_KEY + "=true"), "the P4 room override must still enable multi-bounce occlusion.")
	var project := FileAccess.get_file_as_string(PROJECT_PATH)
	_ok(not project.contains(STREAMING_KEY), "project.godot must not set texture streaming.")
	_ok(not project.contains("multi_bounce_occlusion"), "project.godot must not set multi-bounce occlusion.")

	if _failures.is_empty():
		print("check_streaming: all %d assertions passed." % _checks)
		quit(0)
		return
	for failure in _failures:
		printerr("check_streaming FAIL: " + failure)
	quit(1)


func _ok(p_condition: bool, p_message: String) -> void:
	_checks += 1
	if not p_condition:
		_failures.append(p_message)
