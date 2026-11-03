extends SceneTree
## Feature Parity Room streaming variant (#74).
##
## Instances the #64 room from its own folder so texture streaming can be
## turned on without editing that room's override.cfg or project.godot.
## The scene text is written directly, so two runs match byte for byte.
##
## [code]godot --headless --path godotnext/testbed --script res://generators/build_feature_parity_streaming.gd[/code]

const Scaffold := preload("res://generators/scaffold.gd")

const SCENE_PATH := "res://scenes/feature_parity_room_streaming/feature_parity_room_streaming.tscn"

const SCENE_TEXT := """[gd_scene format=3]

[ext_resource type=\"PackedScene\" path=\"res://scenes/feature_parity_room/feature_parity_room.tscn\" id=\"1_room\"]

[node name=\"FeatureParityRoomStreaming\" type=\"Node3D\"]

[node name=\"Room\" parent=\".\" instance=ExtResource(\"1_room\")]
"""


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var err := Scaffold.write_text_chunked(SCENE_PATH, SCENE_TEXT)
	if err != OK:
		push_error("build_feature_parity_streaming: write failed with error %d." % err)
		quit(1)
		return
	print("build_feature_parity_streaming: wrote " + SCENE_PATH)
	quit(0)
