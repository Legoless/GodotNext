extends Node3D
## #188: on the software tier, voxel lighting shades after this frame's SRC relight.
##
## A spot with energy 0 faces a wall until the wall's stored hits shade to a dark, covered
## front face. The spot's energy is then raised. The render that first sees the light must
## already shade those hits bright: the update runs in the "NextGI: SRC relight" slot, after
## the relight. The pre-render hook shaded from the previous frame's pages, so that render
## stayed dark and the next one lit. Run with run_voxel_lighting_check.sh on the software tier
## (hardware_ray_tracing Off, or Auto under debug/deny_ray_query).

const READBACK := "rendering/global_illumination/nextgi/debug/voxel_lighting_readback"
const WALL_CENTER := Vector3(0.0, 1.0, 0.0)
const WALL_SCALE := Vector3(1.6, 1.6, 0.3)
const FRONT_Z := 0.15
const DARK_MAX := 0.02
const LIT_MIN := 0.05
const READY_FRAMES := 4
const MIN_PROBES := 4
const GIVE_UP_FRAME := 900

var _frames := 0
var _ready_frames := 0
var _dark_luma := 0.0
var _phase := 0
var _lit: Array[float] = []
var _light: SpotLight3D


func _ready() -> void:
	ProjectSettings.set_setting(READBACK, true)
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0, 0, 0)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0, 0, 0)
	environment.ambient_light_energy = 0.0
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	environment.nextgi_enabled = true
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)

	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.8, 0.8, 0.8)
	var mesh := BoxMesh.new()
	mesh.material = material
	var wall := MeshInstance3D.new()
	wall.mesh = mesh
	wall.position = WALL_CENTER
	wall.scale = WALL_SCALE
	add_child(wall)

	_light = SpotLight3D.new()
	# SpotLight3D points along -Z, toward the wall.
	_light.position = Vector3(0.0, 1.0, 1.5)
	_light.spot_angle = 40.0
	_light.spot_range = 4.0
	_light.light_energy = 0.0
	_light.shadow_enabled = false
	add_child(_light)

	var camera := Camera3D.new()
	# look_at() requires the node to be in the tree and prints an error otherwise.
	camera.look_at_from_position(Vector3(0.0, 1.0, 2.6), WALL_CENTER)
	camera.current = true
	add_child(camera)


func _luminance(p_color: Color) -> float:
	return 0.2126 * p_color.r + 0.7152 * p_color.g + 0.0722 * p_color.b


# Mean luminance of covered front-face probes, and how many there were.
func _front(p_info: Dictionary) -> Vector2:
	if not p_info.has("voxel_lighting_probe_positions"):
		return Vector2.ZERO
	var positions: PackedVector3Array = p_info["voxel_lighting_probe_positions"]
	var normals: PackedVector3Array = p_info["voxel_lighting_probe_normals"]
	var results: PackedColorArray = p_info["voxel_lighting_probe_radiance"]
	if positions.size() != normals.size() or positions.size() != results.size():
		return Vector2.ZERO
	var count := 0
	var luma := 0.0
	for index in positions.size():
		var p: Vector3 = positions[index]
		var n: Vector3 = normals[index]
		var r: Color = results[index]
		if absf(p.z - FRONT_Z) > 0.08 or n.z < 0.7 or r.a < 0.5:
			continue
		count += 1
		luma += _luminance(r)
	if count == 0:
		return Vector2.ZERO
	return Vector2(luma / float(count), float(count))


func _fail(p_reason: String) -> void:
	ProjectSettings.set_setting(READBACK, false)
	push_error(p_reason)
	print("VOXEL_RELIGHT_FRAME_FAIL %s" % p_reason)
	get_tree().quit(1)


func _process(_delta: float) -> void:
	_frames += 1
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var tier := str(info.get("tier", ""))
	if tier != "" and tier != "SWRT":
		_fail("needs the software tier, got %s" % tier)
		return
	if _phase == 0:
		if _frames < 60:
			return
		var front := _front(info)
		if front.y >= MIN_PROBES and front.x <= DARK_MAX:
			_ready_frames += 1
			_dark_luma = front.x
		else:
			_ready_frames = 0
		if _ready_frames < READY_FRAMES:
			if _frames > GIVE_UP_FRAME:
				_fail("front face never settled dark (luma %.4f probes %.0f stored %s)" % [front.x, front.y, str(info.get("voxel_lighting_stored_hits", -1))])
			return
		_light.light_energy = 8.0
		_phase = 1
		return
	var front := _front(info)
	_lit.append(front.x)
	if _lit.size() < 2:
		return
	var slot := String(info.get("voxel_lighting_slot", "none"))
	print("VOXEL_RELIGHT_FRAME dark=%.4f switch=%.4f next=%.4f slot=%s tier=%s frame=%d" % [_dark_luma, _lit[0], _lit[1], slot, str(info.get("tier", "")), _frames])
	if str(info.get("tier", "")) != "SWRT":
		_fail("needs the software tier, got %s" % str(info.get("tier", "")))
		return
	if bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/debug/deny_ray_query", false)):
		if str(info.get("reason", "")) != "NO_RAY_QUERY":
			_fail("deny_ray_query reason is %s, expected NO_RAY_QUERY" % str(info.get("reason", "")))
			return
	if slot != "NextGI: SRC relight":
		_fail("voxel lighting ran in slot \"%s\", expected \"NextGI: SRC relight\" (switch %.4f next %.4f)" % [slot, _lit[0], _lit[1]])
		return
	if _lit[0] < LIT_MIN:
		_fail("the frame the light switched on stayed dark (%.4f, next frame %.4f, dark was %.4f)" % [_lit[0], _lit[1], _dark_luma])
		return
	ProjectSettings.set_setting(READBACK, false)
	print("VOXEL_RELIGHT_FRAME_OK dark=%.4f switch=%.4f next=%.4f slot=%s" % [_dark_luma, _lit[0], _lit[1], slot])
	get_tree().quit(0)
