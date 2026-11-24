extends Node3D
## #139: Packed froxel light grid GPU verification.
## Verifies:
##  1. 4,096 lights (omni, spot, barn-door rect area) across 8 fixed cameras:
##     GPU packed grid matches CPU brute-force reference with 0 missing lights,
##     0 extra lights, 0 mismatched cells, 0 overflow, and 0 lights in Hi-Z occluded cells.
##  2. Overflow (> 64 lights in one cell): caps at 64, counts overflow, keeps exact
##     top-64 highest-intensity lights matching CPU reference.
##  3. AreaLight3D barn-door culling: narrow barn doors strictly reduce cell-light assignments
##     compared to open barn doors while matching CPU reference with 0 mismatches.
##  4. Legacy byte-identity: <= 512 lights with MegaLights off leaves packed grid inactive
##     and renders deterministic legacy frames.

const VIEW_SIZE := Vector2i(1920, 1080)


func _ready() -> void:
	# Disable 3D rendering on the root window so SubViewport is the sole 3D view consuming light_grid_request_check().
	get_tree().root.disable_3d = true
	_run()


func _arg(key: String, fallback: String) -> String:
	var prefix := "--" + key + "="
	for a in OS.get_cmdline_user_args():
		if a.begins_with(prefix):
			return a.substr(prefix.length())
	return fallback


func _run() -> void:
	var mode := _arg("lg-check", "all")
	var failures := PackedStringArray()
	var report := {"test": "light_grid", "mode": mode, "pass": false}

	if mode == "all" or mode == "legacy":
		report["legacy"] = await _check_legacy(failures)
	if mode == "all" or mode == "barn_door":
		report["barn_door"] = await _check_barn_door(failures)
	if mode == "all" or mode == "overflow":
		report["overflow"] = await _check_overflow(failures)
	if mode == "all" or mode == "cameras_4096":
		report["cameras_4096"] = await _check_cameras_4096(failures)

	report["failures"] = failures
	report["pass"] = failures.is_empty()
	print("LIGHT_GRID_CHECK_RESULT=" + JSON.stringify(report))
	get_tree().quit(0 if failures.is_empty() else 1)


func _make_viewport(size: Vector2i) -> SubViewport:
	var sub := SubViewport.new()
	sub.own_world_3d = true
	sub.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	sub.msaa_3d = Viewport.MSAA_DISABLED
	sub.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	sub.positional_shadow_atlas_size = 0
	sub.size = size
	add_child(sub)

	var world := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.02, 0.02, 0.03)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_energy = 0.0
	world.environment = env
	sub.add_child(world)
	return sub


func _add_box(parent: Node, pos: Vector3, size: Vector3, albedo: Color) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = size
	var mat := StandardMaterial3D.new()
	mat.albedo_color = albedo
	mat.roughness = 0.7
	box.material = mat
	mi.mesh = box
	mi.position = pos
	parent.add_child(mi)
	return mi


func _request_and_wait_check() -> Dictionary:
	await RenderingServer.frame_post_draw
	RenderingServer.light_grid_request_check()
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	return RenderingServer.light_grid_get_check_result()


func _has_lit_pixels(img: Image) -> bool:
	if img == null:
		return false
	var w := img.get_width()
	var h := img.get_height()
	for y in range(0, h, 8):
		for x in range(0, w, 8):
			var c := img.get_pixel(x, y)
			if c.r > 0.08 or c.g > 0.08 or c.b > 0.08:
				return true
	return false


