extends RefCounted
## NextGI Perf shell (#33). Cameras, sun, 16 local lights and the
## interior-plus-courtyard are saved. The 50k instances are spawned by
## nextgi_perf.gd from seed 1234 and are not part of the packed scene.

const Scaffold := preload("res://generators/scaffold.gd")
const Layout = preload("res://scenes/nextgi_perf/layout.gd")

const STREAM := 35
const CAMERAS: Array[String] = ["CovCam0", "CovCam1", "CovCam2", "CovCam3", "CaptureCam"]


static func build() -> Dictionary:
	var root := Node3D.new()
	root.name = "NextGIPerf"
	root.set_meta("instance_seed", Layout.SEED)
	root.set_meta("default_instances", Layout.DEFAULT_INSTANCES)
	root.set_meta("generator_stream", STREAM)

	var gray := Scaffold.make_constant_material("Gray075", Color(0.75, 0.75, 0.75))
	var interior := Scaffold.make_constant_material("Interior", Color(0.62, 0.6, 0.56))

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

	# Interior x -1..25, z -9..9. Courtyard x 25..105, z -42..42.
	# The lattice (x 1..22 / 28..100) sits on these floors.
	Scaffold.add_box_instance(root, root, "InteriorFloor",
			Vector3(12.0, -0.1, 0.0), Vector3(26.0, 0.2, 18.0), interior)
	Scaffold.add_box_instance(root, root, "InteriorCeiling",
			Vector3(12.0, 4.1, 0.0), Vector3(26.0, 0.2, 18.0), interior)
	Scaffold.add_box_instance(root, root, "InteriorWest",
			Vector3(-1.1, 2.0, 0.0), Vector3(0.2, 4.0, 18.0), interior)
	Scaffold.add_box_instance(root, root, "InteriorNorth",
			Vector3(12.0, 2.0, -9.1), Vector3(26.0, 4.0, 0.2), interior)
	Scaffold.add_box_instance(root, root, "InteriorSouth",
			Vector3(12.0, 2.0, 9.1), Vector3(26.0, 4.0, 0.2), interior)
	Scaffold.add_box_instance(root, root, "InteriorEastNegZ",
			Vector3(25.1, 2.0, -5.5), Vector3(0.2, 4.0, 7.0), interior)
	Scaffold.add_box_instance(root, root, "InteriorEastPosZ",
			Vector3(25.1, 2.0, 5.5), Vector3(0.2, 4.0, 7.0), interior)
	Scaffold.add_box_instance(root, root, "InteriorEastHeader",
			Vector3(25.1, 3.5, 0.0), Vector3(0.2, 1.0, 4.0), interior)

	Scaffold.add_box_instance(root, root, "CourtyardFloor",
			Vector3(65.0, -0.1, 0.0), Vector3(80.0, 0.2, 84.0), gray)
	Scaffold.add_box_instance(root, root, "CourtyardNorth",
			Vector3(65.0, 0.6, -42.1), Vector3(80.0, 1.2, 0.2), gray)
	Scaffold.add_box_instance(root, root, "CourtyardSouth",
			Vector3(65.0, 0.6, 42.1), Vector3(80.0, 1.2, 0.2), gray)
	Scaffold.add_box_instance(root, root, "CourtyardEast",
			Vector3(105.1, 0.6, 0.0), Vector3(0.2, 1.2, 84.0), gray)

	var elevation := deg_to_rad(35.0)
	var azimuth := deg_to_rad(45.0)
	var direction := Vector3(
			-cos(elevation) * sin(azimuth), -sin(elevation), -cos(elevation) * cos(azimuth))
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.transform = Transform3D(Basis.looking_at(direction.normalized(), Vector3.UP), Vector3(40.0, 10.0, 0.0))
	sun.light_energy = 1.0
	sun.shadow_enabled = true
	Scaffold.add_owned(root, sun, root)

	var light_positions: Array[Vector3] = [
		Vector3(6.0, 3.2, -4.0), Vector3(6.0, 3.2, 4.0),
		Vector3(16.0, 3.2, -4.0), Vector3(16.0, 3.2, 4.0),
	]
	for index in 12:
		var angle := TAU * float(index) / 12.0
		light_positions.append(Vector3(62.0 + cos(angle) * 24.0, 3.5, sin(angle) * 24.0))
	for index in light_positions.size():
		var omni := OmniLight3D.new()
		omni.name = "LocalLight%d" % index
		omni.position = light_positions[index]
		omni.light_energy = 0.5
		omni.omni_range = 12.0
		omni.shadow_enabled = false
		Scaffold.add_owned(root, omni, root)

	var spot_pos := Vector3(-4.0, 3.2, 0.0)
	var spot_target := Vector3(20.0, 2.0, 0.0)
	var spot := SpotLight3D.new()
	spot.name = "OffscreenSpot"
	spot.transform = Transform3D(Basis.looking_at((spot_target - spot_pos).normalized(), Vector3.UP), spot_pos)
	spot.light_energy = 1.0
	spot.spot_range = 10.0
	spot.spot_angle = 28.0
	spot.shadow_enabled = false
	Scaffold.add_owned(root, spot, root)

	Scaffold.make_camera(root, root, "CovCam0",
			Vector3(40.0, 3.5, -15.0), Vector3(70.0, 2.0, -5.0), 80.0)
	Scaffold.make_camera(root, root, "CovCam1",
			Vector3(40.0, 3.5, 15.0), Vector3(70.0, 2.0, 5.0), 80.0)
	Scaffold.make_camera(root, root, "CovCam2",
			Vector3(60.0, 4.0, -20.0), Vector3(90.0, 2.0, 0.0), 80.0)
	Scaffold.make_camera(root, root, "CovCam3",
			Vector3(60.0, 4.0, 20.0), Vector3(90.0, 2.0, 0.0), 80.0)
	var capture := Scaffold.make_camera(root, root, "CaptureCam",
			Layout.spline_position(0), Layout.spline_position(1), 80.0, 60.0, true)
	capture.transform = Transform3D(Layout.spline_basis(0), Layout.spline_position(0))

	root.set_script(load("res://scenes/nextgi_perf/nextgi_perf.gd"))
	root.set_meta("skip_runtime", true)
	return {
		"root": root,
		"cameras": CAMERAS,
		"q1_surfaces": _q1_surfaces(),
		"q1_rng_seed": Layout.SEED,
		"offscreen_light_node": "OffscreenSpot",
		"steps": _steps(),
	}


