extends Node

## Closed-box furnace through the reference scene path (#27).
## Lambertian faces, rho = 0.8, emission Le = 1, no lights. Equilibrium is 5.

const EXPECTED := 5.0
const TARGET_SAMPLES := 8192
const RELATIVE := 0.01


func _ready() -> void:
	var result := await run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func run() -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(32, 32)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(viewport)

	var world := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0, 0, 0)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_energy = 0.0
	environment.nextgi_enabled = true
	environment.sdfgi_enabled = false
	world.environment = environment
	viewport.add_child(world)

	var camera := Camera3D.new()
	camera.current = true
	viewport.add_child(camera)

	var mesh_instance := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(4, 4, 4)
	mesh_instance.mesh = box
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.8, 0.8, 0.8)
	material.roughness = 1.0
	material.metallic = 0.0
	material.metallic_specular = 0.0
	material.emission_enabled = true
	material.emission = Color(1, 1, 1)
	material.emission_energy_multiplier = 1.0
	mesh_instance.material_override = material
	mesh_instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	viewport.add_child(mesh_instance)

	var info := {}
	for _frame in 400:
		await get_tree().process_frame
		info = RenderingServer.nextgi_get_tier_info()
		if int(info.get("reference_samples", 0)) >= TARGET_SAMPLES:
			break
	var samples := int(info.get("reference_samples", 0))
	var mean := float(info.get("reference_mean", 0.0))
	var lo := float(info.get("reference_min", 0.0))
	var hi := float(info.get("reference_max", 0.0))
	var failures := PackedStringArray()
	if samples < TARGET_SAMPLES:
		failures.append("samples %d" % samples)
	if absf(mean - EXPECTED) / EXPECTED > RELATIVE:
		failures.append("mean %.4f vs %.1f" % [mean, EXPECTED])
	if absf(lo - EXPECTED) / EXPECTED > RELATIVE or absf(hi - EXPECTED) / EXPECTED > RELATIVE:
		failures.append("pixels %.4f..%.4f outside 1 percent" % [lo, hi])
	return {
		"pass": failures.is_empty(),
		"failures": " | ".join(failures),
		"samples": samples,
		"mean": mean,
		"min": lo,
		"max": hi,
		"hash": String(info.get("reference_hash", "")),
		"instances": int(info.get("rt_instance_count", 0)),
	}
