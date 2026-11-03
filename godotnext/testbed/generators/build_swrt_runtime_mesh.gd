extends RefCounted
## Runtime procedural mesh shell (#181, gate GS-6): a sealed
## 10.0 x 3.5 x 8.0 m room with one omni light and two floor markers
## where the mesh appears (A) and lands (B). The mesh itself is built
## at runtime by swrt_runtime_mesh.gd and is not part of the packed
## scene, which is the whole point of the scene.

const Scaffold := preload("res://generators/scaffold.gd")
const Spec = preload("res://scenes/swrt_runtime_mesh/spec.gd")

const STREAM := 43
const ROOM_SIZE := Vector3(10.0, 3.5, 8.0)
const WALL := 0.20
const CAMERAS: Array[String] = ["CovCam0", "CovCam1", "CovCam2", "CovCam3", "CaptureCam"]


static func build() -> Dictionary:
	var half_x := ROOM_SIZE.x * 0.5
	var half_z := ROOM_SIZE.z * 0.5
	var height := ROOM_SIZE.y
	var wall_span_y := height + 2.0 * WALL
	var outer_x := ROOM_SIZE.x + 2.0 * WALL
	var outer_z := ROOM_SIZE.z + 2.0 * WALL

	var root := Node3D.new()
	root.name = "SwrtRuntimeMesh"
	root.set_meta("room_interior_m", ROOM_SIZE)
	root.set_meta("generator_stream", STREAM)

	var gray := Scaffold.make_constant_material("Gray075", Color(0.75, 0.75, 0.75))
	var marker_a := Scaffold.make_constant_material("MarkerA", Color(0.75, 0.2, 0.2))
	var marker_b := Scaffold.make_constant_material("MarkerB", Color(0.2, 0.2, 0.75))

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0, 0.0, 0.0)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = environment
	Scaffold.add_owned(root, world_env, root)

	Scaffold.add_box_instance(root, root, "Floor",
			Vector3(0.0, -WALL * 0.5, 0.0), Vector3(outer_x, WALL, outer_z), gray)
	Scaffold.add_box_instance(root, root, "Ceiling",
			Vector3(0.0, height + WALL * 0.5, 0.0), Vector3(outer_x, WALL, outer_z), gray)
	Scaffold.add_box_instance(root, root, "WestWall",
			Vector3(-half_x - WALL * 0.5, height * 0.5, 0.0), Vector3(WALL, wall_span_y, outer_z), gray)
	Scaffold.add_box_instance(root, root, "EastWall",
			Vector3(half_x + WALL * 0.5, height * 0.5, 0.0), Vector3(WALL, wall_span_y, outer_z), gray)
	Scaffold.add_box_instance(root, root, "NorthWall",
			Vector3(0.0, height * 0.5, -half_z - WALL * 0.5), Vector3(outer_x, wall_span_y, WALL), gray)
	Scaffold.add_box_instance(root, root, "SouthWall",
			Vector3(0.0, height * 0.5, half_z + WALL * 0.5), Vector3(outer_x, wall_span_y, WALL), gray)

	# Flat floor markers under the two mesh positions, no colliders.
	Scaffold.add_box_instance(root, root, "MarkerA",
			Vector3(Spec.POSITION_A.x, 0.01, Spec.POSITION_A.z), Vector3(2.2, 0.02, 2.2), marker_a, false)
	Scaffold.add_box_instance(root, root, "MarkerB",
			Vector3(Spec.POSITION_B.x, 0.01, Spec.POSITION_B.z), Vector3(2.2, 0.02, 2.2), marker_b, false)

	var omni := OmniLight3D.new()
	omni.name = "RoomLight"
	omni.position = Vector3(0.0, 3.0, 0.0)
	omni.light_energy = 1.0
	omni.omni_range = 9.0
	omni.shadow_enabled = true
	Scaffold.add_owned(root, omni, root)

	Scaffold.make_camera(root, root, "CovCam0",
			Vector3(-half_x + 0.5, 2.8, -half_z + 0.5), Vector3(0.0, 1.0, 0.0), 20.0)
	Scaffold.make_camera(root, root, "CovCam1",
			Vector3(-half_x + 0.5, 2.8, half_z - 0.5), Vector3(0.0, 1.0, 0.0), 20.0)
	Scaffold.make_camera(root, root, "CovCam2",
			Vector3(half_x - 0.5, 2.8, -half_z + 0.5), Vector3(0.0, 1.0, 0.0), 20.0)
	Scaffold.make_camera(root, root, "CovCam3",
			Vector3(half_x - 0.5, 2.8, half_z - 0.5), Vector3(0.0, 1.0, 0.0), 20.0)
	Scaffold.make_camera(root, root, "CaptureCam",
			Vector3(0.0, 1.8, half_z - 0.7), Vector3(0.0, 1.1, 0.0), 20.0, 70.0, true)

	root.set_script(load("res://scenes/swrt_runtime_mesh/swrt_runtime_mesh.gd"))
	root.set_meta("skip_runtime", true)
	return {
		"root": root,
		"cameras": CAMERAS,
		"q1_surfaces": _q1_surfaces(half_x, half_z, height),
		"steps": _steps(),
	}


