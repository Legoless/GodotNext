extends Node3D

## Capture wrapper for the NextGI specular defaults (#54).
##
## Loads Color Box Room A, turns NextGI on and re-exposes its CaptureCam at the
## root as "CaptureCam" so the capture harness can drive it. The harness quits.

func _ready() -> void:
	var packed: PackedScene = load("res://scenes/color_box_room/cbr_a.tscn")
	var scene := packed.instantiate()
	add_child(scene)
	var world := scene.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		var environment := world.environment.duplicate()
		environment.nextgi_enabled = true
		world.environment = environment
	var source := scene.get_node_or_null("CaptureCam") as Camera3D
	var camera := Camera3D.new()
	camera.name = "CaptureCam"
	if source != null:
		camera.transform = source.transform
		camera.fov = source.fov
		camera.near = source.near
		camera.far = source.far
	add_child(camera)
	camera.current = true