static func _q1_surfaces() -> Array:
	var surfaces: Array = []
	surfaces.append(Scaffold.make_patch_surface("InteriorFloor",
			Vector3(0.0, 0.0, -8.0), Vector3(24.0, 0.0, 0.0), Vector3(0.0, 0.0, 16.0),
			Vector3.UP, "CaptureCam"))
	surfaces.append(Scaffold.make_patch_surface("CourtyardFloor",
			Vector3(26.0, 0.0, -40.0), Vector3(78.0, 0.0, 0.0), Vector3(0.0, 0.0, 80.0),
			Vector3.UP, "CovCam0"))
	return surfaces


static func _steps() -> Dictionary:
	return {
		"name": "nextgi_perf",
		"variants": [
			{
				"id": "timing", "form": "timing", "frame0_action": "camera spline",
				"end_frame": Layout.SPLINE_FRAMES - 1, "motion_frames": Layout.SPLINE_FRAMES,
				"motion_start": 0, "motion_end": Layout.SPLINE_FRAMES,
			},
			{
				"id": "p3", "form": "timing", "frame0_action": "4 skinned characters follow the spline",
				"end_frame": Layout.SPLINE_FRAMES - 1, "motion_frames": Layout.SPLINE_FRAMES,
				"motion_start": 0, "motion_end": Layout.SPLINE_FRAMES,
			},
		],
		"cameras": CAMERAS,
		"patch_set": "q1",
		"seed": Layout.SEED,
	}