static func _q1_surfaces(p_half_x: float, p_half_z: float, p_height: float) -> Array:
	var surfaces: Array = []
	var size_x := p_half_x * 2.0
	var size_z := p_half_z * 2.0
	surfaces.append(Scaffold.make_patch_surface("Floor",
			Vector3(-p_half_x, 0.0, -p_half_z), Vector3(size_x, 0.0, 0.0),
			Vector3(0.0, 0.0, size_z), Vector3.UP, "CovCam0"))
	surfaces.append(Scaffold.make_patch_surface("Ceiling",
			Vector3(-p_half_x, p_height, -p_half_z), Vector3(size_x, 0.0, 0.0),
			Vector3(0.0, 0.0, size_z), Vector3.DOWN, "CovCam0"))
	surfaces.append(Scaffold.make_patch_surface("NorthWall",
			Vector3(-p_half_x, 0.0, -p_half_z), Vector3(size_x, 0.0, 0.0),
			Vector3(0.0, p_height, 0.0), Vector3(0.0, 0.0, 1.0), "CaptureCam"))
	surfaces.append(Scaffold.make_patch_surface("SouthWall",
			Vector3(-p_half_x, 0.0, p_half_z), Vector3(size_x, 0.0, 0.0),
			Vector3(0.0, p_height, 0.0), Vector3(0.0, 0.0, -1.0), "CovCam0"))
	surfaces.append(Scaffold.make_patch_surface("WestWall",
			Vector3(-p_half_x, 0.0, -p_half_z), Vector3(0.0, 0.0, size_z),
			Vector3(0.0, p_height, 0.0), Vector3.RIGHT, "CovCam2"))
	surfaces.append(Scaffold.make_patch_surface("EastWall",
			Vector3(p_half_x, 0.0, -p_half_z), Vector3(0.0, 0.0, size_z),
			Vector3(0.0, p_height, 0.0), Vector3.LEFT, "CovCam0"))
	return surfaces


static func _steps() -> Dictionary:
	return {
		"name": "swrt_runtime_mesh",
		"variants": [
			{
				"id": "runtime_mesh", "form": "GS-6",
				"frame0_action": "mesh built at frame 60, moved at frame 180",
				"end_frame": Spec.END_FRAME, "motion_frames": 0,
				"motion_start": Spec.BUILD_FRAME, "motion_end": Spec.MOVE_FRAME,
				"build_frame": Spec.BUILD_FRAME, "move_frame": Spec.MOVE_FRAME,
				"triangles": Spec.triangle_count(),
			},
		],
		"cameras": CAMERAS,
		"patch_set": "q1",
		"seed": Scaffold.stream_seed(STREAM),
	}
