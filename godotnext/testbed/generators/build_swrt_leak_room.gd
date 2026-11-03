extends RefCounted
## SWRT leak room (#181, gates GS-1): one sealed 5.0 x 3.0 x 5.0 m room
## whose every wall is the variant thickness (0.10 m or 0.20 m), a sun
## outside on an open ground slab, and one emissive panel inside as the
## only interior source. Unlike the Color Box Room pair, the leak tested
## here runs from the sunlit exterior into a sealed interior, which is
## the SWRT thin-wall case. Ambient and reflected light are disabled so
## interior light can only come from the emissive panel or through a
## wall. The D16 reference renders for GS-1 run in PQ (§22).

const Scaffold := preload("res://generators/scaffold.gd")

const STREAM_10CM := 40
const STREAM_20CM := 41
const ROOM_SIZE := Vector3(5.0, 3.0, 5.0)
const CAMERAS: Array[String] = [
	"CovCam0", "CovCam1", "CovCam2", "CovCam3", "CaptureCam", "ExteriorCam",
]


static func stream_for(p_variant: String) -> int:
	return STREAM_10CM if p_variant == "10cm" else STREAM_20CM


static func wall_thickness_m(p_variant: String) -> float:
	return 0.10 if p_variant == "10cm" else 0.20


static func build(p_variant: String) -> Dictionary:
	var wall := wall_thickness_m(p_variant)
	var room := ROOM_SIZE
	var outer := room + Vector3(wall, wall, wall) * 2.0
	var half_x := room.x * 0.5
	var half_z := room.z * 0.5
	var center_y := room.y * 0.5
	var wall_span_y := room.y + 2.0 * wall

	var root := Node3D.new()
	root.name = "SwrtLeakRoom" + p_variant.capitalize()
	root.set_meta("wall_thickness_m", wall)
	root.set_meta("room_interior_m", room)
	root.set_meta("generator_stream", stream_for(p_variant))

	var gray := Scaffold.make_constant_material("Gray075", Color(0.75, 0.75, 0.75))
	var red := Scaffold.make_constant_material("RedWall", Color(0.75, 0.05, 0.05))
	var green := Scaffold.make_constant_material("GreenWall", Color(0.05, 0.75, 0.05))
	var emissive := Scaffold.make_constant_material(
			"EmissivePanel", Color(0.9, 0.9, 0.9), 1.0, 0.0, 0.0,
			Color(1.0, 0.95, 0.85), 4.0)
	var ground_material := Scaffold.make_constant_material("Ground", Color(0.45, 0.45, 0.45))

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0, 0.0, 0.0)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = environment
	Scaffold.add_owned(root, world_env, root)

	# Six slabs forming a sealed shell; corners overlap so there is no gap.
	Scaffold.add_box_instance(root, root, "Floor",
			Vector3(0.0, -wall * 0.5, 0.0), Vector3(outer.x, wall, outer.z), gray)
	Scaffold.add_box_instance(root, root, "Ceiling",
			Vector3(0.0, room.y + wall * 0.5, 0.0), Vector3(outer.x, wall, outer.z), gray)
	Scaffold.add_box_instance(root, root, "WestWall",
			Vector3(-half_x - wall * 0.5, center_y, 0.0), Vector3(wall, wall_span_y, outer.z), gray)
	Scaffold.add_box_instance(root, root, "EastWall",
			Vector3(half_x + wall * 0.5, center_y, 0.0), Vector3(wall, wall_span_y, outer.z), gray)
	Scaffold.add_box_instance(root, root, "NorthWall",
			Vector3(0.0, center_y, -half_z - wall * 0.5), Vector3(outer.x, wall_span_y, wall), red)
	Scaffold.add_box_instance(root, root, "SouthWall",
			Vector3(0.0, center_y, half_z + wall * 0.5), Vector3(outer.x, wall_span_y, wall), green)

	# Ground slab the room sits on, top face at y = -wall so the sun has a
	# lit exterior surface and the room floor slab stays the only interior
	# floor. No collider: it is outside every patch surface.
	Scaffold.add_box_instance(root, root, "Ground",
			Vector3(0.0, -wall - 0.1, 0.0), Vector3(24.0, 0.2, 24.0), ground_material, false)

	Scaffold.add_box_instance(root, root, "BlockLarge",
			Vector3(-0.6, 0.6, -0.5), Vector3(1.2, 1.2, 1.2), gray)
	Scaffold.add_box_instance(root, root, "BlockSmall",
			Vector3(0.7, 0.3, 0.6), Vector3(0.6, 0.6, 0.6), gray)

	# The only interior source: an emissive ceiling panel, no collider so
	# patch line-of-sight rays treat it as the light it is, not a blocker.
	var panel := Scaffold.add_box_instance(root, root, "EmissivePanel",
			Vector3(0.0, room.y - 0.03, 0.0), Vector3(1.2, 0.06, 1.2), emissive, false)
	panel.set_meta("emission_energy_multiplier", 4.0)

	# Sun outside, 35 degrees elevation, in no interior camera's way: it
	# lights only the shell exterior and the ground.
	var elevation := deg_to_rad(35.0)
	var azimuth := deg_to_rad(45.0)
	var direction := Vector3(
			-cos(elevation) * sin(azimuth), -sin(elevation), -cos(elevation) * cos(azimuth))
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.transform = Transform3D(Basis.looking_at(direction.normalized(), Vector3.UP), Vector3(8.0, 10.0, 8.0))
	sun.light_energy = 1.0
	sun.shadow_enabled = true
	Scaffold.add_owned(root, sun, root)

	# Interior coverage cameras in the four upper corners looking across,
	# the capture camera inside, and one exterior camera for the sun side.
	Scaffold.make_camera(root, root, "CovCam0",
			Vector3(-half_x + 0.4, 2.5, -half_z + 0.4), Vector3(0.5, 0.8, 0.5), 15.0)
	Scaffold.make_camera(root, root, "CovCam1",
			Vector3(-half_x + 0.4, 2.5, half_z - 0.4), Vector3(0.5, 0.8, -0.5), 15.0)
	Scaffold.make_camera(root, root, "CovCam2",
			Vector3(half_x - 0.4, 2.5, -half_z + 0.4), Vector3(-0.5, 0.8, 0.5), 15.0)
	Scaffold.make_camera(root, root, "CovCam3",
			Vector3(half_x - 0.4, 2.5, half_z - 0.4), Vector3(-0.5, 0.8, -0.5), 15.0)
	# Aimed slightly up so the emissive ceiling panel is in frame: in a
	# capture without GI it is the only lit interior surface by design.
	Scaffold.make_camera(root, root, "CaptureCam",
			Vector3(-half_x + 0.6, 1.5, 0.0), Vector3(half_x, 1.7, 0.0), 15.0, 70.0, true)
	Scaffold.make_camera(root, root, "ExteriorCam",
			Vector3(7.0, 4.5, 7.0), Vector3(0.0, 1.2, 0.0), 40.0)

	return {
		"root": root,
		"cameras": CAMERAS,
		"q1_surfaces": _q1_surfaces(half_x, half_z, room.y),
		"steps": _steps(p_variant),
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
			Vector3(0.0, p_height, 0.0), Vector3(0.0, 0.0, 1.0), "CovCam1"))
	surfaces.append(Scaffold.make_patch_surface("SouthWall",
			Vector3(-p_half_x, 0.0, p_half_z), Vector3(size_x, 0.0, 0.0),
			Vector3(0.0, p_height, 0.0), Vector3(0.0, 0.0, -1.0), "CovCam0"))
	surfaces.append(Scaffold.make_patch_surface("WestWall",
			Vector3(-p_half_x, 0.0, -p_half_z), Vector3(0.0, 0.0, size_z),
			Vector3(0.0, p_height, 0.0), Vector3.RIGHT, "CovCam2"))
	surfaces.append(Scaffold.make_patch_surface("EastWall",
			Vector3(p_half_x, 0.0, -p_half_z), Vector3(0.0, 0.0, size_z),
			Vector3(0.0, p_height, 0.0), Vector3.LEFT, "CovCam0"))
	# The two interior blocks add candidate area a single room needs to
	# reach 128 line-of-sight patches; their bottoms sit on the floor.
	surfaces.append_array(_block_surfaces("BlockLarge", Vector3(-0.6, 0.6, -0.5), 1.2))
	surfaces.append_array(_block_surfaces("BlockSmall", Vector3(0.7, 0.3, 0.6), 0.6))
	return surfaces


