extends Node3D
## Mesh SDF runtime-generation check (#185). Builds a 20k-triangle ArrayMesh
## at runtime (a 100x100 quad grid), adds it to the scene, and after 40
## frames prints the atlas counters. Run with NextGI enabled and Metal
## validation; the SDF builder runs on WorkerThreadPool and the mesh should
## become resident in the brick pool.

const GRID := 100
const SIZE_M := 10.0

var _frames := 0


func _ready() -> void:
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var uvs := PackedVector2Array()
	var indices := PackedInt32Array()
	for z in range(GRID + 1):
		for x in range(GRID + 1):
			var v := Vector3(float(x) / float(GRID) * SIZE_M - SIZE_M * 0.5, sin(float(x) / float(GRID) * TAU) * 0.4, float(z) / float(GRID) * SIZE_M - SIZE_M * 0.5)
			vertices.push_back(v)
			normals.push_back(Vector3.UP)
			uvs.push_back(Vector2(float(x) / float(GRID), float(z) / float(GRID)))
	for z in range(GRID):
		for x in range(GRID):
			var i := z * (GRID + 1) + x
			indices.push_back(i)
			indices.push_back(i + GRID + 1)
			indices.push_back(i + 1)
			indices.push_back(i + 1)
			indices.push_back(i + GRID + 1)
			indices.push_back(i + GRID + 2)
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.6, 0.55, 0.5)
	material.roughness = 0.9
	mesh.surface_set_material(0, material)
	var instance := MeshInstance3D.new()
	instance.name = "RuntimeMesh"
	instance.mesh = mesh
	add_child(instance)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 120.0, 120.0)
	camera.rotation_degrees = Vector3(-30.0, 0.0, 0.0)
	camera.current = true
	add_child(camera)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50.0, -30.0, 0.0)
	add_child(sun)
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.nextgi_enabled = true
	environment.sdfgi_enabled = false
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)
	print("mesh_sdf_runtime: mesh triangles=%d" % (indices.size() / 3))


func _process(_delta: float) -> void:
	_frames += 1
	if _frames == 300:
		var info: Dictionary = RenderingServer.nextgi_get_tier_info()
		print("MESH_SDF_CHECK resident=%s used=%s capacity=%s jobs=%s pending=%s sdf_instances=%s unsupported=%s no_field=%s" % [
			info.get("mesh_sdf_resident_bytes", -1),
			info.get("mesh_sdf_pool_used", -1),
			info.get("mesh_sdf_pool_capacity", -1),
			info.get("mesh_sdf_jobs", -1),
			info.get("mesh_sdf_pending", -1),
			info.get("sdf_instance_count", -1),
			info.get("sdf_unsupported_deformation", -1),
			info.get("sdf_no_field", -1),
		])
		get_tree().quit(0)
