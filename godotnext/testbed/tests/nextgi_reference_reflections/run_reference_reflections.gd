extends Node3D

## Reflection-only reference check for #60 step 6.
##
## Loads the G1 static scene, turns NextGI on with the reference path tracer in
## reflection-only mode, and checks that the reference accumulated samples and
## produced a non-zero reflection image. The reference renders only when the
## project setting `rendering/global_illumination/nextgi/debug/reference_mode`
## is 3; run it through run_reference_reflections.sh.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const FRAMES := 4

var _failures := PackedStringArray()


func _ready() -> void:
	# The reference path tracer is expensive; a small window keeps the build check quick.
	get_window().size = Vector2i(320, 180)
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


func _run() -> void:
	for i in FRAMES:
		await RenderingServer.frame_post_draw

	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var tier := str(info.get("tier", ""))
	var samples := int(info.get("reference_samples", 0))
	var mean := float(info.get("reference_mean", 0.0))

	if int(ProjectSettings.get_setting("rendering/global_illumination/nextgi/debug/reference_mode", 0)) != 3:
		_failures.append("reference_mode is not 3; run the check through its runner")
	if tier != "HWRT":
		_failures.append("tier is %s, expected HWRT" % tier)
	if samples <= 0:
		_failures.append("the reflection-only reference accumulated no samples")
	if mean <= 0.0:
		_failures.append("the reflection-only reference image is black")

	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "nextgi_reference_reflections",
		"pass": ok,
		"tier": tier,
		"samples": samples,
		"mean": mean,
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)
