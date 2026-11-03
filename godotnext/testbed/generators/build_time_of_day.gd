extends RefCounted
## Time of Day (#33). A 16 m courtyard opens into a 4 m interior on +Z.
## The sun's forward direction at azimuth 180° points into that interior.
## One spot sits behind every camera.

const Scaffold := preload("res://generators/scaffold.gd")
const Motion = preload("res://scenes/time_of_day/motion.gd")

const STREAM := 34
const CAMERAS: Array[String] = ["CovCam0", "CovCam1", "CovCam2", "CovCam3", "CaptureCam"]


static func build() -> Dictionary:
	var root := Node3D.new()
	root.name = "TimeOfDay"
	var gray := Scaffold.make_constant_material("Gray075", Color(0.75, 0.75, 0.75))
	var interior := Scaffold.make_constant_material("Interior", Color(0.55, 0.5, 0.42))

	var sky_material := ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_material
	# Automatic mode picks incremental radiance for a lit procedural sky, so a
	# moving sun leaves the cubemap a few frames behind. Realtime rebuilds it
	# from the current light, which keeps --hold-frame equal to that frame.
	sky.process_mode = Sky.PROCESS_MODE_REALTIME
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = environment
	Scaffold.add_owned(root, world_env, root)

	# Courtyard floor and walls. The south wall (inner face z = 8) has a
	# 2 m x 2.2 m opening into the interior.
	Scaffold.add_box_instance(root, root, "CourtyardFloor",
			Vector3(0.0, -0.1, 0.0), Vector3(16.4, 0.2, 16.4), gray)
	Scaffold.add_box_instance(root, root, "NorthWall",
			Vector3(0.0, 2.0, -8.1), Vector3(16.4, 4.0, 0.2), gray)
	Scaffold.add_box_instance(root, root, "WestWall",
			Vector3(-8.1, 2.0, 0.0), Vector3(0.2, 4.0, 16.0), gray)
	Scaffold.add_box_instance(root, root, "EastWall",
			Vector3(8.1, 2.0, 0.0), Vector3(0.2, 4.0, 16.0), gray)
	Scaffold.add_box_instance(root, root, "SouthWallNegX",
			Vector3(-4.6, 2.0, 8.1), Vector3(7.2, 4.0, 0.2), gray)
	Scaffold.add_box_instance(root, root, "SouthWallPosX",
			Vector3(4.6, 2.0, 8.1), Vector3(7.2, 4.0, 0.2), gray)
	Scaffold.add_box_instance(root, root, "SouthWallHeader",
			Vector3(0.0, 3.1, 8.1), Vector3(2.0, 1.8, 0.2), gray)

	Scaffold.add_box_instance(root, root, "InteriorFloor",
			Vector3(0.0, -0.1, 10.2), Vector3(4.4, 0.2, 4.0), interior)
	Scaffold.add_box_instance(root, root, "InteriorCeiling",
			Vector3(0.0, 3.1, 10.2), Vector3(4.4, 0.2, 4.0), interior)
	Scaffold.add_box_instance(root, root, "InteriorWest",
			Vector3(-2.1, 1.5, 10.2), Vector3(0.2, 3.0, 4.0), interior)
	Scaffold.add_box_instance(root, root, "InteriorEast",
			Vector3(2.1, 1.5, 10.2), Vector3(0.2, 3.0, 4.0), interior)
	Scaffold.add_box_instance(root, root, "InteriorSouth",
			Vector3(0.0, 1.5, 12.3), Vector3(4.4, 3.0, 0.2), interior)

	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	var sun_pos := Vector3(0.0, 6.0, -8.0)
	sun.transform = Transform3D(Motion.sun_basis(Motion.BASE_ELEVATION_DEG), sun_pos)
	sun.light_energy = 1.0
	sun.shadow_enabled = true
	Scaffold.add_owned(root, sun, root)

	var spot_pos := Vector3(0.0, 1.8, -6.6)
	var spot_target := Vector3(0.0, 1.2, 9.0)
	var spot := SpotLight3D.new()
	spot.name = "OffscreenSpot"
	spot.transform = Transform3D(Basis.looking_at((spot_target - spot_pos).normalized(), Vector3.UP), spot_pos)
	spot.light_energy = 1.0
	spot.spot_range = 8.0
	spot.spot_angle = 30.0
	spot.shadow_enabled = true
	Scaffold.add_owned(root, spot, root)

	Scaffold.make_camera(root, root, "CovCam0",
			Vector3(-5.0, 2.2, -4.0), Vector3(0.0, 1.2, 8.0), 30.0)
	Scaffold.make_camera(root, root, "CovCam1",
			Vector3(5.0, 2.2, -4.0), Vector3(0.0, 1.2, 8.0), 30.0)
	Scaffold.make_camera(root, root, "CovCam2",
			Vector3(-5.0, 2.2, 2.0), Vector3(0.0, 1.5, 10.0), 30.0)
	Scaffold.make_camera(root, root, "CovCam3",
			Vector3(5.0, 2.2, 2.0), Vector3(0.0, 1.5, 10.0), 30.0)
	Scaffold.make_camera(root, root, "CaptureCam",
			Vector3(0.0, 1.8, -5.0), Vector3(0.0, 1.2, 9.0), 30.0, 60.0, true)

	root.set_script(load("res://scenes/time_of_day/time_of_day.gd"))
	root.set_meta("skip_runtime", true)
	return {
		"root": root,
		"cameras": CAMERAS,
		"q1_surfaces": _q1_surfaces(),
		"offscreen_light_node": "OffscreenSpot",
		"steps": _steps(),
	}


static func _q1_surfaces() -> Array:
	var surfaces: Array = []
	surfaces.append(Scaffold.make_patch_surface("CourtyardFloor",
			Vector3(-8.0, 0.0, -8.0), Vector3(16.0, 0.0, 0.0), Vector3(0.0, 0.0, 16.0),
			Vector3.UP, "CovCam0"))
	surfaces.append(Scaffold.make_patch_surface("InteriorFloor",
			Vector3(-2.0, 0.0, 8.2), Vector3(4.0, 0.0, 0.0), Vector3(0.0, 0.0, 4.0),
			Vector3.UP, "CovCam2"))
	surfaces.append(Scaffold.make_patch_surface("NorthWall",
			Vector3(-8.0, 0.0, -8.0), Vector3(16.0, 0.0, 0.0), Vector3(0.0, 4.0, 0.0),
			Vector3(0.0, 0.0, 1.0), "CovCam0"))
	surfaces.append(Scaffold.make_patch_surface("InteriorSouth",
			Vector3(-2.0, 0.0, 12.2), Vector3(4.0, 0.0, 0.0), Vector3(0.0, 3.0, 0.0),
			Vector3(0.0, 0.0, -1.0), "CaptureCam"))
	return surfaces


static func _steps() -> Dictionary:
	return {
		"name": "time_of_day",
		"variants": [
			{
				"id": "sweep", "form": "Q4b", "frame0_action": "sun elevation starts at base",
				"end_frame": Motion.SWEEP_END_FRAME, "motion_frames": Motion.SWEEP_END_FRAME,
				"motion_start": 0, "motion_end": Motion.SWEEP_END_FRAME,
			},
			{
				"id": "sun_jump", "form": "Q4a", "frame0_action": "sun elevation +15 deg",
				"end_frame": 240, "motion_frames": 0, "motion_start": 0, "motion_end": 0,
			},
		],
		"cameras": CAMERAS,
		"patch_set": "q1",
		"seed": Scaffold.stream_seed(STREAM),
	}
