extends Node

## Static MultiMesh and GridMap octants enter the TLAS. A rewritten MultiMesh and particles do not (#43).


func _ready() -> void:
	var result := await run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func run() -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(viewport)
	var world := Node3D.new()
	viewport.add_child(world)
	var environment := Environment.new()
	environment.nextgi_enabled = true
	environment.sdfgi_enabled = false
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.4, 0.5, 0.6)
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	world.add_child(world_environment)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 4, 12)
	camera.rotation_degrees = Vector3(-18, 0, 0)
	world.add_child(camera)

	var box := BoxMesh.new()
	box.size = Vector3(0.4, 0.4, 0.4)
	var static_mesh := MultiMesh.new()
	static_mesh.transform_format = MultiMesh.TRANSFORM_3D
	static_mesh.mesh = box
	static_mesh.instance_count = 1000
	for index in 1000:
		var transform := Transform3D()
		transform.origin = Vector3(float(index % 40) * 0.5 - 10.0, 0.0, float(index / 40) * 0.5)
		static_mesh.set_instance_transform(index, transform)
	var static_instance := MultiMeshInstance3D.new()
	static_instance.multimesh = static_mesh
	world.add_child(static_instance)

	var library := MeshLibrary.new()
	library.create_item(0)
	library.set_item_mesh(0, box)
	var grid := GridMap.new()
	grid.mesh_library = library
	grid.cell_size = Vector3(1, 1, 1)
	for octant in 3:
		for item in 2:
			grid.set_cell_item(Vector3i(octant * 8 + item, 0, -2), 0)
	world.add_child(grid)

	var dynamic_mesh := MultiMesh.new()
	dynamic_mesh.transform_format = MultiMesh.TRANSFORM_3D
	dynamic_mesh.mesh = box
	dynamic_mesh.instance_count = 4
	var dynamic_instance := MultiMeshInstance3D.new()
	dynamic_instance.multimesh = dynamic_mesh
	dynamic_instance.position = Vector3(0, 2, 0)
	world.add_child(dynamic_instance)
	var particles := GPUParticles3D.new()
	particles.amount = 8
	particles.position = Vector3(1, 1, 1)
	world.add_child(particles)

	var max_tlas := 0
	var dynamic_excluded := 0
	for frame in 20:
		for index in 4:
			var transform := Transform3D()
			transform.origin = Vector3(float(index), 2.0, float(frame) * 0.01)
			dynamic_mesh.set_instance_transform(index, transform)
		await get_tree().process_frame
		var counters := RenderingServer.nextgi_get_counters()
		max_tlas = maxi(max_tlas, int(counters.get("tlas_instances", 0)))
		dynamic_excluded = maxi(dynamic_excluded, int(counters.get("rt_dynamic_excluded", 0)))
	var failures := PackedStringArray()
	if max_tlas < 1006:
		failures.append("tlas %d" % max_tlas)
	if dynamic_excluded < 4:
		failures.append("dynamic %d" % dynamic_excluded)
	return {"pass": failures.is_empty(), "failures": " | ".join(failures), "tlas": max_tlas, "dynamic_excluded": dynamic_excluded}
