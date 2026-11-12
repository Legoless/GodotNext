extends Node3D
## #188: GPU check that a thin wall keeps each room's light out of the other in voxel lighting.
##
## Four 0.2 m walls stand side by side. At the default 10 mesh-SDF voxels per meter, a 0.2 m
## wall's field never drops much below -eps, so the level-0 voxel centers at z = 0.1 inside
## each wall are within eps of a face: 3 cm from the +Z face (room-A face at z = -0.07) or 7 cm
## (-0.03). Room A lies at z below a wall, room B past it, where box B's -Z face faces the wall.
## Lookups blend two voxels along z, so a voxel inside a wall is read from both rooms.
## - Room B lit (box B 0.37 m past the wall): an unshadowed omni light in room B lights the
##   wall's +Z face and box B's face. A voxel inside the wall that files box B in the slot the
##   wall's room-A face reads lights room A.
## - Room A lit (box B 3 cm past the wall): an omni light in room A, whose range ends short of
##   box B, lights the wall's room-A face. A voxel inside the wall that files that face in the
##   slot box B's face reads lights room B.
## Run with run_voxel_lighting_check.sh (hardware ray tracing tier, quality High).
##
## With debug/voxel_lighting_readback on, the engine probes the GLSL lookup at stored hits.
## Room B lit: the room-B face and box B are lit and covered, and the room-A face is dark and
## read from its room-A voxel only (its weight is that voxel's trilinear share). Room A lit: the
## room-A face is lit, and box B's face is dark.
##
## Side setups add a 10 cm wall and a 20 cm wall, lit from room B. A box in room A faces the
## wall from 3 cm away. Its face must read no light from the lit face. The lookup drops a slot
## whose surface is on the far side of the sample. The same scene runs on the software tier.

const READBACK := "rendering/global_illumination/nextgi/debug/voxel_lighting_readback"
const WALL_THICKNESS := 0.2
const WALL_WIDTH := 2.4
const WALL_HEIGHT := 3.0
const LIGHTING_VOXEL := 0.2
# low: the room-A face's z. gap: from the room-B face to box B's -Z face.
const SETUPS := [
	{"x": -2.0, "low": -0.07, "gap": 0.37, "lit": "B"},
	{"x": 2.0, "low": -0.03, "gap": 0.37, "lit": "B"},
	{"x": -5.0, "low": -0.07, "gap": 0.03, "lit": "A"},
	{"x": 5.0, "low": -0.03, "gap": 0.03, "lit": "A"},
]
# Lit from room B. The dark box faces the wall from room A, 3 cm off the room-A face.
# Narrow, and stacked along z inside the level-0 clipmap (the camera sits at z = -4),
# above the four walls so these lights do not reach them. The 20 cm wall at z = -3.63
# puts a voxel center 7 cm from the lit face, the alignment that used to leak. The 10 cm
# wall at z = 0.05 has a voxel center in the middle of the wall.
const SIDE_SETUPS := [
	{"x": 0.0, "y": 4.8, "low": -3.63, "thickness": 0.2, "gap": 0.03, "width": 2.0},
	{"x": 0.0, "y": 4.8, "low": 0.05, "thickness": 0.1, "gap": 0.03, "width": 2.0},
]
# Room A's light: this far in front of the room-A face, with a range that ends 3 cm short of
# box B (light to box B is ROOM_A_LIGHT_DISTANCE + WALL_THICKNESS + gap).
const ROOM_A_LIGHT_DISTANCE := 0.6
const ROOM_A_LIGHT_RANGE := 0.8
# Box B probes this close to the light's axis (in x and y) sit behind the lit part of the wall.
const LIT_RADIUS := 0.5
const PROBE_UNTIL_FRAME := 900

var _frames := 0
var _last_stats := {}


