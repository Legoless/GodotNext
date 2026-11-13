extends Node3D

## Translucency volume check for #61.
##
## Loads Glass & Water, turns NextGI on and asserts that the 1/16-screen x 32
## slice volume exists with the expected footprint: six RGBA16F volumes, the
## three SH channels and their three history copies (#133). The scene also
## exercises the alpha branch on Metal, so validation errors show up in the
## run log.
##
## Run through run_translucency.sh, which enables NextGI.

const FRAMES := 24

var _failures := PackedStringArray()


func _ready() -> void:
	var packed: PackedScene = load("res://scenes/glass_water/glass_water.tscn")
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
	var bytes := int(info.get("translucency_volume_bytes", 0))
	var size := get_viewport().get_visible_rect().size
	var expected := int(ceil(size.x / 16.0)) * int(ceil(size.y / 16.0)) * 32 * 6 * 8

	if tier != "HWRT":
		_failures.append("tier is %s, expected HWRT" % tier)
	if bytes <= 0:
		_failures.append("the translucency volume was not created")
	elif bytes != expected:
		_failures.append("translucency volume bytes %d do not match the expected %d" % [bytes, expected])

	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "nextgi_translucency",
		"pass": ok,
		"tier": tier,
		"bytes": bytes,
		"expected": expected,
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)
