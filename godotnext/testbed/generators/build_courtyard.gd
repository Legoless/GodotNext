extends RefCounted
## Concave Courtyard generator (#10 step 6): one [ArrayMesh] from
## [SurfaceTool] in one [MeshInstance3D]. A 16 x 16 m footprint around an
## open 8 x 8 m courtyard, 6 m walls 0.3 m thick, a roof over the building
## ring and a roofed 2 m colonnade around the courtyard at 3 m height.

const Scaffold := preload("res://generators/scaffold.gd")

const WALL := 0.3
const WALL_HEIGHT := 6.0
const CAMERAS: Array[String] = ["CovCam0", "CovCam1", "CovCam2", "CovCam3", "CaptureCam"]


static func build() -> Dictionary:
	var root := Node3D.new()
	root.name = "ConcaveCourtyard"
	root.set_meta("wall_thickness_m", WALL)

	var gray := Scaffold.make_constant_material("Grey075", Color(0.75, 0.75, 0.75))

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

	var builder = Scaffold.ChunkedSurfaceBuilder.new(gray)
	# Floor slab over the whole footprint.
	builder.add_box(Vector3(0.0, -0.15, 0.0), Vector3(16.0, WALL, 16.0))
	# Outer walls.
	builder.add_box(Vector3(0.0, 3.0, -7.85), Vector3(16.0, WALL_HEIGHT, WALL))
	builder.add_box(Vector3(0.0, 3.0, 7.85), Vector3(16.0, WALL_HEIGHT, WALL))
	builder.add_box(Vector3(-7.85, 3.0, 0.0), Vector3(WALL, WALL_HEIGHT, 15.4))
	builder.add_box(Vector3(7.85, 3.0, 0.0), Vector3(WALL, WALL_HEIGHT, 15.4))
	# Inner courtyard walls (faces at +/-4 m).
	builder.add_box(Vector3(0.0, 3.0, -4.15), Vector3(8.6, WALL_HEIGHT, WALL))
	builder.add_box(Vector3(0.0, 3.0, 4.15), Vector3(8.6, WALL_HEIGHT, WALL))
	builder.add_box(Vector3(-4.15, 3.0, 0.0), Vector3(WALL, WALL_HEIGHT, 8.0))
	builder.add_box(Vector3(4.15, 3.0, 0.0), Vector3(WALL, WALL_HEIGHT, 8.0))
	# Roof slabs over the building ring at 6 m.
	builder.add_box(Vector3(0.0, 6.15, -6.0), Vector3(16.0, WALL, 4.0))
	builder.add_box(Vector3(0.0, 6.15, 6.0), Vector3(16.0, WALL, 4.0))
	builder.add_box(Vector3(-6.0, 6.15, 0.0), Vector3(4.0, WALL, 8.0))
	builder.add_box(Vector3(6.0, 6.15, 0.0), Vector3(4.0, WALL, 8.0))
	# Colonnade roofs: 2 m deep slabs at 3 m, from each inner wall face
	# into the courtyard, on 0.3 m columns along their inner edge.
	builder.add_box(Vector3(0.0, 3.15, -3.0), Vector3(8.0, WALL, 2.0))
	builder.add_box(Vector3(0.0, 3.15, 3.0), Vector3(8.0, WALL, 2.0))
	builder.add_box(Vector3(-3.0, 3.15, 0.0), Vector3(2.0, WALL, 8.0))
	builder.add_box(Vector3(3.0, 3.15, 0.0), Vector3(2.0, WALL, 8.0))
	for offset in [-3.0, -1.0, 1.0, 3.0]:
		builder.add_box(Vector3(offset, 1.5, -2.15), Vector3(WALL, 3.0, WALL))
		builder.add_box(Vector3(offset, 1.5, 2.15), Vector3(WALL, 3.0, WALL))
		builder.add_box(Vector3(-2.15, 1.5, offset), Vector3(WALL, 3.0, WALL))
		builder.add_box(Vector3(2.15, 1.5, offset), Vector3(WALL, 3.0, WALL))

	var instance := MeshInstance3D.new()
	instance.name = "CourtyardMesh"
	instance.mesh = builder.finish()
	Scaffold.add_owned(root, instance, root)
	Scaffold.add_trimesh_collider(root, instance)

	# Sun at 35 degrees elevation, 45 degrees azimuth.
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	var elevation := deg_to_rad(35.0)
	var azimuth := deg_to_rad(45.0)
	var direction := Vector3(
			-cos(elevation) * sin(azimuth), -sin(elevation), -cos(elevation) * cos(azimuth))
	sun.transform = Transform3D(Basis.looking_at(direction.normalized(), Vector3.UP), Vector3.ZERO)
	sun.light_energy = 1.0
	sun.shadow_enabled = true
	Scaffold.add_owned(root, sun, root)

	Scaffold.make_camera(root, root, "CovCam0",
			Vector3(-3.5, 2.5, -3.5), Vector3(0.0, 1.0, 0.0), 60.0)
	Scaffold.make_camera(root, root, "CovCam1",
			Vector3(3.5, 2.5, -3.5), Vector3(0.0, 1.0, 0.0), 60.0)
	Scaffold.make_camera(root, root, "CovCam2",
			Vector3(3.5, 2.5, 3.5), Vector3(0.0, 1.0, 0.0), 60.0)
	Scaffold.make_camera(root, root, "CovCam3",
			Vector3(-3.5, 2.5, 3.5), Vector3(0.0, 1.0, 0.0), 60.0)
	Scaffold.make_camera(root, root, "CaptureCam",
			Vector3(-10.0, 10.0, -10.0), Vector3(0.0, 0.0, 0.0), 100.0, 60.0, true)

	return {
		"root": root,
		"cameras": CAMERAS,
		"q1_surfaces": _q1_surfaces(),
		"mesh_node": "CourtyardMesh",
	}


