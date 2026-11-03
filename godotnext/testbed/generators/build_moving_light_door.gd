extends RefCounted
## Moving Light + Door (#33). Two 4 x 3 x 4 m rooms share a 0.20 m wall.
## A 1 m door (y 0..2, z -0.5..0.5) is cut out of that wall. The panel sits
## on the lit face and slides +Z. Q3 discs sit on solid wall, off the hole.

const Scaffold := preload("res://generators/scaffold.gd")
const Motion = preload("res://scenes/moving_light_door/motion.gd")

const STREAM := 33
const ROOM_SIZE := Vector3(4.0, 3.0, 4.0)
const OUTER_WALL := 0.20
const SHARED := 0.20
const CAMERAS: Array[String] = [
	"CovCam0", "CovCam1", "CovCam2", "CovCam3", "CaptureCam", "LeakCamLit", "LeakCamDark",
]


static func build() -> Dictionary:
	var half_shared := SHARED * 0.5
	var lit_min_x := -half_shared - ROOM_SIZE.x
	var lit_max_x := -half_shared
	var dark_min_x := half_shared
	var dark_max_x := half_shared + ROOM_SIZE.x
	var lit_center_x := (lit_min_x + lit_max_x) * 0.5
	var height := ROOM_SIZE.y
	var half_depth := ROOM_SIZE.z * 0.5
	var full_min_x := lit_min_x - OUTER_WALL
	var full_max_x := dark_max_x + OUTER_WALL
	var full_size_x := full_max_x - full_min_x
	var wall_span_y := height + 2.0 * OUTER_WALL
	var wall_center_y := height * 0.5
	var lit_center := Vector3(lit_center_x, 2.2, 0.0)

	var root := Node3D.new()
	root.name = "MovingLightDoor"
	root.set_meta("shared_wall_thickness_m", SHARED)
	root.set_meta("room_interior_m", ROOM_SIZE)
	root.set_meta("lit_center", lit_center)

	var gray := Scaffold.make_constant_material("Gray075", Color(0.75, 0.75, 0.75))
	var red := Scaffold.make_constant_material("RedWall", Color(0.75, 0.05, 0.05))
	var green := Scaffold.make_constant_material("GreenWall", Color(0.05, 0.75, 0.05))
	var door_material := Scaffold.make_constant_material("Door", Color(0.45, 0.32, 0.18))

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
			Vector3(0.0, -OUTER_WALL * 0.5, 0.0),
			Vector3(full_size_x, OUTER_WALL, ROOM_SIZE.z + 2.0 * OUTER_WALL), gray)
	Scaffold.add_box_instance(root, root, "Ceiling",
			Vector3(0.0, height + OUTER_WALL * 0.5, 0.0),
			Vector3(full_size_x, OUTER_WALL, ROOM_SIZE.z + 2.0 * OUTER_WALL), gray)
	Scaffold.add_box_instance(root, root, "LitEndWall",
			Vector3(lit_min_x - OUTER_WALL * 0.5, wall_center_y, 0.0),
			Vector3(OUTER_WALL, wall_span_y, ROOM_SIZE.z + 2.0 * OUTER_WALL), gray)
	Scaffold.add_box_instance(root, root, "DarkEndWall",
			Vector3(dark_max_x + OUTER_WALL * 0.5, wall_center_y, 0.0),
			Vector3(OUTER_WALL, wall_span_y, ROOM_SIZE.z + 2.0 * OUTER_WALL), gray)
	# Shared wall around the 1 m x 2 m doorway. Pieces meet the hole edges
	# and do not overlap the opening.
	var side_center_z := (-(half_depth + OUTER_WALL) + -0.5) * 0.5
	var side_length_z := (half_depth + OUTER_WALL) - 0.5
	Scaffold.add_box_instance(root, root, "SharedWallNegZ",
			Vector3(0.0, 1.0, side_center_z),
			Vector3(SHARED, 2.0, side_length_z), gray)
	Scaffold.add_box_instance(root, root, "SharedWallPosZ",
			Vector3(0.0, 1.0, -side_center_z),
			Vector3(SHARED, 2.0, side_length_z), gray)
	Scaffold.add_box_instance(root, root, "SharedWallTop",
			Vector3(0.0, (2.0 + height + OUTER_WALL) * 0.5, 0.0),
			Vector3(SHARED, height + OUTER_WALL - 2.0, ROOM_SIZE.z + 2.0 * OUTER_WALL), gray)

	_add_side_wall(root, "LitNorthWall", lit_min_x, lit_max_x, -half_depth, wall_center_y, wall_span_y, red)
	_add_side_wall(root, "SharedNorthWall", lit_max_x, dark_min_x, -half_depth, wall_center_y, wall_span_y, gray)
	_add_side_wall(root, "DarkNorthWall", dark_min_x, dark_max_x, -half_depth, wall_center_y, wall_span_y, gray)
	_add_side_wall(root, "LitSouthWall", lit_min_x, lit_max_x, half_depth, wall_center_y, wall_span_y, green)
	_add_side_wall(root, "SharedSouthWall", lit_max_x, dark_min_x, half_depth, wall_center_y, wall_span_y, gray)
	_add_side_wall(root, "DarkSouthWall", dark_min_x, dark_max_x, half_depth, wall_center_y, wall_span_y, gray)

	Scaffold.add_box_instance(root, root, "BlockLarge",
			Vector3(lit_center_x - 0.5, 0.6, -0.6), Vector3(1.2, 1.2, 1.2), gray)
	Scaffold.add_box_instance(root, root, "BlockSmall",
			Vector3(lit_center_x + 0.6, 0.3, 0.9), Vector3(0.6, 0.6, 0.6), gray)

	var door := Scaffold.add_box_instance(root, root, "Door",
			Vector3(-0.12, 1.0, 0.0), Vector3(0.04, 2.0, 1.0), door_material)
	door.gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC

	var moving := OmniLight3D.new()
	moving.name = "MovingLight"
	moving.position = Motion.light_position("door_open", 0, lit_center)
	moving.light_energy = 1.0
	moving.omni_range = 6.0
	moving.shadow_enabled = true
	Scaffold.add_owned(root, moving, root)

	var spot_pos := Vector3(-5.3, 1.7, 0.0)
	var spot_target := Vector3(0.0, 1.2, 0.0)
	var spot := SpotLight3D.new()
	spot.name = "OffscreenSpot"
	spot.transform = Transform3D(Basis.looking_at((spot_target - spot_pos).normalized(), Vector3.UP), spot_pos)
	spot.light_energy = 1.0
	spot.spot_range = 8.0
	spot.spot_angle = 35.0
	spot.shadow_enabled = true
	Scaffold.add_owned(root, spot, root)

	Scaffold.make_camera(root, root, "CovCam0",
			Vector3(-3.7, 2.2, -1.3), Vector3(-1.2, 1.2, -0.2), 16.0)
	Scaffold.make_camera(root, root, "CovCam1",
			Vector3(-3.7, 2.2, 1.3), Vector3(-1.2, 1.2, 0.2), 16.0)
	Scaffold.make_camera(root, root, "CovCam2",
			Vector3(1.0, 2.2, -1.3), Vector3(3.0, 1.2, -0.2), 16.0)
	Scaffold.make_camera(root, root, "CovCam3",
			Vector3(1.0, 2.2, 1.3), Vector3(3.0, 1.2, 0.2), 16.0)
	Scaffold.make_camera(root, root, "CaptureCam",
			Vector3(-3.9, 1.7, 0.0), Vector3(0.0, 1.2, 0.0), 16.0, 70.0, true)
	Scaffold.make_camera(root, root, "LeakCamLit",
			Vector3(lit_center_x, 1.4, 0.0), Vector3(0.0, 1.2, 0.0), 6.0)
	Scaffold.make_camera(root, root, "LeakCamDark",
			Vector3(-lit_center_x, 1.4, 0.0), Vector3(0.0, 1.2, 0.0), 2.4)

	root.set_script(load("res://scenes/moving_light_door/moving_light_door.gd"))
	root.set_meta("skip_runtime", true)
	return {
		"root": root,
		"cameras": CAMERAS,
		"q1_surfaces": _q1_surfaces(lit_min_x, lit_max_x, dark_min_x, dark_max_x, height, half_depth),
		"q3_patches": _q3_patches(lit_min_x, lit_center_x, half_shared, half_depth),
		"offscreen_light_node": "OffscreenSpot",
		"steps": _steps(),
	}


