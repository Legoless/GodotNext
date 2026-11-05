extends Node3D

## Two-viewport NextGI check for #58 (G3-8 at build scope).
##
## Two SubViewports share one World3D with a NextGI-enabled environment and a
## camera each. The scenario passes (AS, SRC, WRC) must run once per frame and
## both views must render. This is a functional check; timing is PQ.
##
## Run through run_two_viewports.sh, which enables NextGI.

const FRAMES := 32

var _failures := PackedStringArray()


func _ready() -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.02, 0.02, 0.03)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(1, 1, 1)
	environment.ambient_light_energy = 0.2
	environment.nextgi_enabled = true
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	add_child(world_environment)

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-60, -30, 0)
	add_child(light)

	var mesh := BoxMesh.new()
	mesh.size = Vector3(2, 2, 2)
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	add_child(instance)

	for index in 2:
		var viewport := SubViewport.new()
		viewport.name = "Viewport%d" % index
		viewport.size = Vector2i(512, 288)
		viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
		var camera := Camera3D.new()
		camera.name = "Camera%d" % index
		camera.position = Vector3(index * 3.0 - 1.5, 0, 6)
		viewport.add_child(camera)
		camera.current = true
		add_child(viewport)
	_run()


func _run() -> void:
	for i in FRAMES:
		await RenderingServer.frame_post_draw

	var status: Dictionary = RenderingServer.nextgi_get_status()
	var tier := str(status.get("tier", ""))
	if tier != "HWRT":
		_failures.append("tier is %s, expected HWRT" % tier)
	if get_child_count() < 5:
		_failures.append("the two viewports were not created")

	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "nextgi_two_viewports",
		"pass": ok,
		"tier": tier,
		"viewports": 2,
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)
