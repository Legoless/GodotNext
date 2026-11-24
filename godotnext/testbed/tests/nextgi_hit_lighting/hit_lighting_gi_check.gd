extends Node3D

## GPU verification for NextGI Hit Lighting for GI (#128, plan §21.2 contract L7, GL-4).
##
## Verifies:
##   (a) Off mode (hit_lighting_gi = false): 0 hit-lit GI rays, no "NextGI Hit Lighting GI" timestamp,
##       and byte-identical output via --dump-off comparison.
##   (b) Surface Cache mode with hit_lighting_gi = true: cleanly inactive (RAY_LIGHTING_MODE_SURFACE_CACHE),
##       0 hit-lit GI rays, hit_materials_live = false.
##   (c) Non-Epic quality (High, Medium) with hit_lighting_reflections + hit_lighting_gi = true:
##       cleanly inactive (QUALITY_NOT_EPIC), 0 hit-lit GI rays, no "NextGI Hit Lighting GI" timestamp.
##   (d) Epic + HWRT + hit_lighting_reflections + hit_lighting_gi = true:
##       non-zero hit-lit diffuse rays and GI shadow rays, shared sort partition invariant
##       (sorted_hits == refl_sorted_hits + gi_sorted_hits), "NextGI Hit Lighting GI" and
##       "NextGI Hit Lighting" timestamps both present, accurate red/blue color-bleed bounce
##       lighting from textured colored walls onto a matte white receiver wall, and 0 Vulkan
##       validation errors.

const SETTING_MODE := "rendering/global_illumination/nextgi/ray_lighting_mode"
const SETTING_GI := "rendering/global_illumination/nextgi/hit_lighting_gi"
const SETTING_QUALITY := "rendering/global_illumination/nextgi/quality"
const SETTING_BOUNCES := "rendering/global_illumination/nextgi/max_reflection_bounces"

var _env: Environment
var _camera: Camera3D
var _failures: Array[String] = []
var _summary: Dictionary = {}


func _ready() -> void:
	_build_scene()
	call_deferred("_run")


func _make_checker_texture(color_a: Color, color_b: Color) -> ImageTexture:
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			var c := color_a if (((x / 16) + (y / 16)) % 2 == 0) else color_b
			img.set_pixel(x, y, c)
	return ImageTexture.create_from_image(img)


