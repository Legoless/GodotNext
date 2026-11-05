extends Node3D

## Quality preset switching check for #55.
##
## Loads Color Box Room A with NextGI on and switches the preset at runtime:
## Medium -> Epic -> High. Each change must be reflected in the status and the
## run must finish without validation errors (the runner log carries them).
##
## Run through run_quality.sh, which enables NextGI.

const FRAMES_PER_STEP := 8

var _failures := PackedStringArray()


func _ready() -> void:
	var packed: PackedScene = load("res://scenes/color_box_room/cbr_a.tscn")
	var scene := packed.instantiate()
	add_child(scene)
	var world := scene.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		var environment := world.environment.duplicate()
		environment.nextgi_enabled = true
		world.environment = environment
	var camera := scene.get_node_or_null("CaptureCam") as Camera3D
	if camera != null:
		camera.current = true
	_run()


func _set_quality(p_quality: int, p_expected: String) -> void:
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/quality", p_quality)
	for i in FRAMES_PER_STEP:
		await RenderingServer.frame_post_draw
	var status: Dictionary = RenderingServer.nextgi_get_status()
	var quality := str(status.get("quality", ""))
	if quality != p_expected:
		_failures.append("quality is %s, expected %s" % [quality, p_expected])


func _run() -> void:
	await _set_quality(1, "MEDIUM")
	await _set_quality(3, "EPIC")
	await _set_quality(2, "HIGH")

	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "nextgi_quality",
		"pass": ok,
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)