func _check_legacy(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(Vector2i(256, 256))
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 2.0, 8.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.0, 0.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	_add_box(sub, Vector3(0.0, 0.0, 0.0), Vector3(12.0, 0.4, 12.0), Color(0.8, 0.8, 0.8))
	_add_box(sub, Vector3(0.0, 1.5, -2.0), Vector3(3.0, 3.0, 1.0), Color(0.7, 0.7, 0.7))

	for i in range(16):
		var omni := OmniLight3D.new()
		omni.position = Vector3(-3.0 + float(i % 4) * 2.0, 2.0, -1.0 + float(i / 4) * 1.5)
		omni.omni_range = 6.0
		omni.light_energy = 1.5
		omni.light_color = Color(1.0, 0.8, 0.6)
		omni.shadow_enabled = false
		sub.add_child(omni)

	var res := await _request_and_wait_check()
	var img1 := sub.get_texture().get_image()
	await RenderingServer.frame_post_draw
	var img2 := sub.get_texture().get_image()

	var bytes_equal := img1 != null and img2 != null and img1.get_data() == img2.get_data()
	var lit := _has_lit_pixels(img1)

	if bool(res.get("packed_active", true)):
		failures.append("legacy: packed grid was unexpectedly active with 16 lights")
	if not bytes_equal:
		failures.append("legacy: consecutive frames were not byte-identical")
	if not lit:
		failures.append("legacy: rendered image has no lit pixels")

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return {
		"packed_active": res.get("packed_active", null),
		"bytes_equal": bytes_equal,
		"lit": lit,
	}


func _check_barn_door(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(Vector2i(640, 384))
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 3.0, 14.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.0, 0.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	_add_box(sub, Vector3(0.0, 0.0, 0.0), Vector3(30.0, 0.4, 30.0), Color(0.75, 0.75, 0.75))
	_add_box(sub, Vector3(0.0, 3.0, -8.0), Vector3(30.0, 8.0, 0.5), Color(0.75, 0.75, 0.75))

	# 520 AreaLight3D nodes so packed light grid is active (>512).
	var areas: Array[AreaLight3D] = []
	for i in range(520):
		var a := AreaLight3D.new()
		var ix := i % 26
		var iz := i / 26
		a.position = Vector3(-12.5 + float(ix) * 1.0, 3.5, -6.0 + float(iz) * 0.8)
		a.rotation_degrees = Vector3(-90.0, 0.0, 0.0) # Shine straight down (-Y).
		a.area_size = Vector2(0.4, 0.4)
		a.area_range = 8.0
		a.light_energy = 0.6
		a.shadow_enabled = false
		a.barn_door_angle = 90.0
		a.barn_door_length = 0.2
		sub.add_child(a)
		areas.append(a)

	var open_res := await _request_and_wait_check()

	for a in areas:
		a.barn_door_angle = 12.0
		a.barn_door_length = 1.2

	var narrow_res := await _request_and_wait_check()

	var open_assign := int(open_res.get("gpu_total_assignments", -1))
	var narrow_assign := int(narrow_res.get("gpu_total_assignments", -1))

	if not bool(open_res.get("packed_active", false)) or not bool(narrow_res.get("packed_active", false)):
		failures.append("barn_door: packed grid was not active at 520 area lights")
	if int(open_res.get("mismatched_cells", -1)) != 0 or int(narrow_res.get("mismatched_cells", -1)) != 0:
		failures.append("barn_door: GPU vs CPU reference mismatch (open=%d narrow=%d)" % [int(open_res.get("mismatched_cells", -1)), int(narrow_res.get("mismatched_cells", -1))])
	if not (narrow_assign > 0 and open_assign > narrow_assign):
		failures.append("barn_door: expected narrow assignments (%d) strictly less than open (%d)" % [narrow_assign, open_assign])

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return {
		"open_assignments": open_assign,
		"narrow_assignments": narrow_assign,
		"open_mismatched": int(open_res.get("mismatched_cells", -1)),
		"narrow_mismatched": int(narrow_res.get("mismatched_cells", -1)),
	}


func _check_overflow(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(Vector2i(256, 256))
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 2.0, 10.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 2.0, 0.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	_add_box(sub, Vector3(0.0, 2.0, -4.0), Vector3(16.0, 16.0, 0.5), Color(0.8, 0.8, 0.8))

	# 96 overlapping lights right at the wall + 440 in-frustum lights behind the wall -> 536 > 512 activates packed grid.
	for i in range(96):
		var omni := OmniLight3D.new()
		omni.position = Vector3(0.0, 2.0, 0.0)
		omni.omni_range = 12.0
		omni.light_energy = 0.25 + float((i * 17) % 96) * 0.1
		omni.light_color = Color(1.0, 0.9, 0.8)
		omni.shadow_enabled = false
		sub.add_child(omni)

	for i in range(440):
		var far_omni := OmniLight3D.new()
		far_omni.position = Vector3(-12.0 + float(i % 22) * 1.1, -6.0 + float(i / 22) * 0.8, -12.0)
		far_omni.omni_range = 0.5
		far_omni.light_energy = 0.1
		far_omni.shadow_enabled = false
		sub.add_child(far_omni)

	var res := await _request_and_wait_check()
	if not bool(res.get("packed_active", false)):
		failures.append("overflow: packed grid not active")
	if int(res.get("gpu_max_cell_count", 0)) != 64:
		failures.append("overflow: expected gpu_max_cell_count == 64, got %d" % int(res.get("gpu_max_cell_count", 0)))
	if int(res.get("gpu_overflow_cells", 0)) <= 0 or int(res.get("gpu_overflow_lights", 0)) <= 0:
		failures.append("overflow: expected positive overflow counters")
	if int(res.get("mismatched_cells", -1)) != 0 or int(res.get("missing_lights", -1)) != 0 or int(res.get("extra_lights", -1)) != 0:
		failures.append("overflow: GPU top-64 did not match CPU reference (mismatched=%d missing=%d extra=%d)" % [
			int(res.get("mismatched_cells", -1)),
			int(res.get("missing_lights", -1)),
			int(res.get("extra_lights", -1)),
		])

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return {
		"gpu_max_cell_count": int(res.get("gpu_max_cell_count", 0)),
		"gpu_overflow_cells": int(res.get("gpu_overflow_cells", 0)),
		"gpu_overflow_lights": int(res.get("gpu_overflow_lights", 0)),
		"mismatched_cells": int(res.get("mismatched_cells", -1)),
	}


func _check_cameras_4096(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(VIEW_SIZE)
	var cam := Camera3D.new()
	cam.near = 0.1
	cam.far = 140.0
	cam.fov = 65.0
	cam.current = true
	sub.add_child(cam)

	# Large exhibition hall (280m x 12m x 280m) with outer walls and interior baffle walls for Hi-Z occlusion.
	_add_box(sub, Vector3(0.0, -0.25, 0.0), Vector3(280.0, 0.5, 280.0), Color(0.7, 0.7, 0.7))
	_add_box(sub, Vector3(0.0, 12.25, 0.0), Vector3(280.0, 0.5, 280.0), Color(0.6, 0.6, 0.6))
	_add_box(sub, Vector3(0.0, 6.0, -139.0), Vector3(280.0, 12.0, 1.0), Color(0.65, 0.65, 0.7))
	_add_box(sub, Vector3(0.0, 6.0, 139.0), Vector3(280.0, 12.0, 1.0), Color(0.65, 0.65, 0.7))
	_add_box(sub, Vector3(-139.0, 6.0, 0.0), Vector3(1.0, 12.0, 280.0), Color(0.65, 0.65, 0.7))
	_add_box(sub, Vector3(139.0, 6.0, 0.0), Vector3(1.0, 12.0, 280.0), Color(0.65, 0.65, 0.7))
	# Interior baffle walls creating strong Hi-Z occluded regions behind opaque geometry.
	_add_box(sub, Vector3(-36.0, 5.0, -24.0), Vector3(44.0, 10.0, 1.4), Color(0.6, 0.62, 0.65))
	_add_box(sub, Vector3(36.0, 5.0, 24.0), Vector3(44.0, 10.0, 1.4), Color(0.6, 0.62, 0.65))
	_add_box(sub, Vector3(0.0, 5.0, 0.0), Vector3(1.4, 10.0, 38.0), Color(0.6, 0.62, 0.65))

	# Spawn 4,096 lights (64 x 64): 3,072 Omni + 512 Spot + 512 barn-door Area lights (RT-shadowed to exercise pairing skip).
	var rng := RandomNumberGenerator.new()
	rng.seed = 1394096
	for i in range(4096):
		var gx := i % 64
		var gz := i / 64
		var pos := Vector3(
			-132.3 + float(gx) * 4.2 + rng.randf_range(-0.35, 0.35),
			rng.randf_range(1.6, 5.6),
			-132.3 + float(gz) * 4.2 + rng.randf_range(-0.35, 0.35)
		)
		var col := Color(rng.randf_range(0.5, 1.0), rng.randf_range(0.5, 1.0), rng.randf_range(0.5, 1.0))
		if i < 3072:
			var omni := OmniLight3D.new()
			omni.position = pos
			omni.omni_range = rng.randf_range(2.2, 3.4)
			omni.light_color = col
			omni.light_energy = rng.randf_range(1.2, 3.2)
			omni.shadow_enabled = true
			omni.shadow_method = Light3D.SHADOW_METHOD_RAY_TRACED
			sub.add_child(omni)
		elif i < 3584:
			var spot := SpotLight3D.new()
			spot.position = pos
			spot.rotation_degrees = Vector3(rng.randf_range(-65.0, 15.0), rng.randf_range(0.0, 360.0), 0.0)
			spot.spot_range = rng.randf_range(2.4, 3.8)
			spot.spot_angle = rng.randf_range(18.0, 55.0)
			spot.light_color = col
			spot.light_energy = rng.randf_range(1.4, 3.6)
			spot.shadow_enabled = true
			spot.shadow_method = Light3D.SHADOW_METHOD_RAY_TRACED
			sub.add_child(spot)
		else:
			var area := AreaLight3D.new()
			area.position = pos
			area.rotation_degrees = Vector3(rng.randf_range(-75.0, -10.0), rng.randf_range(0.0, 360.0), 0.0)
			area.area_size = Vector2(rng.randf_range(0.4, 1.0), rng.randf_range(0.3, 0.8))
			area.area_range = rng.randf_range(2.4, 3.6)
			area.barn_door_angle = rng.randf_range(18.0, 70.0)
			area.barn_door_length = rng.randf_range(0.25, 0.75)
			area.light_color = col
			area.light_energy = rng.randf_range(1.4, 3.2)
			area.shadow_enabled = true
			area.shadow_method = Light3D.SHADOW_METHOD_RAY_TRACED
			sub.add_child(area)

	var cameras := [
		{"pos": Vector3(0.0, 4.0, 96.0), "target": Vector3(0.0, 3.0, -10.0)},
		{"pos": Vector3(-92.0, 5.0, -92.0), "target": Vector3(0.0, 3.0, 0.0)},
		{"pos": Vector3(92.0, 3.5, -88.0), "target": Vector3(-16.0, 3.5, 14.0)},
		{"pos": Vector3(-88.0, 7.5, 86.0), "target": Vector3(16.0, 2.5, -18.0)},
		{"pos": Vector3(0.0, 10.5, 88.0), "target": Vector3(0.0, 1.0, -24.0)},
		{"pos": Vector3(92.0, 2.5, 0.0), "target": Vector3(-40.0, 4.0, 0.0)},
		{"pos": Vector3(-56.0, 2.2, 52.0), "target": Vector3(36.0, 4.5, -30.0)},
		{"pos": Vector3(58.0, 6.0, 64.0), "target": Vector3(-36.0, 2.0, -32.0)},
	]

	var cam_reports := []
	for idx in range(cameras.size()):
		var spec: Dictionary = cameras[idx]
		cam.look_at_from_position(spec["pos"], spec["target"], Vector3.UP)
		var res := await _request_and_wait_check()
		var img := sub.get_texture().get_image()
		var lit := _has_lit_pixels(img)

		if not bool(res.get("packed_active", false)):
			failures.append("cam%d: packed grid inactive" % idx)
		if not bool(res.get("has_hiz", false)):
			failures.append("cam%d: Hi-Z depth culling inactive" % idx)
		if int(res.get("missing_lights", -1)) != 0:
			failures.append("cam%d: missing_lights=%d" % [idx, int(res.get("missing_lights", -1))])
		if int(res.get("extra_lights", -1)) != 0:
			failures.append("cam%d: extra_lights=%d" % [idx, int(res.get("extra_lights", -1))])
		if int(res.get("mismatched_cells", -1)) != 0:
			failures.append("cam%d: mismatched_cells=%d" % [idx, int(res.get("mismatched_cells", -1))])
		if int(res.get("gpu_overflow_cells", -1)) != 0 or int(res.get("gpu_overflow_lights", -1)) != 0:
			failures.append("cam%d: overflow cells=%d lights=%d" % [idx, int(res.get("gpu_overflow_cells", -1)), int(res.get("gpu_overflow_lights", -1))])
		if int(res.get("hiz_occluded_cells", 0)) <= 0:
			failures.append("cam%d: expected hiz_occluded_cells > 0" % idx)
		if int(res.get("hiz_occluded_nonempty_cells", -1)) != 0:
			failures.append("cam%d: hiz_occluded_nonempty_cells=%d" % [idx, int(res.get("hiz_occluded_nonempty_cells", -1))])
		if not lit:
			failures.append("cam%d: rendered image has no lit pixels" % idx)

		cam_reports.append({
			"cam": idx,
			"light_count": int(res.get("light_count", 0)),
			"missing_lights": int(res.get("missing_lights", -1)),
			"extra_lights": int(res.get("extra_lights", -1)),
			"mismatched_cells": int(res.get("mismatched_cells", -1)),
			"gpu_overflow_cells": int(res.get("gpu_overflow_cells", -1)),
			"gpu_max_cell_count": int(res.get("gpu_max_cell_count", -1)),
			"gpu_total_assignments": int(res.get("gpu_total_assignments", -1)),
			"hiz_occluded_cells": int(res.get("hiz_occluded_cells", 0)),
			"hiz_occluded_nonempty_cells": int(res.get("hiz_occluded_nonempty_cells", -1)),
			"first_mismatch": res.get("first_mismatch", {}),
			"lit": lit,
		})

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return {"cameras": cam_reports}
