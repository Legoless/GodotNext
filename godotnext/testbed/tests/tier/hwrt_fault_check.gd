extends Node

## Launch-to-launch HWRT fault latch (#36).
##
## run_hwrt_safety.sh writes override.cfg with NextGI on and an isolated user directory,
## then runs this scene. The scene draws a few frames so an injected device loss fires
## inside NextGI::render_frame, then prints one GODOTNEXT_RESULT line.
## _ready prints GODOTNEXT_FAULT_DIR before the first draw, so a crash still reports the path.

var _frames := 0


func _ready() -> void:
	print("GODOTNEXT_FAULT_DIR " + ProjectSettings.globalize_path("user://nextgi_hwrt_fault.cfg"))

	var environment := Environment.new()
	environment.nextgi_enabled = true
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 2.0)
	camera.current = true
	add_child(camera)

	var mesh_instance := MeshInstance3D.new()
	var box := BoxMesh.new()
	mesh_instance.mesh = box
	add_child(mesh_instance)

	var light := DirectionalLight3D.new()
	add_child(light)


func _process(_delta: float) -> void:
	_frames += 1
	if _frames < 4:
		return
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var latch_path := "user://nextgi_hwrt_fault.cfg"
	var result := {
		"tier_info": info,
		"latch_exists": FileAccess.file_exists(latch_path),
	}
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0)