func _ready() -> void:
	ProjectSettings.set_setting(READBACK, true)
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0, 0.0, 0.0)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.0, 0.0, 0.0)
	environment.ambient_light_energy = 0.0
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	environment.nextgi_enabled = true
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)

	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.8, 0.8, 0.8)
	for setup: Dictionary in SETUPS:
		var x: float = setup["x"]
		var low: float = setup["low"]
		var gap: float = setup["gap"]
		var high := low + WALL_THICKNESS
		_add_box("Wall", Vector3(x, WALL_HEIGHT * 0.5, low + WALL_THICKNESS * 0.5), Vector3(WALL_WIDTH, WALL_HEIGHT, WALL_THICKNESS), material)
		_add_box("BoxB", Vector3(x, WALL_HEIGHT * 0.5, high + gap + 0.2), Vector3(WALL_WIDTH, WALL_HEIGHT, 0.4), material)
		var light := OmniLight3D.new()
		if setup["lit"] == "B":
			light.position = Vector3(x, WALL_HEIGHT * 0.5, high + gap * 0.5)
			# Reaches neither another setup nor around its own wall's edges.
			light.omni_range = 1.5
		else:
			light.position = Vector3(x, WALL_HEIGHT * 0.5, low - ROOM_A_LIGHT_DISTANCE)
			# Ends short of box B and of every other setup.
			light.omni_range = ROOM_A_LIGHT_RANGE
		light.light_energy = 8.0
		light.shadow_enabled = false
		add_child(light)
	for setup: Dictionary in SIDE_SETUPS:
		var x: float = setup["x"]
		var y: float = setup["y"]
		var low: float = setup["low"]
		var thickness: float = setup["thickness"]
		var gap: float = setup["gap"]
		var side_width: float = setup["width"]
		var high := low + thickness
		_add_box("SideWall", Vector3(x, y, low + thickness * 0.5), Vector3(side_width, 2.2, thickness), material)
		# Black, so a lit lookup on this face is the wall's light coming through the side test.
		var black := StandardMaterial3D.new()
		black.albedo_color = Color(0.0, 0.0, 0.0)
		_add_box("SideBoxA", Vector3(x, y, low - gap - 0.2), Vector3(side_width, 2.2, 0.4), black)
		var light := OmniLight3D.new()
		# 1.5 m past the lit face, range 1.60 m: the lit face's center is inside the sphere and
		# the dark face (at least 13 cm past the lit face) is outside it.
		light.position = Vector3(x, y, high + 1.50)
		light.omni_range = 1.60
		light.light_energy = 64.0
		light.shadow_enabled = false
		add_child(light)

	var camera := Camera3D.new()
	add_child(camera)
	camera.position = Vector3(0.0, 1.5, -4.0)
	camera.look_at(Vector3(0.0, 1.5, 0.0))
	camera.current = true


func _add_box(p_name: String, p_center: Vector3, p_size: Vector3, p_material: Material) -> void:
	var mesh := BoxMesh.new()
	mesh.size = p_size
	mesh.material = p_material
	var instance := MeshInstance3D.new()
	instance.name = p_name
	instance.mesh = mesh
	instance.position = p_center
	add_child(instance)


func _fail(p_reason: String) -> void:
	ProjectSettings.set_setting(READBACK, false)
	push_error(p_reason)
	print("VOXEL_THIN_WALL_FAIL %s %s" % [p_reason, JSON.stringify(_last_stats)])
	get_tree().quit(1)


func _luminance(p_color: Color) -> float:
	return 0.2126 * p_color.r + 0.7152 * p_color.g + 0.0722 * p_color.b


