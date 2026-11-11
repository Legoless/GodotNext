extends Node3D

## Two NextGI views of different sizes in one frame (#186).
##
## Two SubViewports share one World3D: a 960x540 view and a 640x360 view. Buffers that the views
## share and update once per view (the forward pass's scene uniform buffers, the gather's
## parameters) were read with the other view's contents when the GPU ran the next frame's first
## work before this frame's last work had finished. Only the small view reading the large view's
## layout (cluster grid, probe grid) reads past the end of its own buffers, which Metal shader
## validation reports, so the large view comes first in the tree: in the other order the same
## fault stays in bounds and goes unreported. run_two_sizes_check.sh runs this under validation
## and fails on any report.
##
## Pass/fail: the gather runs for both views, so gather_diffuse_dispatches grows by exactly 2 per
## frame over the measured frames, on the HWRT or the SWRT tier.

const SIZES: Array[Vector2i] = [Vector2i(960, 540), Vector2i(640, 360)]
const WARMUP_FRAMES := 30
const MEASURE_FRAMES := 60

var _failures := PackedStringArray()


func _ready() -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = Sky.new()
	environment.sky.sky_material = ProceduralSkyMaterial.new()
	environment.nextgi_enabled = true
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	add_child(world_environment)

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-50, -30, 0)
	light.shadow_enabled = true
	add_child(light)

	var floor_mesh := PlaneMesh.new()
	floor_mesh.size = Vector2(20, 20)
	var floor_instance := MeshInstance3D.new()
	floor_instance.mesh = floor_mesh
	add_child(floor_instance)
	for i in 6:
		var box := BoxMesh.new()
		box.size = Vector3(1.5, 2.0 + i * 0.3, 1.5)
		var instance := MeshInstance3D.new()
		instance.mesh = box
		instance.position = Vector3(-5 + i * 2.0, 1.0, -2.0 + (i % 2) * 2.0)
		add_child(instance)

	for index in SIZES.size():
		var viewport := SubViewport.new()
		viewport.name = "Viewport%d" % index
		viewport.size = SIZES[index]
		viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
		var camera := Camera3D.new()
		viewport.add_child(camera)
		add_child(viewport)
		camera.look_at_from_position(Vector3(index * 6.0 - 3.0, 3.0, 8.0), Vector3(0, 1, 0))
		camera.current = true
	_run()


func _run() -> void:
	for i in WARMUP_FRAMES:
		await RenderingServer.frame_post_draw
	var start := int(RenderingServer.nextgi_get_tier_info().get("gather_diffuse_dispatches", 0))
	for i in MEASURE_FRAMES:
		await RenderingServer.frame_post_draw
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var dispatches := float(int(info.get("gather_diffuse_dispatches", 0)) - start) / float(MEASURE_FRAMES)

	var tier := str(info.get("gather_tier", ""))
	if tier != "HWRT" and tier != "SWRT":
		_failures.append("the gather ran on tier '%s', expected HWRT or SWRT" % tier)
	if not is_equal_approx(dispatches, 2.0):
		_failures.append("%.2f gather dispatches per frame, expected 2 (one per view)" % dispatches)

	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "nextgi_two_sizes",
		"pass": ok,
		"tier": str(info.get("tier", "")),
		"gather_tier": tier,
		"sizes": [SIZES[0], SIZES[1]],
		"dispatches_per_frame": dispatches,
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)
