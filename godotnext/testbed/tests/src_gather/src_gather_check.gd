extends Node

## NextGI HWRT diffuse gather check (#25).
##
## Renders Color Box Room A in a 1920x1080 SubViewport and checks the 1/16 rpp counter,
## camera-pan history, the coverage counter, then an open sky for miss luminance.
## Prints one "GODOTNEXT_RESULT <json>" line.

const VIEW_SIZE := Vector2i(1920, 1080)
const EXPECTED_RAYS := 130560
const PROBE_COUNT := 120 * 68


var _failures := PackedStringArray()


func _ready() -> void:
	var result := await run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _check(condition: bool, what: String) -> void:
	if not condition:
		_failures.append(what)


func run() -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = VIEW_SIZE
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	viewport.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	viewport.use_taa = false
	add_child(viewport)

	var room: Node3D = load("res://scenes/color_box_room/cbr_a.tscn").instantiate()
	viewport.add_child(room)
	var camera := room.get_node("CaptureCam") as Camera3D
	_check(camera != null, "CaptureCam")
	if camera == null:
		return _result({})
	_enable_nextgi(room)
	camera.current = true

	var info := {}
	var matched := false
	for frame in 120:
		await get_tree().process_frame
		info = RenderingServer.nextgi_get_tier_info()
		if int(info.get("src_gather_rays", 0)) == EXPECTED_RAYS:
			matched = true
			break
	_check(info.get("tier", "") == "HWRT", "HWRT tier")
	_check(Vector2i(int(info.get("src_gather_screen_width", 0)), int(info.get("src_gather_screen_height", 0))) == VIEW_SIZE, "internal size 1920x1080, got %s x %s" % [info.get("src_gather_screen_width", 0), info.get("src_gather_screen_height", 0)])
	_check(int(info.get("src_gather_expected_rays", 0)) == EXPECTED_RAYS, "expected ray count")
	_check(matched, "gather rays %s == %s" % [info.get("src_gather_rays", 0), EXPECTED_RAYS])

	var static_resets := 1 << 30
	for frame in 8:
		await get_tree().process_frame
		info = RenderingServer.nextgi_get_tier_info()
		static_resets = mini(static_resets, int(info.get("src_gather_history_resets", 1 << 30)))
	_check(static_resets < PROBE_COUNT / 4, "static history resets %d stay under a quarter of the probes" % static_resets)

	camera.global_position += camera.global_transform.basis.x * 0.04
	await get_tree().process_frame
	info = RenderingServer.nextgi_get_tier_info()
	var pan_resets := int(info.get("src_gather_history_resets", PROBE_COUNT))
	_check(pan_resets < PROBE_COUNT / 2, "camera pan keeps history outside disocclusions, resets %d" % pan_resets)
	_check(int(info.get("src_gather_rays", 0)) == EXPECTED_RAYS, "ray count holds after the pan")

	var coverage_hits := RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_NEXTGI_COVERAGE_HITS)
	var coverage_rays := RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_NEXTGI_COVERAGE_RAYS)
	_check(coverage_rays > 0, "coverage rays readable, got %d" % coverage_rays)
	_check(coverage_hits > 0, "coverage hits readable, got %d" % coverage_hits)
	_check(coverage_hits <= coverage_rays, "coverage hits do not exceed rays")

	# Keep the room meshes. Freeing them invalidates the TLAS built earlier in the frame.
	# The open sky replaces the room environment so misses sample the sky, not a black clear.
	var sky_root := Node3D.new()
	sky_root.position = Vector3(300.0, 0.0, 0.0)
	viewport.add_child(sky_root)
	var room_environment := room.get_node("WorldEnvironment") as WorldEnvironment
	room_environment.environment = _make_open_sky_environment()
	_build_open_sky(sky_root)
	camera.current = false
	var miss_luminance := 0.0
	var sky_info := {}
	for frame in 40:
		await get_tree().process_frame
		sky_info = RenderingServer.nextgi_get_tier_info()
		miss_luminance = maxf(miss_luminance, float(sky_info.get("src_gather_miss_luminance", 0.0)))
	_check(miss_luminance > 0.0, "open sky miss luminance %s" % miss_luminance)

	var result := _result(info)
	result["static_resets"] = static_resets
	result["pan_resets"] = pan_resets
	result["coverage_hits"] = coverage_hits
	result["coverage_rays"] = coverage_rays
	result["miss_luminance"] = miss_luminance
	result["sky_rays"] = int(sky_info.get("src_gather_rays", 0))
	return result


func _enable_nextgi(room: Node) -> void:
	var world_environment := room.get_node_or_null("WorldEnvironment") as WorldEnvironment
	_check(world_environment != null, "WorldEnvironment")
	if world_environment == null:
		return
	var environment := world_environment.environment.duplicate()
	environment.nextgi_enabled = true
	environment.ssao_enabled = false
	environment.ssr_enabled = false
	environment.ssil_enabled = false
	environment.sdfgi_enabled = false
	world_environment.environment = environment


func _make_open_sky_environment() -> Environment:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	var material := ProceduralSkyMaterial.new()
	material.sky_top_color = Color(0.35, 0.55, 0.9)
	material.sky_horizon_color = Color(0.75, 0.8, 0.9)
	material.ground_bottom_color = Color(0.25, 0.25, 0.25)
	material.ground_horizon_color = Color(0.45, 0.45, 0.45)
	sky.sky_material = material
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.45, 0.6, 0.9)
	environment.ambient_light_energy = 1.0
	environment.nextgi_enabled = true
	return environment


func _build_open_sky(parent: Node3D) -> void:

	var mesh_instance := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(40.0, 40.0)
	mesh_instance.mesh = plane
	parent.add_child(mesh_instance)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35.0, 25.0, 0.0)
	parent.add_child(sun)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.2, 4.0)
	camera.look_at_from_position(camera.position, Vector3(0.0, 1.2, 0.0))
	camera.current = true
	parent.add_child(camera)


func _result(info: Dictionary) -> Dictionary:
	return {
		"pass": _failures.is_empty(),
		"failures": _failures,
		"tier": info.get("tier", ""),
		"rays": int(info.get("src_gather_rays", 0)),
		"screen": [int(info.get("src_gather_screen_width", 0)), int(info.get("src_gather_screen_height", 0))],
		"uncovered": int(info.get("src_gather_uncovered", 0)),
		"misses": int(info.get("src_gather_misses", 0)),
	}