static func _add_side_wall(
		p_root: Node, p_name: String, p_min_x: float, p_max_x: float,
		p_inner_z: float, p_center_y: float, p_span_y: float, p_material: Material) -> void:
	var length := p_max_x - p_min_x
	var center_z := p_inner_z + signf(p_inner_z) * OUTER_WALL * 0.5
	Scaffold.add_box_instance(p_root, p_root, p_name,
			Vector3((p_min_x + p_max_x) * 0.5, p_center_y, center_z),
			Vector3(length, p_span_y, OUTER_WALL), p_material)


static func _q1_surfaces(
		p_lit_min: float, p_lit_max: float, p_dark_min: float, p_dark_max: float,
		p_height: float, p_half_depth: float) -> Array:
	var surfaces: Array = []
	var rooms := [
		{"min": p_lit_min, "max": p_lit_max, "camera": "CovCam0", "end": "LitEndWall",
			"shared_normal": Vector3.LEFT, "north": "LitNorthWall", "south": "LitSouthWall",
			"end_face_x": p_lit_min, "end_normal": Vector3.RIGHT, "shared_face_x": p_lit_max},
		{"min": p_dark_min, "max": p_dark_max, "camera": "CovCam2", "end": "DarkEndWall",
			"shared_normal": Vector3.RIGHT, "north": "DarkNorthWall", "south": "DarkSouthWall",
			"end_face_x": p_dark_max, "end_normal": Vector3.LEFT, "shared_face_x": p_dark_min},
	]
	for room_value in rooms:
		var room: Dictionary = room_value
		var min_x: float = room["min"]
		var size_x: float = float(room["max"]) - min_x
		var camera: String = room["camera"]
		surfaces.append(Scaffold.make_patch_surface("Floor",
				Vector3(min_x, 0.0, -p_half_depth), Vector3(size_x, 0.0, 0.0),
				Vector3(0.0, 0.0, p_half_depth * 2.0), Vector3.UP, camera))
		surfaces.append(Scaffold.make_patch_surface("Ceiling",
				Vector3(min_x, p_height, -p_half_depth), Vector3(size_x, 0.0, 0.0),
				Vector3(0.0, 0.0, p_half_depth * 2.0), Vector3.DOWN, camera))
		surfaces.append(Scaffold.make_patch_surface(room["north"],
				Vector3(min_x, 0.0, -p_half_depth), Vector3(size_x, 0.0, 0.0),
				Vector3(0.0, p_height, 0.0), Vector3(0.0, 0.0, 1.0), camera))
		surfaces.append(Scaffold.make_patch_surface(room["south"],
				Vector3(min_x, 0.0, p_half_depth), Vector3(size_x, 0.0, 0.0),
				Vector3(0.0, p_height, 0.0), Vector3(0.0, 0.0, -1.0), camera))
		surfaces.append(Scaffold.make_patch_surface(room["end"],
				Vector3(room["end_face_x"], 0.0, -p_half_depth), Vector3(0.0, 0.0, p_half_depth * 2.0),
				Vector3(0.0, p_height, 0.0), room["end_normal"], camera))
		var face_x: float = room["shared_face_x"]
		var shared_normal: Vector3 = room["shared_normal"]
		# Above the door and to either side of it. The hole itself is not a surface.
		surfaces.append(Scaffold.make_patch_surface("SharedWallTop",
				Vector3(face_x, 2.0, -p_half_depth), Vector3(0.0, 0.0, p_half_depth * 2.0),
				Vector3(0.0, p_height - 2.0, 0.0), shared_normal, camera))
		surfaces.append(Scaffold.make_patch_surface("SharedWallNegZ",
				Vector3(face_x, 0.0, -p_half_depth), Vector3(0.0, 0.0, p_half_depth - 0.5),
				Vector3(0.0, 2.0, 0.0), shared_normal, camera))
		surfaces.append(Scaffold.make_patch_surface("SharedWallPosZ",
				Vector3(face_x, 0.0, 0.5), Vector3(0.0, 0.0, p_half_depth - 0.5),
				Vector3(0.0, 2.0, 0.0), shared_normal, camera))
	return surfaces


