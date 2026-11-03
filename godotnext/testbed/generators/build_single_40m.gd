extends RefCounted
## Single-mesh 42 m hall generator (#10 step 5): one [ArrayMesh] built with
## [SurfaceTool] in one [MeshInstance3D]. The 42 x 4 x 3 m interior (length x
## depth x height) is split by 0.10 m interior walls into 7 rooms,
## alternating dark/lit so the central room is lit. The Q3 pair sits in the
## central lit room and its dark neighbor, mirrored across their wall.

const Scaffold := preload("res://generators/scaffold.gd")

const INTERIOR := Vector3(42.0, 3.0, 4.0)
const INTERIOR_WALL := 0.10
const OUTER_WALL := 0.20
const WALL_X: Array[float] = [-15.0, -9.0, -3.0, 3.0, 9.0, 15.0]
const LIT_ROOMS: Array[int] = [1, 3, 5]
const Q1_ROOMS: Array[int] = [1, 3, 4, 5]
const Q1_CAMERAS := {1: "CovCam0", 3: "CovCam1", 4: "CovCam2", 5: "CovCam3"}
const CAMERAS: Array[String] = ["CovCam0", "CovCam1", "CovCam2", "CovCam3", "CaptureCam", "LeakCamLit", "LeakCamDark"]


static func room_bounds(p_room: int) -> Vector2:
	var walls: Array[float] = [-21.0, -15.0, -9.0, -3.0, 3.0, 9.0, 15.0, 21.0]
	var min_x := walls[p_room] + (INTERIOR_WALL * 0.5 if p_room > 0 else 0.0)
	var max_x := walls[p_room + 1] - (INTERIOR_WALL * 0.5 if p_room < 6 else 0.0)
	return Vector2(min_x, max_x)


static func build() -> Dictionary:
	var root := Node3D.new()
	root.name = "ColorBoxRoomSingle40m"
	root.set_meta("interior_wall_thickness_m", INTERIOR_WALL)
	root.set_meta("hall_interior_m", INTERIOR)

	var gray := Scaffold.make_constant_material("Grey075", Color(0.75, 0.75, 0.75))

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0, 0.0, 0.0)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = environment
	Scaffold.add_owned(root, world_env, root)

	var half_depth := INTERIOR.z * 0.5
	var height := INTERIOR.y
	var total_x := INTERIOR.x + 2.0 * OUTER_WALL
	var total_z := INTERIOR.z + 2.0 * OUTER_WALL
	var span_y := height + 2.0 * OUTER_WALL
	var center_y := height * 0.5
	var builder = Scaffold.ChunkedSurfaceBuilder.new(gray)
	builder.add_box(Vector3(0.0, -OUTER_WALL * 0.5, 0.0), Vector3(total_x, OUTER_WALL, total_z))
	builder.add_box(Vector3(0.0, height + OUTER_WALL * 0.5, 0.0), Vector3(total_x, OUTER_WALL, total_z))
	builder.add_box(Vector3(-INTERIOR.x * 0.5 - OUTER_WALL * 0.5, center_y, 0.0), Vector3(OUTER_WALL, span_y, total_z))
	builder.add_box(Vector3(INTERIOR.x * 0.5 + OUTER_WALL * 0.5, center_y, 0.0), Vector3(OUTER_WALL, span_y, total_z))
	builder.add_box(Vector3(0.0, center_y, -half_depth - OUTER_WALL * 0.5), Vector3(total_x, span_y, OUTER_WALL))
	builder.add_box(Vector3(0.0, center_y, half_depth + OUTER_WALL * 0.5), Vector3(total_x, span_y, OUTER_WALL))
	for wall_x in WALL_X:
		builder.add_box(Vector3(wall_x, center_y, 0.0), Vector3(INTERIOR_WALL, span_y, total_z))
	var instance := MeshInstance3D.new()
	instance.name = "HallMesh"
	instance.mesh = builder.finish()
	instance.set_meta("interior_wall_thickness_m", INTERIOR_WALL)
	Scaffold.add_owned(root, instance, root)
	Scaffold.add_trimesh_collider(root, instance)

	for room in LIT_ROOMS:
		var bounds := room_bounds(room)
		var omni := OmniLight3D.new()
		omni.name = "RoomLight%d" % room
		omni.position = Vector3((bounds.x + bounds.y) * 0.5, 2.6, 0.0)
		omni.light_energy = 1.0
		omni.omni_range = 6.0
		omni.shadow_enabled = true
		Scaffold.add_owned(root, omni, root)

	var center_bounds := room_bounds(3)
	var dark_bounds := room_bounds(4)
	for room in Q1_ROOMS:
		var bounds := room_bounds(room)
		var camera_name: String = Q1_CAMERAS[room]
		Scaffold.make_camera(root, root, camera_name,
				Vector3(bounds.x + 0.5, 2.5, -1.5),
				Vector3((bounds.x + bounds.y) * 0.5, 1.0, 0.0), 15.0)
	Scaffold.make_camera(root, root, "CaptureCam",
			Vector3(center_bounds.x + 0.8, 2.0, 0.0), Vector3(center_bounds.y, 1.2, 0.0), 15.0, 70.0, true)
	Scaffold.make_camera(root, root, "LeakCamLit",
			Vector3((center_bounds.x + center_bounds.y) * 0.5, 1.5, 0.0), Vector3(center_bounds.y, 1.5, 0.0), 5.0)
	Scaffold.make_camera(root, root, "LeakCamDark",
			Vector3((dark_bounds.x + dark_bounds.y) * 0.5, 1.5, 0.0), Vector3(dark_bounds.x, 1.5, 0.0), 5.0)

	return {
		"root": root,
		"cameras": CAMERAS,
		"q1_surfaces": _q1_surfaces(height, half_depth),
		"q3_patches": _q3_patches(half_depth),
		"interior_wall_thickness_m": INTERIOR_WALL,
		"mesh_node": "HallMesh",
	}


