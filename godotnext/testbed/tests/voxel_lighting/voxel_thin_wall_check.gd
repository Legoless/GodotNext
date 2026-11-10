extends Node3D
## #188: GPU check that a thin wall keeps the next room's light out of voxel lighting.
##
## Two 0.2 m walls stand side by side, 4 m apart. At the default 10 mesh-SDF voxels per meter,
## a 0.2 m wall's field never drops much below -eps, so the level-0 voxel centers at z = 0.1
## inside each wall are within eps of a face: 3 cm from the +Z face of the left wall and 7 cm
## from the +Z face of the right one. Room A (z below the walls) has no light. In room B an
## unshadowed omni light lights each wall's +Z face and a box face 0.37 m beyond the wall, which
## also faces -Z. A lookup on a wall's room-A face blends slot +Z of the voxels at z = -0.1 and
## z = 0.1, so a voxel inside the wall that files the box face there lights room A.
## Run with run_voxel_lighting_check.sh (hardware ray tracing tier, quality High).
##
## With debug/voxel_lighting_readback on, the engine probes the GLSL lookup at stored hits. For
## each wall: the room-B face and the box face are lit and covered, and the room-A face is
## covered (both voxels hold that face) and dark.

const READBACK := "rendering/global_illumination/nextgi/debug/voxel_lighting_readback"
const WALL_THICKNESS := 0.2
const WALL_WIDTH := 2.4
const WALL_HEIGHT := 3.0
const GAP := 0.37
# Left: room-A face at z = -0.07 (centers 3 cm from the +Z face). Right: -0.03 (7 cm).
const SETUPS := [Vector2(-2.0, -0.07), Vector2(2.0, -0.03)]
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
	for setup: Vector2 in SETUPS:
		var x := setup.x
		var low := setup.y
		var high := low + WALL_THICKNESS
		_add_box("Wall", Vector3(x, WALL_HEIGHT * 0.5, low + WALL_THICKNESS * 0.5), Vector3(WALL_WIDTH, WALL_HEIGHT, WALL_THICKNESS), material)
		_add_box("BoxB", Vector3(x, WALL_HEIGHT * 0.5, high + GAP + 0.2), Vector3(WALL_WIDTH, WALL_HEIGHT, 0.4), material)
		var light := OmniLight3D.new()
		light.position = Vector3(x, WALL_HEIGHT * 0.5, high + GAP * 0.5)
		# Reaches neither the other setup nor around its own wall's edges.
		light.omni_range = 1.5
		light.light_energy = 8.0
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
	if str(info.get("tier", "")) != "HWRT":
		_fail("needs the HWRT tier for SRC relight, got %s" % str(info.get("tier", "")))
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
	# Per setup: [count, luminance sum, covered count, weight sum] for room A, room B, box.
	var buckets := []
	for setup: Vector2 in SETUPS:
		buckets.append({"room_a": [0, 0.0, 0, 0.0], "room_b": [0, 0.0, 0, 0.0], "box": [0, 0.0, 0, 0.0]})
	for index in positions.size():
		var p := positions[index]
		var n := normals[index]
		var r := results[index]
		if p.y < 0.5 or p.y > WALL_HEIGHT - 0.5:
			continue
		for setup_index in SETUPS.size():
			var setup: Vector2 = SETUPS[setup_index]
			if absf(p.x - setup.x) > WALL_WIDTH * 0.5 - 0.4:
				continue
			var low := setup.y
			var high := low + WALL_THICKNESS
			var key := ""
			if absf(p.z - low) < 0.03 and n.z < -0.9:
				key = "room_a"
			elif absf(p.z - high) < 0.03 and n.z > 0.9:
				key = "room_b"
			elif absf(p.z - (high + GAP)) < 0.03 and n.z < -0.9:
				key = "box"
			if key == "":
				continue
			var bucket: Array = buckets[setup_index][key]
			bucket[0] += 1
			bucket[1] += _luminance(r)
			if r.a >= 0.5:
				bucket[2] += 1
			bucket[3] += r.a
	_last_stats = {"frame": _frames, "probes": positions.size(), "setups": buckets}
	var failure := _probe_failure(buckets)
	if failure != "":
		# SRC capture and relight can still be converging. Give them until PROBE_UNTIL_FRAME.
		if _frames > PROBE_UNTIL_FRAME:
			_fail(failure)
		return
	print("VOXEL_THIN_WALL probes %s" % JSON.stringify(_last_stats))
	print("VOXEL_THIN_WALL_PASS")
	ProjectSettings.set_setting(READBACK, false)
	get_tree().quit(0)


func _probe_failure(p_buckets: Array) -> String:
	for setup_index in SETUPS.size():
		var name := "wall at z %.2f" % SETUPS[setup_index].y
		var setup: Dictionary = p_buckets[setup_index]
		for key in ["room_a", "room_b", "box"]:
			if int(setup[key][0]) < 8:
				return "%s: only %d %s probes" % [name, int(setup[key][0]), key]
		var room_a: Array = setup["room_a"]
		var room_b: Array = setup["room_b"]
		var box: Array = setup["box"]
		var box_mean := float(box[1]) / float(box[0])
		var room_b_mean := float(room_b[1]) / float(room_b[0])
		var room_a_mean := float(room_a[1]) / float(room_a[0])
		if box_mean <= 0.5 or room_b_mean <= 0.5:
			return "%s: room B is not lit (box %.4f, wall %.4f)" % [name, box_mean, room_b_mean]
		if float(box[2]) / float(box[0]) < 0.8 or float(room_b[2]) / float(room_b[0]) < 0.8:
			return "%s: room B coverage below 0.8" % name
		# Both voxels around the room-A face hold that face: full weight, not a WRC fallback.
		if float(room_a[3]) / float(room_a[0]) < 0.95:
			return "%s: room-A face mean weight %.3f below 0.95 (a voxel inside the wall lost that face)" % [name, float(room_a[3]) / float(room_a[0])]
		if room_a_mean > 0.05 * box_mean:
			return "%s: room-A face %.4f leaks room B's box %.4f through the wall (direction slots)" % [name, room_a_mean, box_mean]
	return ""
