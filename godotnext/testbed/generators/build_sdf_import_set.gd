extends RefCounted
## SDF import-set display scene (#181, gate GS-5): all 16 sign-set
## meshes (12 closed, 4 open) in two rows on a ground slab, built from
## the same geometry the OBJ assets in [code]assets/sdf_import_set/[/code]
## carry. Closed meshes are gray; open meshes are orange with culling
## disabled in this scene only, so their inside faces are visible in a
## capture. The OBJ assets themselves carry no material.

const Scaffold := preload("res://generators/scaffold.gd")
const SdfMeshes := preload("res://generators/sdf_mesh_library.gd")

const STREAM := 44
const CAMERAS: Array[String] = ["CovCam0", "CovCam1", "CovCam2", "CovCam3", "CaptureCam"]
const COLUMNS := 8
const SPACING := 3.4
const ROW_Z: Array[float] = [-3.6, 3.6]


static func build() -> Dictionary:
	var root := Node3D.new()
	root.name = "SdfImportSet"
	root.set_meta("generator_stream", STREAM)
	root.set_meta("closed_count", SdfMeshes.CLOSED_NAMES.size())
	root.set_meta("open_count", SdfMeshes.OPEN_NAMES.size())

	var closed_material := Scaffold.make_constant_material("Closed", Color(0.55, 0.6, 0.68))
	var open_material := Scaffold.make_constant_material("Open", Color(0.8, 0.45, 0.2))
	open_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	var ground_material := Scaffold.make_constant_material("Ground", Color(0.5, 0.48, 0.45))

	var sky_material := ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_material
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = environment
	Scaffold.add_owned(root, world_env, root)

	Scaffold.add_box_instance(root, root, "Ground",
			Vector3(0.0, -0.1, 0.0), Vector3(30.0, 0.2, 16.0), ground_material)

	var meshes := SdfMeshes.all_meshes()
	for index in meshes.size():
		var mesh_data: Dictionary = meshes[index]
		var stats := SdfMeshes.analyze(mesh_data)
		var column := index % COLUMNS
		var row := index / COLUMNS
		var x := (float(column) - float(COLUMNS - 1) * 0.5) * SPACING
		var instance := MeshInstance3D.new()
		# Node name is the asset name verbatim so a scene dump lines up
		# with the OBJ files and the sign reference without a mapping.
		instance.name = String(mesh_data["name"])
		instance.mesh = SdfMeshes.build_array_mesh(
				mesh_data, closed_material if bool(mesh_data["closed"]) else open_material)
		var min_y := (stats["aabb_min"] as Vector3).y
		instance.position = Vector3(x, -min_y, ROW_Z[row])
		if String(mesh_data["name"]) == "plane_sheet":
			# Tilt the flat sheet so a capture shows it has no thickness.
			instance.rotation_degrees = Vector3(-55.0, 20.0, 0.0)
			instance.position.y = 0.9
		instance.set_meta("sdf_mesh_name", String(mesh_data["name"]))
		instance.set_meta("sdf_closed", bool(mesh_data["closed"]))
		instance.set_meta("triangles", int(stats["triangles"]))
		Scaffold.add_owned(root, instance, root)
		Scaffold.add_trimesh_collider(root, instance)

	var elevation := deg_to_rad(35.0)
	var azimuth := deg_to_rad(45.0)
	var direction := Vector3(
			-cos(elevation) * sin(azimuth), -sin(elevation), -cos(elevation) * cos(azimuth))
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.transform = Transform3D(Basis.looking_at(direction.normalized(), Vector3.UP), Vector3(12.0, 12.0, 12.0))
	sun.light_energy = 1.0
	sun.shadow_enabled = true
	Scaffold.add_owned(root, sun, root)

	Scaffold.make_camera(root, root, "CovCam0",
			Vector3(-14.0, 9.0, -12.0), Vector3(0.0, 0.5, 0.0), 60.0)
	Scaffold.make_camera(root, root, "CovCam1",
			Vector3(14.0, 9.0, -12.0), Vector3(0.0, 0.5, 0.0), 60.0)
	Scaffold.make_camera(root, root, "CovCam2",
			Vector3(-14.0, 9.0, 12.0), Vector3(0.0, 0.5, 0.0), 60.0)
	Scaffold.make_camera(root, root, "CovCam3",
			Vector3(14.0, 9.0, 12.0), Vector3(0.0, 0.5, 0.0), 60.0)
	Scaffold.make_camera(root, root, "CaptureCam",
			Vector3(0.0, 13.0, 15.0), Vector3(0.0, 0.0, 0.0), 60.0, 60.0, true)

	return {
		"root": root,
		"cameras": CAMERAS,
		"q1_surfaces": [
			Scaffold.make_patch_surface("Ground",
					Vector3(-15.0, 0.0, -8.0), Vector3(30.0, 0.0, 0.0),
					Vector3(0.0, 0.0, 16.0), Vector3.UP, "CaptureCam"),
		],
		"steps": _steps(),
	}


static func _steps() -> Dictionary:
	return {
		"name": "sdf_import_set",
		"variants": [
			{
				"id": "sign_set", "form": "GS-5", "frame0_action": "none (static)",
				"end_frame": 0, "motion_frames": 0, "motion_start": 0, "motion_end": 0,
			},
		],
		"cameras": CAMERAS,
		"patch_set": "q1",
		"seed": Scaffold.stream_seed(STREAM),
		"closed_meshes": SdfMeshes.CLOSED_NAMES,
		"open_meshes": SdfMeshes.OPEN_NAMES,
	}