static func _q1_surfaces(p_height: float, p_half_depth: float) -> Array:
	var surfaces: Array = []
	for room in Q1_ROOMS:
		var bounds := room_bounds(room)
		var size_x := bounds.y - bounds.x
		var camera: String = Q1_CAMERAS[room]
		surfaces.append(Scaffold.make_patch_surface("HallMesh",
				Vector3(bounds.x, 0.0, -p_half_depth), Vector3(size_x, 0.0, 0.0),
				Vector3(0.0, 0.0, p_half_depth * 2.0), Vector3.UP, camera))
		surfaces.append(Scaffold.make_patch_surface("HallMesh",
				Vector3(bounds.x, p_height, -p_half_depth), Vector3(size_x, 0.0, 0.0),
				Vector3(0.0, 0.0, p_half_depth * 2.0), Vector3.DOWN, camera))
		surfaces.append(Scaffold.make_patch_surface("HallMesh",
				Vector3(bounds.x, 0.0, -p_half_depth), Vector3(size_x, 0.0, 0.0),
				Vector3(0.0, p_height, 0.0), Vector3(0.0, 0.0, 1.0), camera))
		surfaces.append(Scaffold.make_patch_surface("HallMesh",
				Vector3(bounds.x, 0.0, p_half_depth), Vector3(size_x, 0.0, 0.0),
				Vector3(0.0, p_height, 0.0), Vector3(0.0, 0.0, -1.0), camera))
		surfaces.append(Scaffold.make_patch_surface("HallMesh",
				Vector3(bounds.x, 0.0, -p_half_depth), Vector3(0.0, 0.0, p_half_depth * 2.0),
				Vector3(0.0, p_height, 0.0), Vector3.RIGHT, camera))
		surfaces.append(Scaffold.make_patch_surface("HallMesh",
				Vector3(bounds.y, 0.0, -p_half_depth), Vector3(0.0, 0.0, p_half_depth * 2.0),
				Vector3(0.0, p_height, 0.0), Vector3.LEFT, camera))
	return surfaces


## 16 patches in the central lit room (4 per vertical wall), mirrored across
## the wall at x = 3.0 into the dark neighbor room.
static func _q3_patches(p_half_depth: float) -> Array:
	var bounds := room_bounds(3)
	var center_x := (bounds.x + bounds.y) * 0.5
	var mirror_x := 3.0
	var walls := [
		{"normal": Vector3.LEFT,
			"positions": [Vector3(bounds.y, 1.0, -1.0), Vector3(bounds.y, 1.0, 1.0),
					Vector3(bounds.y, 2.0, -1.0), Vector3(bounds.y, 2.0, 1.0)]},
		{"normal": Vector3.RIGHT,
			"positions": [Vector3(bounds.x, 1.0, -1.0), Vector3(bounds.x, 1.0, 1.0),
					Vector3(bounds.x, 2.0, -1.0), Vector3(bounds.x, 2.0, 1.0)]},
		{"normal": Vector3(0.0, 0.0, 1.0),
			"positions": [Vector3(center_x - 1.5, 1.0, -p_half_depth), Vector3(center_x + 1.5, 1.0, -p_half_depth),
					Vector3(center_x - 1.5, 2.0, -p_half_depth), Vector3(center_x + 1.5, 2.0, -p_half_depth)]},
		{"normal": Vector3(0.0, 0.0, -1.0),
			"positions": [Vector3(center_x - 1.5, 1.0, p_half_depth), Vector3(center_x + 1.5, 1.0, p_half_depth),
					Vector3(center_x - 1.5, 2.0, p_half_depth), Vector3(center_x + 1.5, 2.0, p_half_depth)]},
	]
	var patches: Array = []
	var lit: Array = []
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
				"node": "HallMesh", "camera": "LeakCamLit",
			})
			lit.append({"pos": pos, "normal": normal, "id": id})
			id += 1
	for item_value in lit:
		var item: Dictionary = item_value
		var pos: Vector3 = item["pos"]
		var normal: Vector3 = item["normal"]
		patches.append({
			"id": id, "room": "dark", "mirror_of": int(item["id"]),
			"pos": [2.0 * mirror_x - pos.x, pos.y, pos.z],
			"normal": [-normal.x, normal.y, normal.z],
			"radius": Scaffold.Q1_PATCH_RADIUS,
			"node": "HallMesh", "camera": "LeakCamDark",
		})
		id += 1
	return patches
