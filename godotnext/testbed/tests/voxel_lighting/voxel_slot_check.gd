extends Node3D
## #188: GPU check of voxel lighting's texture space, direction slots and D6 card rule.
##
## A 0.4 m wall (a unit box scaled 4 x 3 x 0.4, so non-uniformly) has a spot light without
## shadows on its front (+Z) face, off center in both card axes, so one card page holds a lit
## disk and a dark rest, and a flip of either page axis moves the disk onto a dark bucket.
## A sphere stands beside it. The sky and ambient light are black, so the wall's back face only
## gets light that leaks. Run with run_voxel_lighting_check.sh (hardware ray tracing tier).
##
## Phase 1, debug/voxel_lighting_readback off: hits are stored, and the GPU counters read -1
## with no readback (the shade has no synchronous readback in production).
## Phase 2, readback on: the engine probes the GLSL lookup at stored hits. The lit disk on the
## front face must be bright and covered, the far side of the front face dark and the disk's
## mirror below the wall's middle dark (a wrong card, page, texel or a flipped page axis breaks
## this), the back face behind the disk dark (a wrong direction slot leaks
## the lit face through the wall) and most of the sphere covered (D6 matches the captured
## depth, not the card plane).
## Phase 3: switching to the software tier rebuilds the volume. Without a registry event in
## this static scene the new volume must still fill from the geometry already there.

const WALL_SCALE := Vector3(4.0, 3.0, 0.4)
const WALL_CENTER := Vector3(0.0, 1.5, 0.0)
# The spot's disk on the front face, off the face's center (0, 1.5) in x and y. A 30 degree cone
# from 1.73 m lights about 1 m around it. That reaches the face's center, which the SRC relight
# needs (it picks a card's lights at the card's center), but not MIRROR_CENTER, where a flipped
# v puts the disk, nor the dark bucket, where a flipped u puts it.
const SPOT_CENTER := Vector2(0.5, 2.2)
const SPOT_DISTANCE := 1.73
const DISK_RADIUS := 0.5
const MIRROR_CENTER := Vector2(0.5, 0.8)
const MIRROR_RADIUS := 0.3
const SPHERE_CENTER := Vector3(3.2, 1.2, 1.0)
const SPHERE_RADIUS := 0.6
const READBACK := "rendering/global_illumination/nextgi/debug/voxel_lighting_readback"
const QUIET_FRAMES := 240
const PROBE_FROM_FRAME := 300
const PROBE_UNTIL_FRAME := 900
const TOGGLE_FRAMES := 240

var _frames := 0
var _phase := 1
var _toggle_frame := 0
var _last_stats := {}


func _ready() -> void:
	ProjectSettings.set_setting(READBACK, false)
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
	_add_box("Wall", WALL_CENTER, WALL_SCALE, material)
	var sphere_mesh := SphereMesh.new()
	sphere_mesh.radius = SPHERE_RADIUS
	sphere_mesh.height = SPHERE_RADIUS * 2.0
	sphere_mesh.material = material
	var sphere := MeshInstance3D.new()
	sphere.name = "Sphere"
	sphere.mesh = sphere_mesh
	sphere.position = SPHERE_CENTER
	add_child(sphere)

	# Points along -Z at the front face. Without a shadow the SRC relight lights it directly.
	var light := SpotLight3D.new()
	light.position = Vector3(SPOT_CENTER.x, SPOT_CENTER.y, WALL_CENTER.z + WALL_SCALE.z * 0.5 + SPOT_DISTANCE)
	light.spot_angle = 30.0
	light.spot_range = 5.0
	light.light_energy = 8.0
	light.shadow_enabled = false
	add_child(light)

	var camera := Camera3D.new()
	add_child(camera)
	camera.position = Vector3(0.5, 1.5, 4.0)
	camera.look_at(Vector3(0.5, 1.5, 0.0))
	camera.current = true