static func _q3_patches(
		p_lit_min: float, p_lit_center: float, p_half_shared: float, p_half_depth: float) -> Array:
	var lit: Array = []
	var walls := [
		{"node": "SharedWallNegZ", "dark_node": "SharedWallNegZ", "normal": Vector3.LEFT,
			"positions": [Vector3(-p_half_shared, 1.0, -1.15), Vector3(-p_half_shared, 1.7, -1.15)]},
		{"node": "SharedWallPosZ", "dark_node": "SharedWallPosZ", "normal": Vector3.LEFT,
			"positions": [Vector3(-p_half_shared, 1.0, 1.15), Vector3(-p_half_shared, 1.7, 1.15)]},
		{"node": "LitEndWall", "dark_node": "DarkEndWall", "normal": Vector3.RIGHT,
			"positions": [Vector3(p_lit_min, 1.0, -1.0), Vector3(p_lit_min, 1.0, 1.0),
					Vector3(p_lit_min, 2.0, -1.0), Vector3(p_lit_min, 2.0, 1.0)]},
		{"node": "LitNorthWall", "dark_node": "DarkNorthWall", "normal": Vector3(0.0, 0.0, 1.0),
			"positions": [Vector3(p_lit_center - 1.0, 1.0, -p_half_depth), Vector3(p_lit_center + 1.0, 1.0, -p_half_depth),
					Vector3(p_lit_center - 1.0, 2.0, -p_half_depth), Vector3(p_lit_center + 1.0, 2.0, -p_half_depth)]},
		{"node": "LitSouthWall", "dark_node": "DarkSouthWall", "normal": Vector3(0.0, 0.0, -1.0),
			"positions": [Vector3(p_lit_center - 1.0, 1.0, p_half_depth), Vector3(p_lit_center + 1.0, 1.0, p_half_depth),
					Vector3(p_lit_center - 1.0, 2.0, p_half_depth), Vector3(p_lit_center + 1.0, 2.0, p_half_depth)]},
	]
	var patches: Array = []
	var id := 0
	for wall_value in walls:
		var wall: Dictionary = wall_value
		for pos_value in wall["positions"]:
			var pos: Vector3 = pos_value
			var normal: Vector3 = wall["normal"]
			patches.append({
				"id": id, "room": "lit",
				"pos": [pos.x, pos.y, pos.z],
				"normal": [normal.x, normal.y, normal.z],
				"radius": Scaffold.Q1_PATCH_RADIUS,
				"node": wall["node"], "camera": "LeakCamLit",
			})
			lit.append({"pos": pos, "normal": normal, "dark_node": wall["dark_node"], "id": id})
			id += 1
	for item_value in lit:
		var item: Dictionary = item_value
		var pos: Vector3 = item["pos"]
		var normal: Vector3 = item["normal"]
		patches.append({
			"id": id, "room": "dark", "mirror_of": int(item["id"]),
			"pos": [-pos.x, pos.y, pos.z],
			"normal": [-normal.x, normal.y, normal.z],
			"radius": Scaffold.Q1_PATCH_RADIUS,
			"node": item["dark_node"], "camera": "LeakCamDark",
		})
		id += 1
	return patches


static func _steps() -> Dictionary:
	return {
		"name": "moving_light_door",
		"variants": [
			{
				"id": "door_open", "form": "Q4b", "frame0_action": "gap opens from 0",
				"end_frame": 240, "motion_frames": 120, "motion_start": 0, "motion_end": 120,
			},
			{
				"id": "light_toggle", "form": "Q4a", "frame0_action": "light energy 0 to 1",
				"end_frame": 240, "motion_frames": 0, "motion_start": 0, "motion_end": 0,
			},
			{
				"id": "door_teleport", "form": "Q4a", "frame0_action": "door gap 0 to 1 m",
				"end_frame": 240, "motion_frames": 0, "motion_start": 0, "motion_end": 0,
			},
		],
		"cameras": CAMERAS,
		"patch_set": "q1+q3",
		"seed": Scaffold.stream_seed(STREAM),
	}
