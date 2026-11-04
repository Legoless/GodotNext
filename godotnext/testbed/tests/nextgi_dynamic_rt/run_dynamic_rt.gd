extends Node3D

## Engine-path check for GPU-written TLAS instances (#57).
##
## Builds one small animated dynamic MultiMesh (it must appear in the TLAS), one huge animated
## dynamic MultiMesh that cannot fit the 100k budget (its elements must surface as
## RT_INSTANCE_CAP), and a GPUParticles3D system, then reads nextgi_get_counters().
##
## Run with the NextGI project setting on, e.g.:
##   GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
##     MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
##     godot --path godotnext/testbed --rendering-driver metal res://tests/nextgi_dynamic_rt/run_dynamic_rt.tscn
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const SMALL_COUNT := 64
const HUGE_COUNT := 110000
const FRAMES := 24

var _small: MultiMeshInstance3D
var _huge: MultiMeshInstance3D
var _frame := 0
var _failures := PackedStringArray()


func _ready() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.02, 0.02, 0.03)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(1, 1, 1)
	env.ambient_light_energy = 0.2
	env.nextgi_enabled = true
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	var camera := Camera3D.new()
	camera.position = Vector3(0, 0, 12)
	add_child(camera)
	camera.look_at(Vector3.ZERO)
	camera.current = true

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-60, -30, 0)
	add_child(light)

	_small = _make_multimesh(SMALL_COUNT, 0.2, 2.0)
	_huge = _make_multimesh(HUGE_COUNT, 0.05, 40.0)
	_make_particles()


func _make_multimesh(count: int, size: float, spread: float) -> MultiMeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = Vector3(size, size, size)
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.mesh = mesh
	multimesh.instance_count = count
	var rows := PackedFloat32Array()
	rows.resize(count * 12)
	var side := int(ceil(sqrt(float(count))))
	for i in count:
		var x := (float(i % side) / float(side) - 0.5) * spread
		var y := (float(i / side) / float(side) - 0.5) * spread
		var o := i * 12
		rows[o + 0] = 1.0
		rows[o + 3] = x
		rows[o + 5] = 1.0
		rows[o + 7] = y
		rows[o + 10] = 1.0
	multimesh.set_buffer(rows)
	var instance := MultiMeshInstance3D.new()
	instance.multimesh = multimesh
	add_child(instance)
	return instance


func _make_particles() -> void:
	var particles := GPUParticles3D.new()
	particles.amount = 1024
	particles.lifetime = 1.0
	particles.emitting = true
	particles.position = Vector3(0, 0, -3)
	var mesh := BoxMesh.new()
	mesh.size = Vector3(0.05, 0.05, 0.05)
	particles.draw_pass_1 = mesh
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(1, 0.5, 0.2)
	mesh.material = material
	add_child(particles)


func _process(_delta: float) -> void:
	_frame += 1
	# One updated element per frame keeps both systems in the RT dynamic class without a rebuild.
	_small.multimesh.set_instance_transform(0, Transform3D(Basis(), Vector3(0, 0, 0.25)))
	_huge.multimesh.set_instance_transform(0, Transform3D(Basis(), Vector3(0, 0, -0.25)))
	if _frame < FRAMES:
		return

	var status: Dictionary = RenderingServer.nextgi_get_status()
	var counters: Dictionary = RenderingServer.nextgi_get_counters()
	var tier := String(status.get("tier", ""))
	var instance_cap := int(counters.get("rt_instance_cap", 0))
	var tlas_instances := int(counters.get("tlas_instances", 0))

	if tier != "HWRT":
		_failures.append("tier is %s, expected HWRT" % tier)
	if instance_cap < HUGE_COUNT:
		_failures.append("rt_instance_cap %d does not count the %d over-budget elements" % [instance_cap, HUGE_COUNT])
	if tlas_instances < SMALL_COUNT:
		_failures.append("tlas_instances %d does not include the %d small dynamic elements" % [tlas_instances, SMALL_COUNT])

	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "nextgi_dynamic_rt",
		"pass": ok,
		"tier": tier,
		"tlas_instances": tlas_instances,
		"rt_instance_cap": instance_cap,
		"rt_coverage": int(counters.get("rt_coverage", 0)),
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)
