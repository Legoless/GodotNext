extends Node3D

## NextGI World Cache smoke check (#294, plan §21.2.1 / §26.1 / §26.2).
##
## Builds a lit Cornell-style room with saturated red and green side walls, white diffuse surfaces,
## a reflective metal box, a translucent glass pane, and an interior shadow-casting OmniLight3D
## (no ambient light, so indirect lighting and reflections come from NextGI).
##
## Modes (passed after `--`):
##   --expect=off              Default setting (world_cache=false): SRC runs, World Cache allocates 0 bytes.
##   --expect=on-full          world_cache=true, HWRT mode Full: World Cache active, SRC unallocated/skipped,
##                             valid cell counters, non-black lighting, and non-black DEBUG_DRAW_NEXTGI_WORLD_CACHE.
##   --expect=on-simplified    world_cache=true, HWRT mode Simplified Material: World Cache active using
##                             simplified surface colours with 0 material hit groups and 0 attribute stream bytes.
##   --expect=fallback-inline  world_cache=true, HWRT mode Inline: keeps SRC, reports INLINE_HWRT, 0 WC bytes.
##   --expect=fallback-swrt    world_cache=true, SWRT tier: keeps SRC, reports SOFTWARE_TIER, 0 WC bytes.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const SETTLE_FRAMES := 32
const DEBUG_FRAMES := 8

var _expect := "off"
var _failures := PackedStringArray()
var _result := {}


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--expect="):
			_expect = arg.trim_prefix("--expect=")
	_result["expect"] = _expect
	_build_scene()
	_run()


func _build_scene() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.02, 0.02, 0.03)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.nextgi_enabled = true
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 1.7, 2.35)
	cam.fov = 55.0
	add_child(cam)
	cam.look_at(Vector3(0.0, 1.45, -2.5), Vector3.UP)

	var light := OmniLight3D.new()
	light.position = Vector3(0.0, 3.3, -0.3)
	light.light_energy = 5.0
	light.omni_range = 14.0
	light.shadow_enabled = true
	add_child(light)

	var white_mat := _make_mat(Color(0.85, 0.85, 0.85), 0.85, 0.0)
	var red_mat := _make_mat(Color(0.92, 0.08, 0.06), 0.85, 0.0)
	var green_mat := _make_mat(Color(0.08, 0.90, 0.12), 0.85, 0.0)
	var mirror_mat := _make_mat(Color(0.92, 0.92, 0.94), 0.06, 0.92)
	var glass_mat := _make_mat(Color(0.80, 0.92, 1.0, 0.35), 0.08, 0.0)
	glass_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA

	# Cornell room enclosure (6m wide, 4m tall, 6.4m deep).
	_add_box("Floor", Vector3(6.0, 0.2, 6.4), Vector3(0.0, -0.1, 0.0), white_mat)
	_add_box("Ceiling", Vector3(6.0, 0.2, 6.4), Vector3(0.0, 4.1, 0.0), white_mat)
	_add_box("BackWall", Vector3(6.0, 4.0, 0.2), Vector3(0.0, 2.0, -3.1), white_mat)
	_add_box("FrontWall", Vector3(6.0, 4.0, 0.2), Vector3(0.0, 2.0, 3.3), white_mat)
	_add_box("LeftRedWall", Vector3(0.2, 4.0, 6.4), Vector3(-3.1, 2.0, 0.0), red_mat)
	_add_box("RightGreenWall", Vector3(0.2, 4.0, 6.4), Vector3(3.1, 2.0, 0.0), green_mat)

	# Interior diffuse box + reflective metallic box + translucent pane.
	var diff_box := _add_box("DiffuseBox", Vector3(1.1, 1.8, 1.1), Vector3(-0.95, 0.9, -1.15), white_mat)
	diff_box.rotation_degrees.y = 22.0
	var refl_box := _add_box("ReflectiveBox", Vector3(1.0, 1.1, 1.0), Vector3(1.0, 0.55, -0.85), mirror_mat)
	refl_box.rotation_degrees.y = -28.0
	_add_box("GlassPane", Vector3(1.1, 1.2, 0.06), Vector3(0.0, 1.2, 0.15), glass_mat)


