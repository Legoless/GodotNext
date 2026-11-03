extends RefCounted
## Overlap-heavy kitbash shell (#181, gate GS-9). Ground, sun, sky and
## cameras are saved; the 2,400 overlapping rock instances are spawned
## at runtime by swrt_kitbash.gd from the seeded layout and are not
## part of the packed scene.

const Scaffold := preload("res://generators/scaffold.gd")
const Layout = preload("res://scenes/swrt_kitbash/layout.gd")

const STREAM := 42
const CAMERAS: Array[String] = ["CovCam0", "CovCam1", "CovCam2", "CovCam3", "CaptureCam"]


static func build() -> Dictionary:
	var root := Node3D.new()
	root.name = "SwrtKitbash"
	root.set_meta("instance_seed", Layout.SEED)
	root.set_meta("default_instances", Layout.DEFAULT_INSTANCES)
	root.set_meta("generator_stream", STREAM)

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
			Vector3(0.0, -0.1, 0.0), Vector3(70.0, 0.2, 70.0), ground_material)

	var elevation := deg_to_rad(35.0)
	var azimuth := deg_to_rad(45.0)
	var direction := Vector3(
			-cos(elevation) * sin(azimuth), -sin(elevation), -cos(elevation) * cos(azimuth))
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.transform = Transform3D(Basis.looking_at(direction.normalized(), Vector3.UP), Vector3(20.0, 15.0, 20.0))
	sun.light_energy = 1.0
	sun.shadow_enabled = true
	Scaffold.add_owned(root, sun, root)

	# Overhead spot: every camera looks down at the piles, so a light far
	# above the field stays outside all frustums (build_all verifies).
	var spot_pos := Vector3(0.0, 38.0, 0.0)
	var spot_target := Vector3(0.0, 1.0, 0.0)
	var spot := SpotLight3D.new()
	spot.name = "OffscreenSpot"
	spot.transform = Transform3D(Basis.looking_at((spot_target - spot_pos).normalized(), Vector3(1.0, 0.0, 0.0)), spot_pos)
	spot.light_energy = 1.0
	spot.spot_range = 50.0
	spot.spot_angle = 25.0
	spot.shadow_enabled = false
	Scaffold.add_owned(root, spot, root)

	Scaffold.make_camera(root, root, "CovCam0",
			Vector3(22.0, 10.0, -22.0), Vector3(0.0, 1.0, 0.0), 90.0)
	Scaffold.make_camera(root, root, "CovCam1",
			Vector3(-22.0, 10.0, 22.0), Vector3(0.0, 1.0, 0.0), 90.0)
	Scaffold.make_camera(root, root, "CovCam2",
			Vector3(-22.0, 14.0, -22.0), Vector3(0.0, 0.5, 0.0), 90.0)
	Scaffold.make_camera(root, root, "CovCam3",
			Vector3(0.0, 26.0, 10.0), Vector3(0.0, 0.0, 0.0), 90.0)
	Scaffold.make_camera(root, root, "CaptureCam",
			Vector3(19.0, 7.0, 19.0), Vector3(0.0, 1.2, 0.0), 90.0, 60.0, true)

	root.set_script(load("res://scenes/swrt_kitbash/swrt_kitbash.gd"))
	root.set_meta("skip_runtime", true)
	return {
		"root": root,
		"cameras": CAMERAS,
		"q1_surfaces": [
			Scaffold.make_patch_surface("Ground",
					Vector3(-34.0, 0.0, -34.0), Vector3(68.0, 0.0, 0.0),
					Vector3(0.0, 0.0, 68.0), Vector3.UP, "CovCam3"),
		],
		"q1_rng_seed": Layout.SEED,
		"offscreen_light_node": "OffscreenSpot",
		"steps": _steps(),
	}


static func _steps() -> Dictionary:
	return {
		"name": "swrt_kitbash",
		"variants": [
			{
				"id": "overlap", "form": "GS-9", "frame0_action": "none (static piles)",
				"end_frame": 0, "motion_frames": 0, "motion_start": 0, "motion_end": 0,
			},
		],
		"cameras": CAMERAS,
		"patch_set": "q1",
		"seed": Layout.SEED,
		"instances_default": Layout.DEFAULT_INSTANCES,
	}
