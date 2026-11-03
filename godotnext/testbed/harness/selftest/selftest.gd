extends Node3D

## One hundred cubes, one shadowed directional light, and SDFGI.
## The layout uses its own generator so it does not depend on the global RNG.

const CUBE_COUNT := 100
const LAYOUT_SEED := 1


func _ready() -> void:
	var sky_material := ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_material

	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_BG
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	environment.tonemap_exposure = 1.0
	environment.glow_enabled = false
	environment.adjustment_enabled = false
	environment.sdfgi_enabled = true
	# Bounce feedback feeds probe results back into the cascades, so the image
	# keeps moving and two captures of the same seed do not hash the same.
	# Zero keeps SDFGI on and lets the probe history reach a fixed point.
	environment.sdfgi_bounce_feedback = 0.0
	RenderingServer.environment_set_sdfgi_frames_to_converge(RenderingServer.ENV_SDFGI_CONVERGE_IN_5_FRAMES)
	RenderingServer.environment_set_sdfgi_frames_to_update_light(RenderingServer.ENV_SDFGI_UPDATE_LIGHT_IN_1_FRAME)

	var world_environment := WorldEnvironment.new()
	world_environment.name = "WorldEnvironment"
	world_environment.environment = environment
	add_child(world_environment)

	var light := DirectionalLight3D.new()
	light.name = "Sun"
	light.rotation_degrees = Vector3(-50.0, 30.0, 0.0)
	light.light_energy = 1.0
	light.shadow_enabled = true
	add_child(light)

	var camera := Camera3D.new()
	camera.name = "CaptureCam"
	camera.position = Vector3(8.0, 6.0, 12.0)
	add_child(camera)
	camera.look_at(Vector3.ZERO)
	camera.current = true

	var mesh := BoxMesh.new()
	mesh.size = Vector3(0.4, 0.4, 0.4)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.75, 0.75, 0.75)
	material.roughness = 1.0
	material.metallic = 0.0
	material.diffuse_mode = StandardMaterial3D.DIFFUSE_LAMBERT

	var generator := RandomNumberGenerator.new()
	generator.seed = LAYOUT_SEED
	for _index in CUBE_COUNT:
		var cube := MeshInstance3D.new()
		cube.mesh = mesh
		cube.material_override = material
		cube.position = Vector3(
			generator.randf_range(-3.0, 3.0),
			generator.randf_range(-3.0, 3.0),
			generator.randf_range(-3.0, 3.0)
		)
		add_child(cube)