func _make_mat(p_albedo: Color, p_roughness: float, p_metallic: float) -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = p_albedo
	mat.roughness = p_roughness
	mat.metallic = p_metallic
	return mat


func _add_box(p_name: String, p_size: Vector3, p_pos: Vector3, p_mat: Material) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.name = p_name
	var mesh := BoxMesh.new()
	mesh.size = p_size
	mi.mesh = mesh
	mi.material_override = p_mat
	mi.position = p_pos
	add_child(mi)
	return mi


func _frames(p_count: int) -> void:
	for _i in range(p_count):
		await RenderingServer.frame_post_draw


func _sample_image_stats(p_img: Image) -> Dictionary:
	var w := p_img.get_width()
	var h := p_img.get_height()
	if w <= 0 or h <= 0:
		return { "mean_luma": 0.0, "nonblack_ratio": 0.0, "max_luma": 0.0 }
	var sum_luma := 0.0
	var max_luma := 0.0
	var nonblack := 0
	var total := 0
	var step := maxi(1, mini(w, h) / 64)
	for y in range(0, h, step):
		for x in range(0, w, step):
			var c := p_img.get_pixel(x, y)
			var l := 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
			sum_luma += l
			if l > max_luma:
				max_luma = l
			if l > 0.004:
				nonblack += 1
			total += 1
	return {
		"mean_luma": sum_luma / float(maxi(total, 1)),
		"nonblack_ratio": float(nonblack) / float(maxi(total, 1)),
		"max_luma": max_luma,
	}


func _check(p_ok: bool, p_msg: String) -> void:
	if not p_ok:
		_failures.append(p_msg)