static func _block_surfaces(p_name: String, p_center: Vector3, p_size: float) -> Array:
	var half := p_size * 0.5
	var min_v := p_center - Vector3.ONE * half
	var surfaces: Array = []
	surfaces.append(Scaffold.make_patch_surface(p_name,
			Vector3(min_v.x, p_center.y + half, min_v.z), Vector3(p_size, 0.0, 0.0),
			Vector3(0.0, 0.0, p_size), Vector3.UP, "CovCam0"))
	surfaces.append(Scaffold.make_patch_surface(p_name,
			Vector3(p_center.x + half, 0.0, min_v.z), Vector3(0.0, 0.0, p_size),
			Vector3(0.0, p_size, 0.0), Vector3.RIGHT, "CovCam2"))
	surfaces.append(Scaffold.make_patch_surface(p_name,
			Vector3(p_center.x - half, 0.0, min_v.z), Vector3(0.0, 0.0, p_size),
			Vector3(0.0, p_size, 0.0), Vector3.LEFT, "CovCam0"))
	surfaces.append(Scaffold.make_patch_surface(p_name,
			Vector3(min_v.x, 0.0, p_center.z + half), Vector3(p_size, 0.0, 0.0),
			Vector3(0.0, p_size, 0.0), Vector3(0.0, 0.0, 1.0), "CovCam1"))
	surfaces.append(Scaffold.make_patch_surface(p_name,
			Vector3(min_v.x, 0.0, p_center.z - half), Vector3(p_size, 0.0, 0.0),
			Vector3(0.0, p_size, 0.0), Vector3(0.0, 0.0, -1.0), "CovCam0"))
	return surfaces


static func _steps(p_variant: String) -> Dictionary:
	return {
		"name": "swrt_leak_room_" + p_variant,
		"variants": [
			{
				"id": "static", "form": "Q1+Q3", "frame0_action": "none (static)",
				"end_frame": 0, "motion_frames": 0, "motion_start": 0, "motion_end": 0,
			},
		],
		"cameras": CAMERAS,
		"patch_set": "q1",
		"seed": Scaffold.stream_seed(stream_for(p_variant)),
		"wall_thickness_m": wall_thickness_m(p_variant),
	}