static func _q1_surfaces() -> Array:
	var surfaces: Array = []
	# Courtyard floor (inside the colonnade columns) and the four inner
	# wall faces. Cameras sit in the courtyard corners.
	surfaces.append(Scaffold.make_patch_surface("CourtyardMesh",
			Vector3(-4.0, 0.0, -4.0), Vector3(8.0, 0.0, 0.0), Vector3(0.0, 0.0, 8.0),
			Vector3.UP, "CovCam0"))
	surfaces.append(Scaffold.make_patch_surface("CourtyardMesh",
			Vector3(-4.0, 0.0, -4.0), Vector3(8.0, 0.0, 0.0), Vector3(0.0, WALL_HEIGHT, 0.0),
			Vector3(0.0, 0.0, 1.0), "CovCam2"))
	surfaces.append(Scaffold.make_patch_surface("CourtyardMesh",
			Vector3(-4.0, 0.0, 4.0), Vector3(8.0, 0.0, 0.0), Vector3(0.0, WALL_HEIGHT, 0.0),
			Vector3(0.0, 0.0, -1.0), "CovCam0"))
	surfaces.append(Scaffold.make_patch_surface("CourtyardMesh",
			Vector3(-4.0, 0.0, -4.0), Vector3(0.0, 0.0, 8.0), Vector3(0.0, WALL_HEIGHT, 0.0),
			Vector3.RIGHT, "CovCam1"))
	surfaces.append(Scaffold.make_patch_surface("CourtyardMesh",
			Vector3(4.0, 0.0, -4.0), Vector3(0.0, 0.0, 8.0), Vector3(0.0, WALL_HEIGHT, 0.0),
			Vector3.LEFT, "CovCam3"))
	# Colonnade roof undersides (normal down) add concave coverage.
	surfaces.append(Scaffold.make_patch_surface("CourtyardMesh",
			Vector3(-4.0, 3.0, -4.0), Vector3(8.0, 0.0, 0.0), Vector3(0.0, 0.0, 2.0),
			Vector3.DOWN, "CovCam0"))
	surfaces.append(Scaffold.make_patch_surface("CourtyardMesh",
			Vector3(-4.0, 3.0, 2.0), Vector3(8.0, 0.0, 0.0), Vector3(0.0, 0.0, 2.0),
			Vector3.DOWN, "CovCam2"))
	return surfaces