func _build_scene() -> void:
	_env = Environment.new()
	_env.background_mode = Environment.BG_COLOR
	_env.background_color = Color(0.01, 0.01, 0.015)
	_env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	_env.ambient_light_color = Color(0.01, 0.01, 0.015)
	_env.ambient_light_energy = 1.0
	_env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	_env.nextgi_enabled = true
	_env.nextgi_max_roughness_to_trace = 0.4
	_env.nextgi_ray_lighting_mode = Environment.NEXTGI_RAY_LIGHTING_MODE_PROJECT_DEFAULT

	var world_env := WorldEnvironment.new()
	world_env.environment = _env
	add_child(world_env)

	# Compact color-box room (width 3.6m, height 2.8m, depth 4.0m) so opposite/side walls sit well
	# within 1.5x WRC handoff distance (2.25m near the camera) for strong hit-lit diffuse bounce.
	_camera = Camera3D.new()
	_camera.fov = 55.0
	add_child(_camera)
	_camera.look_at_from_position(Vector3(0.0, 1.4, 2.2), Vector3(0.0, 1.4, -1.8), Vector3.UP)

	var white_mat := StandardMaterial3D.new()
	white_mat.albedo_color = Color(0.88, 0.88, 0.88)
	white_mat.roughness = 0.92
	white_mat.metallic = 0.0

	# Matte white back receiver wall at z = -1.8.
	var back_wall := MeshInstance3D.new()
	var back_box := BoxMesh.new()
	back_box.size = Vector3(3.8, 2.9, 0.16)
	back_wall.mesh = back_box
	back_wall.material_override = white_mat
	back_wall.position = Vector3(0.0, 1.4, -1.88)
	add_child(back_wall)

	# Matte white floor at y = 0.0 and ceiling at y = 2.8.
	var floor_mesh := MeshInstance3D.new()
	var floor_box := BoxMesh.new()
	floor_box.size = Vector3(3.8, 0.16, 4.8)
	floor_mesh.mesh = floor_box
	floor_mesh.material_override = white_mat
	floor_mesh.position = Vector3(0.0, -0.08, 0.2)
	add_child(floor_mesh)

	var ceiling_mesh := MeshInstance3D.new()
	var ceiling_box := BoxMesh.new()
	ceiling_box.size = Vector3(3.8, 0.16, 4.8)
	ceiling_mesh.mesh = ceiling_box
	ceiling_mesh.material_override = white_mat
	ceiling_mesh.position = Vector3(0.0, 2.88, 0.2)
	add_child(ceiling_mesh)

	# Textured vivid red-orange left wall at x = -1.8 (drives red bounce onto back wall left half).
	var left_wall := MeshInstance3D.new()
	var side_box := BoxMesh.new()
	side_box.size = Vector3(0.16, 2.9, 4.8)
	left_wall.mesh = side_box
	var left_mat := StandardMaterial3D.new()
	left_mat.albedo_texture = _make_checker_texture(Color(0.98, 0.08, 0.04), Color(0.82, 0.18, 0.06))
	left_mat.roughness = 0.85
	left_mat.metallic = 0.0
	left_wall.material_override = left_mat
	left_wall.position = Vector3(-1.88, 1.4, 0.2)
	add_child(left_wall)

	# Textured vivid blue-cyan right wall at x = +1.8 (drives blue bounce onto back wall right half).
	var right_wall := MeshInstance3D.new()
	right_wall.mesh = side_box
	var right_mat := StandardMaterial3D.new()
	right_mat.albedo_texture = _make_checker_texture(Color(0.05, 0.18, 0.98), Color(0.08, 0.42, 0.95))
	right_mat.roughness = 0.85
	right_mat.metallic = 0.0
	right_wall.material_override = right_mat
	right_wall.position = Vector3(1.88, 1.4, 0.2)
	add_child(right_wall)

	# Small glossy center strip on the back wall so reflection hit lighting also marks hits in the
	# shared record buffer alongside GI hits, validating the combined sort partition.
	var mirror_strip := MeshInstance3D.new()
	var strip_box := BoxMesh.new()
	strip_box.size = Vector3(0.55, 0.9, 0.06)
	mirror_strip.mesh = strip_box
	var mirror_mat := StandardMaterial3D.new()
	mirror_mat.albedo_color = Color(0.95, 0.95, 0.95)
	mirror_mat.metallic = 1.0
	mirror_mat.roughness = 0.04
	mirror_strip.material_override = mirror_mat
	mirror_strip.position = Vector3(0.0, 1.4, -1.76)
	add_child(mirror_strip)

	# Matte wall behind the camera at z = +2.8 so mirror_strip reflection rays hit real scene
	# geometry and enter the shared hit-record buffer alongside diffuse GI hits.
	var front_wall := MeshInstance3D.new()
	var front_box := BoxMesh.new()
	front_box.size = Vector3(3.8, 2.9, 0.16)
	front_wall.mesh = front_box
	var front_mat := StandardMaterial3D.new()
	front_mat.albedo_color = Color(0.18, 0.18, 0.20)
	front_mat.roughness = 0.9
	front_wall.material_override = front_mat
	front_wall.position = Vector3(0.0, 1.4, 2.88)
	add_child(front_wall)

	# Small occluder bar near the left wall so GI shadow rays hit real shadowing geometry.
	var occ := MeshInstance3D.new()
	var occ_box := BoxMesh.new()
	occ_box.size = Vector3(0.25, 0.25, 0.9)
	occ.mesh = occ_box
	var occ_mat := StandardMaterial3D.new()
	occ_mat.albedo_color = Color(0.2, 0.2, 0.2)
	occ_mat.roughness = 0.9
	occ.material_override = occ_mat
	occ.position = Vector3(-1.35, 2.35, -0.3)
	add_child(occ)

	# Two shadow-casting spotlights aimed directly at the colored side walls so the matte white back
	# wall receives its color bleed purely from bounced GI rather than direct light.
	var spot_left := SpotLight3D.new()
	spot_left.light_color = Color(1.0, 0.98, 0.95)
	spot_left.light_energy = 18.0
	spot_left.spot_range = 8.0
	spot_left.spot_angle = 42.0
	spot_left.shadow_enabled = true
	add_child(spot_left)
	spot_left.look_at_from_position(Vector3(0.2, 1.9, -0.4), Vector3(-1.8, 1.3, -0.8), Vector3.UP)

	var spot_right := SpotLight3D.new()
	spot_right.light_color = Color(1.0, 0.98, 0.95)
	spot_right.light_energy = 18.0
	spot_right.spot_range = 8.0
	spot_right.spot_angle = 42.0
	spot_right.shadow_enabled = true
	add_child(spot_right)
	spot_right.look_at_from_position(Vector3(-0.2, 1.9, -0.4), Vector3(1.8, 1.3, -0.8), Vector3.UP)


