extends Node3D

## SRC feedback on rotated instances (#251).
##
## Two boxes float under an open sky with nothing else in the scene: one rotated 90 degrees about
## X, one rotated about Z and scaled non-uniformly. Each card's 16 feedback rays leave along its
## world normal's hemisphere. A box is convex, so every correctly oriented ray escapes to the sky.
## The card normal used to go through the transpose of the instance basis, which turned the
## rotated faces' hemispheres into the box, so those rays hit it.
##
## Pass/fail: src_feedback_rays grows and at least 85% of the feedback rays escape. Correct rays
## can still hit the other box, or, on the software tier, graze the box's own distance field near
## the surface: 97% escape on HWRT and 94% on SWRT on the M5 Max, against 31% with the transposed
## basis. The runner
## (run_rotated_feedback_check.sh) runs this on HWRT, on SWRT and under debug/deny_ray_query.

const WARMUP_FRAMES := 30
const MEASURE_FRAMES := 60
const MIN_ESCAPE_FRACTION := 0.85

var _failures := PackedStringArray()


func _ready() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(640, 360)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(viewport)

	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = Sky.new()
	environment.sky.sky_material = ProceduralSkyMaterial.new()
	environment.nextgi_enabled = true
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	viewport.add_child(world_environment)

	var camera := Camera3D.new()
	camera.position = Vector3(0, 1.5, 7)
	camera.look_at_from_position(camera.position, Vector3(0, 0, 0))
	viewport.add_child(camera)

	var sun := DirectionalLight3D.new()
	sun.rotation = Vector3(-0.8, 0.5, 0)
	viewport.add_child(sun)

	var rotated := _box(Transform3D(Basis(Vector3.RIGHT, PI * 0.5), Vector3(-1.6, 0, 0)))
	viewport.add_child(rotated)
	var scaled := _box(Transform3D(Basis(Vector3.BACK, PI * 0.5).scaled(Vector3(1.5, 0.6, 1.0)), Vector3(1.6, 0, 0)))
	viewport.add_child(scaled)

	for _frame in WARMUP_FRAMES:
		await RenderingServer.frame_post_draw
	var rays := 0
	var escapes := 0
	var tier := ""
	for _frame in MEASURE_FRAMES:
		await RenderingServer.frame_post_draw
		var info := RenderingServer.nextgi_get_tier_info()
		tier = String(info.get("tier", ""))
		rays += int(info.get("src_feedback_rays", 0))
		escapes += int(info.get("src_feedback_escapes", 0))

	var fraction := float(escapes) / float(rays) if rays > 0 else 0.0
	if tier != "HWRT" and tier != "SWRT":
		_failures.append("tier %s is not active; run through run_rotated_feedback_check.sh" % tier)
	if rays == 0:
		_failures.append("no SRC feedback rays were traced")
	elif fraction < MIN_ESCAPE_FRACTION:
		_failures.append("%.3f of the feedback rays escaped, expected at least %.2f" % [fraction, MIN_ESCAPE_FRACTION])
	var result := {
		"test": "rotated_feedback",
		"tier": tier,
		"feedback_rays": rays,
		"feedback_escapes": escapes,
		"escape_fraction": fraction,
		"pass": _failures.is_empty(),
		"failures": _failures,
	}
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _box(transform: Transform3D) -> MeshInstance3D:
	var instance := MeshInstance3D.new()
	var mesh := BoxMesh.new()
	mesh.size = Vector3(1.4, 1.4, 1.4)
	instance.mesh = mesh
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	instance.transform = transform
	return instance