func _add_box(p_name: String, p_center: Vector3, p_scale: Vector3, p_material: Material) -> void:
	var mesh := BoxMesh.new()
	mesh.material = p_material
	var instance := MeshInstance3D.new()
	instance.name = p_name
	instance.mesh = mesh
	instance.position = p_center
	instance.scale = p_scale
	add_child(instance)


func _fail(p_reason: String) -> void:
	ProjectSettings.set_setting(READBACK, false)
	push_error(p_reason)
	print("VOXEL_SLOTS_FAIL %s %s" % [p_reason, JSON.stringify(_last_stats)])
	get_tree().quit(1)


func _luminance(p_color: Color) -> float:
	return 0.2126 * p_color.r + 0.7152 * p_color.g + 0.0722 * p_color.b


func _process(_delta: float) -> void:
	_frames += 1
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	if _phase == 1:
		if _frames < QUIET_FRAMES:
			return
		_check_quiet(info)
	elif _phase == 2:
		if _frames < PROBE_FROM_FRAME:
			return
		_check_probes(info)
	elif _phase == 3:
		_check_toggle(info)


func _check_quiet(p_info: Dictionary) -> void:
	for key in ["tier", "voxel_lighting_stored_hits", "voxel_lighting_coverage_ppm", "voxel_lighting_resampled_nonzero", "voxel_lighting_readbacks"]:
		if not p_info.has(key):
			_fail("missing tier info %s" % key)
			return
	if str(p_info["tier"]) != "HWRT":
		_fail("needs the HWRT tier for SRC relight, got %s" % str(p_info["tier"]))
		return
	if int(p_info["voxel_lighting_stored_hits"]) <= 0:
		_fail("no stored hits after %d frames" % _frames)
		return
	if int(p_info["voxel_lighting_readbacks"]) != 0 or int(p_info["voxel_lighting_coverage_ppm"]) != -1 or int(p_info["voxel_lighting_resampled_nonzero"]) != -1:
		_fail("GPU counters were read back with %s off" % READBACK)
		return
	if p_info.has("voxel_lighting_probe_positions"):
		_fail("probes were read back with %s off" % READBACK)
		return
	print("VOXEL_SLOTS quiet stored_hits=%d readbacks=0" % int(p_info["voxel_lighting_stored_hits"]))
	ProjectSettings.set_setting(READBACK, true)
	_phase = 2


func _check_probes(p_info: Dictionary) -> void:
	if not p_info.has("voxel_lighting_probe_positions"):
		if _frames > PROBE_UNTIL_FRAME:
			_fail("no probes with %s on" % READBACK)
		return
	var positions: PackedVector3Array = p_info["voxel_lighting_probe_positions"]
	var normals: PackedVector3Array = p_info["voxel_lighting_probe_normals"]
	var results: PackedColorArray = p_info["voxel_lighting_probe_radiance"]
	if positions.size() != normals.size() or positions.size() != results.size():
		_fail("probe arrays differ in size")
		return
	var front_z := WALL_CENTER.z + WALL_SCALE.z * 0.5
	var back_z := WALL_CENTER.z - WALL_SCALE.z * 0.5
	var lit := [0, 0.0, 0]
	var dark := [0, 0.0, 0]
	var mirror := [0, 0.0, 0]
	var back := [0, 0.0, 0]
	var sphere := [0, 0.0, 0]
	for index in positions.size():
		var p := positions[index]
		var n := normals[index]
		var r := results[index]
		var bucket = null
		var in_disk := Vector2(p.x, p.y).distance_to(SPOT_CENTER) < DISK_RADIUS
		var inside_y := p.y > WALL_CENTER.y - 1.1 and p.y < WALL_CENTER.y + 1.1
		if absf(p.z - front_z) < 0.03 and n.z > 0.9:
			if in_disk:
				bucket = lit
			elif Vector2(p.x, p.y).distance_to(MIRROR_CENTER) < MIRROR_RADIUS:
				bucket = mirror
			elif p.x > -1.7 and p.x < -0.6 and inside_y:
				bucket = dark
		elif absf(p.z - back_z) < 0.03 and n.z < -0.9 and in_disk:
			bucket = back
		elif absf(p.distance_to(SPHERE_CENTER) - SPHERE_RADIUS) < 0.05:
			bucket = sphere
		if bucket == null:
			continue
		bucket[0] += 1
		bucket[1] += _luminance(r)
		if r.a >= 0.5:
			bucket[2] += 1
	_last_stats = {
		"frame": _frames,
		"probes": positions.size(),
		"lit": lit,
		"dark": dark,
		"mirror": mirror,
		"back": back,
		"sphere": sphere,
		"coverage_ppm": int(p_info.get("voxel_lighting_coverage_ppm", -1)),
		"resampled_nonzero": int(p_info.get("voxel_lighting_resampled_nonzero", -1)),
		"readbacks": int(p_info.get("voxel_lighting_readbacks", 0)),
	}
	var failure := _probe_failure(lit, dark, mirror, back, sphere, p_info)
	if failure != "":
		# SRC capture and relight can still be converging. Give them until PROBE_UNTIL_FRAME.
		if _frames > PROBE_UNTIL_FRAME:
			_fail(failure)
		return
	print("VOXEL_SLOTS probes %s" % JSON.stringify(_last_stats))
	ProjectSettings.set_setting(READBACK, false)
	RenderingServer.nextgi_set_hardware_ray_tracing(1)
	_toggle_frame = _frames
	_phase = 3