func _process(_delta: float) -> void:
	_frames += 1
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	if not info.has("voxel_lighting_probe_positions"):
		if _frames > PROBE_UNTIL_FRAME:
			_fail("no probes with %s on (tier %s)" % [READBACK, str(info.get("tier", ""))])
		return
	var tier := str(info.get("tier", ""))
	if tier != "HWRT" and tier != "SWRT":
		_fail("needs the HWRT or SWRT tier for SRC relight, got %s" % tier)
		return
	# Judge a fully traced volume: in this static scene the re-trace stops once the fill is done.
	if int(info.get("voxel_lighting_retrace", -1)) != 0:
		if _frames > PROBE_UNTIL_FRAME:
			_fail("the volume was still tracing at frame %d" % _frames)
		return
	var positions: PackedVector3Array = info["voxel_lighting_probe_positions"]
	var normals: PackedVector3Array = info["voxel_lighting_probe_normals"]
	var results: PackedColorArray = info["voxel_lighting_probe_radiance"]
	if positions.size() != normals.size() or positions.size() != results.size():
		_fail("probe arrays differ in size")
		return
	# Per setup: [count, luminance sum, covered count, weight sum, luminance max] per bucket.
	var buckets := []
	for setup: Dictionary in SETUPS:
		buckets.append({"room_a": [0, 0.0, 0, 0.0, 0.0], "room_b": [0, 0.0, 0, 0.0, 0.0], "box": [0, 0.0, 0, 0.0, 0.0], "box_lit": [0, 0.0, 0, 0.0, 0.0]})
	var side_buckets := []
	for setup: Dictionary in SIDE_SETUPS:
		side_buckets.append({"lit": [0, 0.0, 0, 0.0, 0.0], "dark": [0, 0.0, 0, 0.0, 0.0], "back": [0, 0.0, 0, 0.0, 0.0]})
	for index in positions.size():
		var p := positions[index]
		var n := normals[index]
		var r := results[index]
		if p.y >= 0.5 and p.y <= WALL_HEIGHT - 0.5:
			for setup_index in SETUPS.size():
				var setup: Dictionary = SETUPS[setup_index]
				var x: float = setup["x"]
				if absf(p.x - x) > WALL_WIDTH * 0.5 - 0.4:
					continue
				var low: float = setup["low"]
				var high := low + WALL_THICKNESS
				var keys := []
				if absf(p.z - low) < 0.03 and n.z < -0.9:
					keys.append("room_a")
				elif absf(p.z - high) < 0.03 and n.z > 0.9:
					keys.append("room_b")
				elif absf(p.z - (high + float(setup["gap"]))) < 0.03 and n.z < -0.9:
					keys.append("box")
					if Vector2(p.x - x, p.y - WALL_HEIGHT * 0.5).length() < LIT_RADIUS:
						keys.append("box_lit")
				for key: String in keys:
					var bucket: Array = buckets[setup_index][key]
					var luminance := _luminance(r)
					bucket[0] += 1
					bucket[1] += luminance
					if r.a >= 0.5:
						bucket[2] += 1
					bucket[3] += r.a
					bucket[4] = maxf(bucket[4], luminance)
		for setup_index in SIDE_SETUPS.size():
			var side: Dictionary = SIDE_SETUPS[setup_index]
			var x: float = side["x"]
			var side_width: float = side["width"]
			if absf(p.x - x) > side_width * 0.5 - 0.2:
				continue
			var low: float = side["low"]
			var thickness: float = side["thickness"]
			var high := low + thickness
			var dark_z := low - float(side["gap"])
			var in_lit := Vector2(p.x - x, p.y - float(side["y"])).length() < 0.5
			var key := ""
			# The dark face is outside the light's range across the whole wall, so it is not
			# limited to the lit disk. The lit face is, because only that disk is in range.
			if absf(p.z - high) < 0.05 and n.z > 0.9 and in_lit:
				key = "lit"
			elif absf(p.z - dark_z) < 0.05 and n.z > 0.9:
				key = "dark"
			elif absf(p.z - low) < 0.05 and n.z < -0.9:
				key = "back"
			if key == "":
				continue
			var side_bucket: Array = side_buckets[setup_index][key]
			var side_luminance := _luminance(r)
			side_bucket[0] += 1
			side_bucket[1] += side_luminance
			if r.a >= 0.5:
				side_bucket[2] += 1
			side_bucket[3] += r.a
			side_bucket[4] = maxf(side_bucket[4], side_luminance)
	_last_stats = {"frame": _frames, "probes": positions.size(), "setups": buckets, "sides": side_buckets}
	var failure := _probe_failure(buckets)
	var side_failure := _side_failure(side_buckets)
	if side_failure != "":
		failure = side_failure if failure == "" else "%s; %s" % [failure, side_failure]
	if failure != "":
		# SRC capture and relight can still be converging. Give them until PROBE_UNTIL_FRAME.
		if _frames > PROBE_UNTIL_FRAME:
			_fail(failure)
		return
	print("VOXEL_THIN_WALL probes %s" % JSON.stringify(_last_stats))
	print("VOXEL_THIN_WALL_PASS")
	ProjectSettings.set_setting(READBACK, false)
	get_tree().quit(0)