func _run() -> void:
	await _frames(SETTLE_FRAMES)
	var tier: Dictionary = RenderingServer.nextgi_get_tier_info()
	var status: Dictionary = RenderingServer.nextgi_get_status()
	var counters: Dictionary = RenderingServer.nextgi_get_counters()
	var main_img := get_viewport().get_texture().get_image()
	var main_stats := _sample_image_stats(main_img)

	_result["tier"] = String(tier.get("tier", ""))
	_result["hardware_ray_tracing_mode"] = String(tier.get("hardware_ray_tracing_mode", ""))
	_result["world_cache"] = bool(counters.get("world_cache", false))
	_result["world_cache_active"] = bool(counters.get("world_cache_active", false))
	_result["world_cache_replaces_src"] = bool(counters.get("world_cache_replaces_src", false))
	_result["world_cache_reason"] = String(counters.get("world_cache_reason", ""))
	_result["world_cache_src_allocated"] = bool(counters.get("world_cache_src_allocated", false))
	_result["world_cache_bytes"] = int(counters.get("world_cache_bytes", 0))
	_result["world_cache_dispatches"] = int(counters.get("world_cache_dispatches", 0))
	_result["world_cache_paths_traced"] = int(counters.get("world_cache_paths_traced", 0))
	_result["world_cache_cells_active"] = int(counters.get("world_cache_cells_active", 0))
	_result["world_cache_cells_updated"] = int(counters.get("world_cache_cells_updated", 0))
	_result["src_capture_texels"] = int(counters.get("src_capture_texels", 0))
	_result["src_card_count"] = int(tier.get("src_card_count", 0))
	_result["src_relight_texels"] = int(counters.get("src_relight_texels", 0))
	_result["status_world_cache_reason"] = String(status.get("world_cache_reason", ""))
	_result["main_stats"] = main_stats

	_check(float(main_stats["mean_luma"]) > 0.02, "rendered frame must be lit and non-black (mean_luma=%.4f)" % main_stats["mean_luma"])

	match _expect:
		"off":
			_check(_result["tier"] == "HWRT", "expected HWRT tier (%s)" % _result["tier"])
			_check(not _result["world_cache"], "world_cache must be false when off")
			_check(not _result["world_cache_active"], "world_cache_active must be false when off")
			_check(not _result["world_cache_replaces_src"], "world_cache_replaces_src must be false when off")
			_check(_result["world_cache_reason"] == "OFF", "world_cache_reason must be OFF (%s)" % _result["world_cache_reason"])
			_check(_result["world_cache_bytes"] == 0, "world_cache_bytes must be 0 when off (%d)" % _result["world_cache_bytes"])
			_check(_result["world_cache_dispatches"] == 0, "world_cache_dispatches must be 0 when off (%d)" % _result["world_cache_dispatches"])
			_check(_result["world_cache_src_allocated"], "SRC must stay allocated when world_cache is off")
			_check(_result["src_relight_texels"] > 0 and _result["src_card_count"] > 0, "SRC must capture and relight when world_cache is off (cards=%d relight=%d)" % [_result["src_card_count"], _result["src_relight_texels"]])
		"on-full":
			_check(_result["tier"] == "HWRT", "expected HWRT tier (%s)" % _result["tier"])
			_check(_result["hardware_ray_tracing_mode"] == "FULL", "expected FULL HWRT mode (%s)" % _result["hardware_ray_tracing_mode"])
			_check(_result["world_cache"], "world_cache must be true")
			_check(_result["world_cache_active"], "world_cache_active must be true")
			_check(_result["world_cache_replaces_src"], "world_cache_replaces_src must be true")
			_check(_result["world_cache_reason"] == "ACTIVE", "world_cache_reason must be ACTIVE (%s)" % _result["world_cache_reason"])
			_check(not _result["world_cache_src_allocated"], "SRC must NOT be allocated when World Cache is active")
			_check(_result["src_capture_texels"] == 0 and _result["src_card_count"] == 0, "SRC capture must be skipped when World Cache is active (texels=%d cards=%d)" % [_result["src_capture_texels"], _result["src_card_count"]])
			_check(_result["src_relight_texels"] == 0, "SRC relight must be skipped when World Cache is active (%d)" % _result["src_relight_texels"])
			_check(_result["world_cache_bytes"] > 0, "world_cache_bytes must be > 0 (%d)" % _result["world_cache_bytes"])
			_check(_result["world_cache_dispatches"] > 0, "world_cache_dispatches must be > 0 (%d)" % _result["world_cache_dispatches"])
			_check(_result["world_cache_paths_traced"] > 0, "world_cache_paths_traced must be > 0 (%d)" % _result["world_cache_paths_traced"])
			_check(_result["world_cache_cells_active"] > 0, "world_cache_cells_active must be > 0 (%d)" % _result["world_cache_cells_active"])
			_check(int(counters.get("hit_material_groups", 0)) == 1, "hit_material_groups must be 1 in Full mode")
			_check(bool(counters.get("hit_world_cache_raygen", false)), "hit_world_cache_raygen must be true")
			await _check_debug_view()
		"on-simplified":
			_check(_result["tier"] == "HWRT", "expected HWRT tier (%s)" % _result["tier"])
			_check(_result["hardware_ray_tracing_mode"] == "SIMPLIFIED_MATERIAL", "expected SIMPLIFIED_MATERIAL HWRT mode (%s)" % _result["hardware_ray_tracing_mode"])
			_check(_result["world_cache"], "world_cache must be true")
			_check(_result["world_cache_active"], "world_cache_active must be true in Simplified Material mode")
			_check(_result["world_cache_replaces_src"], "world_cache_replaces_src must be true in Simplified Material mode")
			_check(_result["world_cache_reason"] == "ACTIVE", "world_cache_reason must be ACTIVE (%s)" % _result["world_cache_reason"])
			_check(not _result["world_cache_src_allocated"], "SRC must NOT be allocated in Simplified Material World Cache")
			_check(_result["src_capture_texels"] == 0 and _result["src_card_count"] == 0, "SRC capture must be skipped (texels=%d cards=%d)" % [_result["src_capture_texels"], _result["src_card_count"]])
			_check(_result["src_relight_texels"] == 0, "SRC relight must be skipped (%d)" % _result["src_relight_texels"])
			_check(_result["world_cache_bytes"] > 0, "world_cache_bytes must be > 0 (%d)" % _result["world_cache_bytes"])
			_check(_result["world_cache_paths_traced"] > 0, "world_cache_paths_traced must be > 0 (%d)" % _result["world_cache_paths_traced"])
			_check(_result["world_cache_cells_active"] > 0, "world_cache_cells_active must be > 0 (%d)" % _result["world_cache_cells_active"])
			_check(int(counters.get("hit_material_groups", 1)) == 0, "no material hit groups in Simplified Material mode")
			_check(int(counters.get("hit_materials", -1)) == 0, "hit_materials must be 0 in Simplified Material mode (%d)" % counters.get("hit_materials", -1))
			_check(int(counters.get("hit_shaders", -1)) == 0, "hit_shaders must be 0 in Simplified Material mode (%d)" % counters.get("hit_shaders", -1))
			_check(int(counters.get("rt_attribute_bytes", -1)) == 0, "rt_attribute_bytes must be 0 in Simplified Material mode (%d)" % counters.get("rt_attribute_bytes", -1))
			_check(bool(counters.get("hit_world_cache_raygen", false)), "hit_world_cache_raygen must be true")
			await _check_debug_view()
		"fallback-inline":
			_check(_result["tier"] == "HWRT", "expected HWRT tier (%s)" % _result["tier"])
			_check(_result["hardware_ray_tracing_mode"] == "INLINE", "expected INLINE mode (%s)" % _result["hardware_ray_tracing_mode"])
			_check(_result["world_cache"], "world_cache requested must be true")
			_check(not _result["world_cache_active"], "world_cache_active must be false in Inline mode")
			_check(not _result["world_cache_replaces_src"], "world_cache_replaces_src must be false in Inline mode")
			_check(_result["world_cache_reason"] == "INLINE_HWRT", "world_cache_reason must be INLINE_HWRT (%s)" % _result["world_cache_reason"])
			_check(_result["world_cache_bytes"] == 0, "world_cache_bytes must be 0 in Inline fallback (%d)" % _result["world_cache_bytes"])
			_check(_result["world_cache_src_allocated"], "SRC must remain allocated in Inline fallback")
			_check(_result["src_relight_texels"] > 0 and _result["src_card_count"] > 0, "SRC must run in Inline fallback (cards=%d relight=%d)" % [_result["src_card_count"], _result["src_relight_texels"]])
		"fallback-swrt":
			_check(_result["tier"] == "SWRT", "expected SWRT tier (%s)" % _result["tier"])
			_check(_result["world_cache"], "world_cache requested must be true")
			_check(not _result["world_cache_active"], "world_cache_active must be false on SWRT")
			_check(not _result["world_cache_replaces_src"], "world_cache_replaces_src must be false on SWRT")
			_check(_result["world_cache_reason"] == "SOFTWARE_TIER", "world_cache_reason must be SOFTWARE_TIER (%s)" % _result["world_cache_reason"])
			_check(_result["world_cache_bytes"] == 0, "world_cache_bytes must be 0 on SWRT fallback (%d)" % _result["world_cache_bytes"])
			_check(_result["world_cache_src_allocated"], "SRC must remain allocated on SWRT fallback")
		_:
			_check(false, "unknown --expect mode: %s" % _expect)

	_result["pass"] = _failures.is_empty()
	_result["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)


func _check_debug_view() -> void:
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGI_WORLD_CACHE
	await _frames(DEBUG_FRAMES)
	var dbg_img := get_viewport().get_texture().get_image()
	var dbg_stats := _sample_image_stats(dbg_img)
	_result["debug_view_stats"] = dbg_stats
	_check(float(dbg_stats["mean_luma"]) > 0.005, "World Cache debug view must be non-black (mean_luma=%.4f)" % dbg_stats["mean_luma"])
	_check(float(dbg_stats["nonblack_ratio"]) > 0.10, "World Cache debug view must cover >10%% of screen (ratio=%.3f)" % dbg_stats["nonblack_ratio"])
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
	await _frames(2)