func _probe_failure(p_lit: Array, p_dark: Array, p_mirror: Array, p_back: Array, p_sphere: Array, p_info: Dictionary) -> String:
	for bucket in [[p_lit, "lit front"], [p_dark, "dark front"], [p_mirror, "mirrored front"], [p_back, "back"], [p_sphere, "sphere"]]:
		if int(bucket[0][0]) < 8:
			return "only %d %s probes" % [int(bucket[0][0]), bucket[1]]
	var lit_mean := float(p_lit[1]) / float(p_lit[0])
	var dark_mean := float(p_dark[1]) / float(p_dark[0])
	var mirror_mean := float(p_mirror[1]) / float(p_mirror[0])
	var back_mean := float(p_back[1]) / float(p_back[0])
	if lit_mean <= 0.01:
		return "lit front is dark (%.4f)" % lit_mean
	if float(p_lit[2]) / float(p_lit[0]) < 0.8:
		return "lit front coverage %.3f below 0.8" % (float(p_lit[2]) / float(p_lit[0]))
	if dark_mean > 0.25 * lit_mean:
		return "dark front %.4f is not dark next to lit %.4f (texture space)" % [dark_mean, lit_mean]
	if mirror_mean > 0.25 * lit_mean:
		return "mirrored front %.4f is not dark next to lit %.4f (flipped v)" % [mirror_mean, lit_mean]
	if back_mean > 0.1 * lit_mean:
		return "back face %.4f leaks the lit front %.4f (direction slots)" % [back_mean, lit_mean]
	if float(p_sphere[2]) / float(p_sphere[0]) < 0.6:
		return "sphere coverage %.3f below 0.6 (D6 card depth)" % (float(p_sphere[2]) / float(p_sphere[0]))
	if int(p_info.get("voxel_lighting_coverage_ppm", -1)) <= 0 or int(p_info.get("voxel_lighting_resampled_nonzero", -1)) != 1:
		return "GPU counters were not read back"
	return ""


func _check_toggle(p_info: Dictionary) -> void:
	var tier := str(p_info.get("tier", ""))
	var stored := int(p_info.get("voxel_lighting_stored_hits", 0))
	if tier == "SWRT" and stored > 0:
		print("VOXEL_SLOTS toggle tier=SWRT stored_hits=%d frames=%d" % [stored, _frames - _toggle_frame])
		print("VOXEL_SLOTS_PASS")
		get_tree().quit(0)
		return
	if _frames - _toggle_frame > TOGGLE_FRAMES:
		_fail("after the switch to the software tier: tier=%s stored_hits=%d" % [tier, stored])