# Every failing condition of every setup, joined; empty when all hold.
func _probe_failure(p_buckets: Array) -> String:
	var failures := PackedStringArray()
	for setup_index in SETUPS.size():
		var failure := _setup_failure(SETUPS[setup_index], p_buckets[setup_index])
		if failure != "":
			failures.append(failure)
	return "; ".join(failures)


func _setup_failure(p_setup: Dictionary, p_buckets: Dictionary) -> String:
	var name := "wall at z %.2f, room %s lit" % [p_setup["low"], p_setup["lit"]]
	for key in ["room_a", "room_b", "box"]:
		if int(p_buckets[key][0]) < 8:
			return "%s: only %d %s probes" % [name, int(p_buckets[key][0]), key]
	var room_a: Array = p_buckets["room_a"]
	var room_b: Array = p_buckets["room_b"]
	var box: Array = p_buckets["box"]
	var box_mean := float(box[1]) / float(box[0])
	var room_b_mean := float(room_b[1]) / float(room_b[0])
	var room_a_mean := float(room_a[1]) / float(room_a[0])
	var failures := PackedStringArray()
	if p_setup["lit"] == "B":
		if box_mean <= 0.5 or room_b_mean <= 0.5:
			failures.append("room B is not lit (box %.4f, wall %.4f)" % [box_mean, room_b_mean])
		if float(box[2]) / float(box[0]) < 0.8 or float(room_b[2]) / float(room_b[0]) < 0.8:
			failures.append("room B coverage below 0.8")
		# The room-A face reads the voxel in room A (z = -0.1) and the one inside the wall
		# (z = 0.1). Only the first may hold anything in the slot the face reads, so the weight is
		# that voxel's trilinear share: neither the share of both (the voxel in the wall holds a
		# surface that box B's face would read too) nor a WRC fallback.
		var share := 1.0 - absf(float(p_setup["low"]) + 0.1) / LIGHTING_VOXEL
		var room_a_weight := float(room_a[3]) / float(room_a[0])
		if absf(room_a_weight - share) > 0.05:
			failures.append("room-A face mean weight %.3f, its room-A voxel's share is %.3f" % [room_a_weight, share])
		if room_a_mean > 0.05 * box_mean:
			failures.append("room-A face %.4f leaks room B's box %.4f through the wall (direction slots)" % [room_a_mean, box_mean])
	else:
		var box_lit: Array = p_buckets["box_lit"]
		if int(box_lit[0]) < 4:
			failures.append("only %d box probes behind the lit part of the wall" % int(box_lit[0]))
		var room_a_max := float(room_a[4])
		if room_a_max <= 0.2:
			failures.append("room A is not lit (wall face max %.4f)" % room_a_max)
		if float(box[4]) > 0.05 * room_a_max:
			failures.append("box B's face %.4f (max) leaks room A's wall face %.4f through the wall (direction slots)" % [float(box[4]), room_a_max])
		if room_b_mean > 0.05 * room_a_max:
			failures.append("room-B face %.4f leaks room A's wall face %.4f through the wall" % [room_b_mean, room_a_max])
	if failures.is_empty():
		return ""
	return "%s: %s" % [name, ", ".join(failures)]


func _side_failure(p_buckets: Array) -> String:
	var failures := PackedStringArray()
	for setup_index in SIDE_SETUPS.size():
		var setup: Dictionary = SIDE_SETUPS[setup_index]
		var name := "%.0f cm wall at z %.2f" % [float(setup["thickness"]) * 100.0, float(setup["low"])]
		var lit: Array = p_buckets[setup_index]["lit"]
		var dark: Array = p_buckets[setup_index]["dark"]
		if int(lit[0]) < 8:
			failures.append("%s: only %d lit probes" % [name, int(lit[0])])
			continue
		if int(dark[0]) < 8:
			failures.append("%s: only %d dark probes" % [name, int(dark[0])])
			continue
		var lit_max := float(lit[4])
		var dark_max := float(dark[4])
		if lit_max <= 0.25:
			failures.append("%s: lit face is dark (max %.4f)" % [name, lit_max])
		# The dark face's brightest probe. A mean hides a leak that only some probes read.
		if dark_max > 0.02:
			failures.append("%s: dark face max %.4f leaks the lit face %.4f through the wall" % [name, dark_max, lit_max])
	return "; ".join(failures)