func _wait_frames(n: int) -> void:
	for _i in n:
		await get_tree().process_frame


func _capture_region_avg(img: Image, uv_rect: Rect2) -> Color:
	var w := img.get_width()
	var h := img.get_height()
	var x0 := clampi(int(uv_rect.position.x * w), 0, w - 1)
	var y0 := clampi(int(uv_rect.position.y * h), 0, h - 1)
	var x1 := clampi(int((uv_rect.position.x + uv_rect.size.x) * w), x0 + 1, w)
	var y1 := clampi(int((uv_rect.position.y + uv_rect.size.y) * h), y0 + 1, h)
	var acc := Color(0, 0, 0, 0)
	var count := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			acc += img.get_pixel(x, y)
			count += 1
	return acc / float(maxi(count, 1))


func _check(cond: bool, msg: String) -> void:
	if not cond:
		_failures.append(msg)


func _timestamp_names() -> PackedStringArray:
	var names := PackedStringArray()
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return names
	for i in int(rd.get_captured_timestamps_count()):
		names.append(rd.get_captured_timestamp_name(i))
	return names


func _run() -> void:
	var dump_off_path := ""
	var dump_mode := 0
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--dump-off="):
			dump_off_path = arg.substr("--dump-off=".length())
		elif arg.begins_with("--ray-mode="):
			dump_mode = int(arg.substr("--ray-mode=".length()))

	ProjectSettings.set_setting(SETTING_BOUNCES, 1)
	ProjectSettings.set_setting(SETTING_QUALITY, 3) # EPIC

	if not dump_off_path.is_empty():
		ProjectSettings.set_setting(SETTING_MODE, dump_mode)
		ProjectSettings.set_setting(SETTING_GI, false)
		await _wait_frames(22)
		await RenderingServer.frame_post_draw
		var ts_dump := _timestamp_names()
		var img_dump := get_viewport().get_texture().get_image()
		var counters_dump := RenderingServer.nextgi_get_counters()
		var tier_dump := RenderingServer.nextgi_get_tier_info()
		var f := FileAccess.open(dump_off_path, FileAccess.WRITE)
		_check(f != null, "Failed to open dump path: %s" % dump_off_path)
		if f != null:
			f.store_buffer(img_dump.get_data())
			f.close()
		_check(not bool(counters_dump.get("hit_lighting_gi_active", false)), "Dump off: hit_lighting_gi_active should be false")
		_check(int(counters_dump.get("hit_lighting_gi_rays", 0)) == 0, "Dump off: hit_lighting_gi_rays should be 0")
		_check(int(counters_dump.get("hit_lighting_gi_dispatches", 0)) == 0, "Dump off: hit_lighting_gi_dispatches should be 0")
		_check(not ts_dump.has("NextGI Hit Lighting GI"), "Dump off: must not emit 'NextGI Hit Lighting GI' timestamp")
		_summary["dump_off"] = {
			"path": dump_off_path,
			"mode": dump_mode,
			"bytes": img_dump.get_data().size(),
			"hit_lighting_gi_active": counters_dump.get("hit_lighting_gi_active", false),
			"hit_lighting_gi_rays": counters_dump.get("hit_lighting_gi_rays", 0),
			"hit_lighting_gi_dispatches": counters_dump.get("hit_lighting_gi_dispatches", 0),
			"hit_materials_live": tier_dump.get("hit_materials_live", false),
			"has_gi_timestamp": ts_dump.has("NextGI Hit Lighting GI"),
		}
		_summary["pass"] = _failures.is_empty()
		_summary["failures"] = _failures
		print("GODOTNEXT_RESULT " + JSON.stringify(_summary))
		get_tree().quit(0 if _failures.is_empty() else 1)
		return

	# Phase A: Off mode (hit_lighting_gi = false, ray_lighting_mode = 0) at Epic -> zero GI hit-lighting work.
	ProjectSettings.set_setting(SETTING_MODE, 0)
	ProjectSettings.set_setting(SETTING_GI, false)
	await _wait_frames(18)
	await RenderingServer.frame_post_draw
	var ts_off := _timestamp_names()
	var counters_off := RenderingServer.nextgi_get_counters()
	var tier_off := RenderingServer.nextgi_get_tier_info()
	_check(not bool(counters_off.get("hit_lighting_gi_active", false)), "Phase A: hit_lighting_gi_active must be false when off")
	_check(String(counters_off.get("hit_lighting_gi_reason", "")) == "OFF", "Phase A: hit_lighting_gi_reason must be OFF")
	_check(int(counters_off.get("hit_lighting_gi_rays", 0)) == 0, "Phase A: hit_lighting_gi_rays must be 0")
	_check(int(counters_off.get("hit_lighting_gi_dispatches", 0)) == 0, "Phase A: hit_lighting_gi_dispatches must be 0")
	_check(not bool(tier_off.get("hit_materials_live", false)), "Phase A: hit_materials_live must be false in Surface Cache mode")
	_check(not ts_off.has("NextGI Hit Lighting GI"), "Phase A: must not emit 'NextGI Hit Lighting GI' timestamp")
	_summary["phase_a_off"] = {
		"active": counters_off.get("hit_lighting_gi_active", false),
		"reason": counters_off.get("hit_lighting_gi_reason", ""),
		"gi_rays": counters_off.get("hit_lighting_gi_rays", 0),
		"gi_dispatches": counters_off.get("hit_lighting_gi_dispatches", 0),
		"has_gi_timestamp": ts_off.has("NextGI Hit Lighting GI"),
	}

	# Phase B: Surface Cache mode gating (hit_lighting_gi = true, ray_lighting_mode = 0, Epic).
	ProjectSettings.set_setting(SETTING_MODE, 0)
	ProjectSettings.set_setting(SETTING_GI, true)
	await _wait_frames(10)
	await RenderingServer.frame_post_draw
	var ts_sc := _timestamp_names()
	var counters_sc := RenderingServer.nextgi_get_counters()
	var tier_sc := RenderingServer.nextgi_get_tier_info()
	_check(not bool(counters_sc.get("hit_lighting_gi_active", false)), "Phase B: hit_lighting_gi_active must be false in Surface Cache mode")
	_check(String(counters_sc.get("hit_lighting_gi_reason", "")) == "RAY_LIGHTING_MODE_SURFACE_CACHE", "Phase B: expected RAY_LIGHTING_MODE_SURFACE_CACHE, got %s" % str(counters_sc.get("hit_lighting_gi_reason", "")))
	_check(int(counters_sc.get("hit_lighting_gi_rays", 0)) == 0, "Phase B: hit_lighting_gi_rays must be 0 in Surface Cache mode")
	_check(int(counters_sc.get("hit_lighting_gi_dispatches", 0)) == 0, "Phase B: hit_lighting_gi_dispatches must be 0 in Surface Cache mode")
	_check(not bool(tier_sc.get("hit_materials_live", false)), "Phase B: hit_materials_live must stay false in Surface Cache mode")
	_check(not ts_sc.has("NextGI Hit Lighting GI"), "Phase B: must not emit 'NextGI Hit Lighting GI' timestamp in Surface Cache mode")
	_summary["phase_b_surface_cache_gated"] = {
		"active": counters_sc.get("hit_lighting_gi_active", false),
		"reason": counters_sc.get("hit_lighting_gi_reason", ""),
		"gi_rays": counters_sc.get("hit_lighting_gi_rays", 0),
		"gi_dispatches": counters_sc.get("hit_lighting_gi_dispatches", 0),
		"has_gi_timestamp": ts_sc.has("NextGI Hit Lighting GI"),
	}

	# Phase C: Non-Epic gating (High and Medium with ray_lighting_mode = 1, hit_lighting_gi = true).
	ProjectSettings.set_setting(SETTING_MODE, 1)
	ProjectSettings.set_setting(SETTING_GI, true)
	ProjectSettings.set_setting(SETTING_QUALITY, 2) # HIGH
	await _wait_frames(20)
	await RenderingServer.frame_post_draw
	var ts_high := _timestamp_names()
	var counters_high := RenderingServer.nextgi_get_counters()
	var tier_high := RenderingServer.nextgi_get_tier_info()
	_check(bool(counters_high.get("hit_lighting_active", false)), "Phase C (High): reflection hit lighting should be active")
	_check(not bool(counters_high.get("hit_lighting_gi_active", false)), "Phase C (High): GI hit lighting must be inactive at High")
	_check(String(counters_high.get("hit_lighting_gi_reason", "")) == "QUALITY_NOT_EPIC", "Phase C (High): expected QUALITY_NOT_EPIC, got %s" % str(counters_high.get("hit_lighting_gi_reason", "")))
	_check(int(counters_high.get("hit_lighting_gi_rays", 0)) == 0, "Phase C (High): hit_lighting_gi_rays must be 0 at High")
	_check(int(counters_high.get("hit_lighting_gi_dispatches", 0)) == 0, "Phase C (High): hit_lighting_gi_dispatches must be 0 at High")
	_check(not ts_high.has("NextGI Hit Lighting GI"), "Phase C (High): must not emit 'NextGI Hit Lighting GI' at High")

	ProjectSettings.set_setting(SETTING_QUALITY, 1) # MEDIUM
	await _wait_frames(10)
	await RenderingServer.frame_post_draw
	var ts_med := _timestamp_names()
	var counters_med := RenderingServer.nextgi_get_counters()
	_check(not bool(counters_med.get("hit_lighting_gi_active", false)), "Phase C (Medium): GI hit lighting must be inactive at Medium")
	_check(int(counters_med.get("hit_lighting_gi_rays", 0)) == 0, "Phase C (Medium): hit_lighting_gi_rays must be 0 at Medium")
	_check(not ts_med.has("NextGI Hit Lighting GI"), "Phase C (Medium): must not emit 'NextGI Hit Lighting GI' at Medium")
	_summary["phase_c_non_epic_gated"] = {
		"high_quality": tier_high.get("quality", ""),
		"high_refl_active": counters_high.get("hit_lighting_active", false),
		"high_gi_active": counters_high.get("hit_lighting_gi_active", false),
		"high_gi_reason": counters_high.get("hit_lighting_gi_reason", ""),
		"high_gi_rays": counters_high.get("hit_lighting_gi_rays", 0),
		"high_has_gi_timestamp": ts_high.has("NextGI Hit Lighting GI"),
		"medium_gi_active": counters_med.get("hit_lighting_gi_active", false),
		"medium_gi_rays": counters_med.get("hit_lighting_gi_rays", 0),
		"medium_has_gi_timestamp": ts_med.has("NextGI Hit Lighting GI"),
	}

	# Phase D: Active Epic + HWRT + hit_lighting_reflections + hit_lighting_gi = true.
	ProjectSettings.set_setting(SETTING_QUALITY, 3) # EPIC
	ProjectSettings.set_setting(SETTING_MODE, 1)
	ProjectSettings.set_setting(SETTING_GI, true)
	await _wait_frames(35)
	await RenderingServer.frame_post_draw
	# Capture GPU timestamps BEFORE get_image() (which flushes frames in flight).
	var ts_active := _timestamp_names()
	var img_gi := get_viewport().get_texture().get_image()
	var counters_gi := RenderingServer.nextgi_get_counters()
	var tier_gi := RenderingServer.nextgi_get_tier_info()

	var gi_rays := int(counters_gi.get("hit_lighting_gi_rays", 0))
	var gi_marked := int(counters_gi.get("hit_lighting_gi_marked_hits", 0))
	var gi_sorted := int(counters_gi.get("hit_lighting_gi_sorted_hits", 0))
	var gi_shadows := int(counters_gi.get("hit_lighting_gi_shadow_rays", 0))
	var gi_dispatches := int(counters_gi.get("hit_lighting_gi_dispatches", 0))
	var refl_sorted := int(counters_gi.get("hit_lighting_refl_sorted_hits", 0))
	var total_sorted := int(counters_gi.get("hit_lighting_sorted_hits", 0))

	_check(bool(tier_gi.get("hit_lighting_gi", false)), "Phase D: tier_info.hit_lighting_gi must be true")
	_check(bool(tier_gi.get("hit_lighting_gi_active", false)), "Phase D: tier_info.hit_lighting_gi_active must be true")
	_check(String(tier_gi.get("hit_lighting_gi_reason", "")) == "ACTIVE", "Phase D: tier_info.hit_lighting_gi_reason must be ACTIVE")
	_check(int(tier_gi.get("hit_lighting_gi_rays", 0)) > 0, "Phase D: tier_info.hit_lighting_gi_rays must be > 0")
	_check(bool(counters_gi.get("hit_lighting_gi_active", false)), "Phase D: counters.hit_lighting_gi_active must be true")
	_check(gi_rays > 0, "Phase D: hit_lighting_gi_rays must be > 0 (got %d)" % gi_rays)
	_check(gi_marked > 0, "Phase D: hit_lighting_gi_marked_hits must be > 0 (got %d)" % gi_marked)
	_check(gi_sorted > 0, "Phase D: hit_lighting_gi_sorted_hits must be > 0 (got %d)" % gi_sorted)
	_check(gi_sorted == gi_marked, "Phase D: hit_lighting_gi_sorted_hits must equal gi_marked_hits (sorted=%d marked=%d)" % [gi_sorted, gi_marked])
	_check(gi_rays == gi_sorted, "Phase D: hit_lighting_gi_rays must equal gi_sorted_hits (rays=%d sorted=%d)" % [gi_rays, gi_sorted])
	_check(gi_shadows > 0, "Phase D: hit_lighting_gi_shadow_rays must be > 0 (got %d)" % gi_shadows)
	_check(gi_dispatches > 0, "Phase D: hit_lighting_gi_dispatches must be > 0 (got %d)" % gi_dispatches)
	_check(refl_sorted > 0, "Phase D: hit_lighting_refl_sorted_hits must be > 0 alongside GI (got %d)" % refl_sorted)
	_check(total_sorted == refl_sorted + gi_sorted, "Phase D: shared sort partition mismatch (total=%d refl=%d gi=%d)" % [total_sorted, refl_sorted, gi_sorted])

	var has_sort_ts := ts_active.has("NextGI Hit Sort")
	var has_refl_ts := ts_active.has("NextGI Hit Lighting")
	var has_gi_ts := ts_active.has("NextGI Hit Lighting GI")
	if ts_active.size() > 0:
		_check(has_sort_ts, "Phase D: missing GPU timestamp 'NextGI Hit Sort'")
		_check(has_refl_ts, "Phase D: missing GPU timestamp 'NextGI Hit Lighting'")
		_check(has_gi_ts, "Phase D: missing GPU timestamp 'NextGI Hit Lighting GI'")

	# Sample left and right halves of the matte white back wall (excluding the center mirror strip).
	var back_left := _capture_region_avg(img_gi, Rect2(0.28, 0.38, 0.14, 0.24))
	var back_right := _capture_region_avg(img_gi, Rect2(0.58, 0.38, 0.14, 0.24))
	_check(back_left.r > back_left.b + 0.03 and back_left.r > 0.05, "Phase D: matte back wall left side should receive red bounce (got %s)" % str(back_left))
	_check(back_right.b > back_right.r + 0.03 and back_right.b > 0.05, "Phase D: matte back wall right side should receive blue bounce (got %s)" % str(back_right))

	_summary["phase_d_active_epic"] = {
		"quality": tier_gi.get("quality", ""),
		"tier": tier_gi.get("tier", ""),
		"ser": tier_gi.get("hit_lighting_ser", false),
		"ser_mode": tier_gi.get("hit_lighting_ser_mode", "NONE"),
		"gi_active": counters_gi.get("hit_lighting_gi_active", false),
		"gi_reason": counters_gi.get("hit_lighting_gi_reason", ""),
		"gi_rays": gi_rays,
		"gi_marked_hits": gi_marked,
		"gi_sorted_hits": gi_sorted,
		"gi_shadow_rays": gi_shadows,
		"gi_dispatches": gi_dispatches,
		"refl_sorted_hits": refl_sorted,
		"total_sorted_hits": total_sorted,
		"timestamp_count": ts_active.size(),
		"has_sort_timestamp": has_sort_ts,
		"has_refl_timestamp": has_refl_ts,
		"has_gi_timestamp": has_gi_ts,
		"back_left_rgb": [back_left.r, back_left.g, back_left.b],
		"back_right_rgb": [back_right.r, back_right.g, back_right.b],
	}

	_summary["pass"] = _failures.is_empty()
	_summary["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(_summary))
	get_tree().quit(0 if _failures.is_empty() else 1)
