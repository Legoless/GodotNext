extends RefCounted
## Color Box Room generator (#10 step 5): two sealed 4.0 x 3.0 x 4.0 m rooms
## side by side on X. The lit room (west) has a red north wall and a green
## south wall; the dark room (east) is all gray. Variants: "a" (0.10 m shared
## wall), "b" (0.20 m), "offscreen" (B with a spot behind the capture
## camera), "lightmap" (B plus a LightmapGI node).

const Scaffold := preload("res://generators/scaffold.gd")

const ROOM_SIZE := Vector3(4.0, 3.0, 4.0)
const OUTER_WALL := 0.20
const CAMERAS: Array[String] = ["CovCam0", "CovCam1", "CovCam2", "CovCam3", "CaptureCam", "LeakCamLit", "LeakCamDark"]


static func build(p_variant: String) -> Dictionary:
	var shared := 0.10 if p_variant == "a" else 0.20
	var half_shared := shared * 0.5
	var lit_min_x := -half_shared - ROOM_SIZE.x
	var lit_max_x := -half_shared
	var dark_min_x := half_shared
	var dark_max_x := half_shared + ROOM_SIZE.x
	var lit_center_x := (lit_min_x + lit_max_x) * 0.5
	var dark_center_x := (dark_min_x + dark_max_x) * 0.5
	var height := ROOM_SIZE.y
	var half_depth := ROOM_SIZE.z * 0.5
	var full_min_x := lit_min_x - OUTER_WALL
	var full_max_x := dark_max_x + OUTER_WALL
	var full_size_x := full_max_x - full_min_x
	var full_center_x := (full_min_x + full_max_x) * 0.5
	var wall_span_y := height + 2.0 * OUTER_WALL
	var wall_center_y := height * 0.5

	var root := Node3D.new()
	root.name = "ColorBoxRoom" + p_variant.capitalize()
	root.set_meta("shared_wall_thickness_m", shared)
	root.set_meta("room_interior_m", ROOM_SIZE)

	var gray := Scaffold.make_constant_material("Grey075", Color(0.75, 0.75, 0.75))
	var red := Scaffold.make_constant_material("RedWall", Color(0.75, 0.05, 0.05))
	var green := Scaffold.make_constant_material("GreenWall", Color(0.05, 0.75, 0.05))

	# Sealed environment: black background, no ambient, so the dark room
	# receives light only through (or leaking past) the shared wall.
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
			Vector3(full_center_x, -OUTER_WALL * 0.5, 0.0),
			Vector3(full_size_x, OUTER_WALL, ROOM_SIZE.z + 2.0 * OUTER_WALL), gray)
	Scaffold.add_box_instance(root, root, "Ceiling",
			Vector3(full_center_x, height + OUTER_WALL * 0.5, 0.0),
			Vector3(full_size_x, OUTER_WALL, ROOM_SIZE.z + 2.0 * OUTER_WALL), gray)
	Scaffold.add_box_instance(root, root, "LitEndWall",
			Vector3(lit_min_x - OUTER_WALL * 0.5, wall_center_y, 0.0),
			Vector3(OUTER_WALL, wall_span_y, ROOM_SIZE.z + 2.0 * OUTER_WALL), gray)
	Scaffold.add_box_instance(root, root, "DarkEndWall",
			Vector3(dark_max_x + OUTER_WALL * 0.5, wall_center_y, 0.0),
			Vector3(OUTER_WALL, wall_span_y, ROOM_SIZE.z + 2.0 * OUTER_WALL), gray)
	Scaffold.add_box_instance(root, root, "SharedWall",
			Vector3(0.0, wall_center_y, 0.0),
			Vector3(shared, wall_span_y, ROOM_SIZE.z + 2.0 * OUTER_WALL), gray)
	# North (red in the lit room) and south (green) walls, split per room so
	# the dark room stays all gray.
	_add_side_wall(root, "LitNorthWall", lit_min_x, lit_max_x, -half_depth, wall_center_y, wall_span_y, red)
	_add_side_wall(root, "SharedNorthWall", lit_max_x, dark_min_x, -half_depth, wall_center_y, wall_span_y, gray)
	_add_side_wall(root, "DarkNorthWall", dark_min_x, dark_max_x, -half_depth, wall_center_y, wall_span_y, gray)
	_add_side_wall(root, "LitSouthWall", lit_min_x, lit_max_x, half_depth, wall_center_y, wall_span_y, green)
	_add_side_wall(root, "SharedSouthWall", lit_max_x, dark_min_x, half_depth, wall_center_y, wall_span_y, gray)
	_add_side_wall(root, "DarkSouthWall", dark_min_x, dark_max_x, half_depth, wall_center_y, wall_span_y, gray)

	Scaffold.add_box_instance(root, root, "BlockLarge",
			Vector3(lit_center_x - 0.5, 0.6, -0.5), Vector3(1.2, 1.2, 1.2), gray)
	Scaffold.add_box_instance(root, root, "BlockSmall",
			Vector3(lit_center_x + 0.7, 0.3, 0.6), Vector3(0.6, 0.6, 0.6), gray)

	var capture_pos := Vector3(lit_min_x + 0.8, 2.0, 0.0)
	if p_variant == "offscreen":
		# Behind CaptureCam (west of it), outside every camera frustum,
		# aimed at the far (shared) wall.
		var spot := SpotLight3D.new()
		spot.name = "OffscreenSpot"
		var spot_pos := capture_pos + Vector3(-0.45, 0.3, 0.0)
		var target := Vector3(lit_max_x, 1.4, 0.0)
		spot.transform = Transform3D(
				Basis.looking_at((target - spot_pos).normalized(), Vector3.UP), spot_pos)
		spot.light_energy = 1.0
		spot.spot_range = 6.0
		spot.spot_angle = 35.0
		spot.shadow_enabled = true
		Scaffold.add_owned(root, spot, root)
	else:
		var omni := OmniLight3D.new()
		omni.name = "RoomLight"
		omni.position = Vector3(lit_center_x, 2.6, 0.0)
		omni.light_energy = 1.0
		omni.omni_range = 6.0
		omni.shadow_enabled = true
		if p_variant == "lightmap":
			omni.light_bake_mode = Light3D.BAKE_DYNAMIC
		Scaffold.add_owned(root, omni, root)

	if p_variant == "lightmap":
		var lightmap := LightmapGI.new()
		lightmap.name = "LightmapGI"
		lightmap.bounces = 16
		lightmap.quality = LightmapGI.BAKE_QUALITY_ULTRA
		lightmap.use_denoiser = false
		lightmap.use_texture_for_bounces = false
		Scaffold.add_owned(root, lightmap, root)

	# West-side coverage cameras looking east, so the off-screen spot (west
	# of them) is in no frustum. LeakCamDark gets a short far plane ending
	# just past the shared wall for the same reason.
	Scaffold.make_camera(root, root, "CovCam0",
			Vector3(lit_min_x + 0.5, 2.5, -1.5), Vector3(lit_center_x, 1.0, 0.0), 15.0)
	Scaffold.make_camera(root, root, "CovCam1",
			Vector3(lit_min_x + 0.5, 2.5, 1.5), Vector3(lit_center_x, 1.0, 0.0), 15.0)
	Scaffold.make_camera(root, root, "CovCam2",
			Vector3(dark_min_x + 0.5, 2.5, -1.5), Vector3(dark_center_x, 1.0, 0.0), 15.0)
	Scaffold.make_camera(root, root, "CovCam3",
			Vector3(dark_min_x + 0.5, 2.5, 1.5), Vector3(dark_center_x, 1.0, 0.0), 15.0)
	Scaffold.make_camera(root, root, "CaptureCam",
			capture_pos, Vector3(lit_max_x, 1.2, 0.0), 15.0, 70.0, true)
	Scaffold.make_camera(root, root, "LeakCamLit",
			Vector3(lit_center_x, 1.5, 0.0), Vector3(lit_max_x, 1.5, 0.0), 4.0)
	Scaffold.make_camera(root, root, "LeakCamDark",
			Vector3(dark_center_x, 1.5, 0.0), Vector3(dark_min_x, 1.5, 0.0), 3.0)

	return {
		"root": root,
		"cameras": CAMERAS,
		"q1_surfaces": _q1_surfaces(lit_min_x, lit_max_x, dark_min_x, dark_max_x, height, half_depth),
		"q3_patches": _q3_patches(lit_min_x, lit_center_x, half_shared, half_depth),
		"shared_wall_thickness_m": shared,
		"offscreen_light_node": "OffscreenSpot" if p_variant == "offscreen" else "",
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
		surfaces.append(Scaffold.make_patch_surface("SharedWall",
				Vector3(room["shared_face_x"], 0.0, -p_half_depth), Vector3(0.0, 0.0, p_half_depth * 2.0),
				Vector3(0.0, p_height, 0.0), room["shared_normal"], camera))
	return surfaces


## 16 lit-room patches (4 per vertical wall) mirrored across the shared wall
## (x -> -x) into 16 dark-room patches.
static func _q3_patches(
		p_lit_min: float, p_lit_center: float, p_half_shared: float, p_half_depth: float) -> Array:
	var lit: Array = []
	var walls := [
		{"node": "SharedWall", "dark_node": "SharedWall", "normal": Vector3.LEFT,
			"positions": [Vector3(-p_half_shared, 1.0, -1.0), Vector3(-p_half_shared, 1.0, 1.0),
					Vector3(-p_half_shared, 2.0, -1.0), Vector3(-p_half_shared, 2.0, 1.0)]},
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
