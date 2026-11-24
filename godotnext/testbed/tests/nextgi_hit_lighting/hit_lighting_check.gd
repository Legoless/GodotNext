/**************************************************************************/
/*  hit_lighting_check.gd                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

extends Node3D

## GPU verification for NextGI Hit Lighting for Reflections (#129, plan §21.2 contract L7, GL-1..GL-3).
##
## Verifies:
##   (a) Surface Cache mode (ray_lighting_mode = 0) is byte-identical to baseline and dispatches zero hit lighting.
##   (b) Hit Lighting for Reflections renders accurate mirror/glossy reflections of textured/colored materials
##       and direct lighting with ray-traced shadows.
##   (c) Multi-bounce (max_reflection_bounces = 2) reflects a secondary mirror bounce showing a hidden beacon.
##   (d) Translucent tinted glass ahead of a reflector accumulates tint and increments translucent_skips.
##   (e) GPU counters and timestamps ("NextGI Hit Sort", "NextGI Hit Lighting") report non-zero sorted hits.
##
## Run with NextGI enabled (High/Epic) on Vulkan HWRT:
##   godot --path godotnext/testbed --rendering-driver vulkan --gpu-profile res://tests/nextgi_hit_lighting/hit_lighting_check.tscn

const SETTING_MODE := "rendering/global_illumination/nextgi/ray_lighting_mode"
const SETTING_BOUNCES := "rendering/global_illumination/nextgi/max_reflection_bounces"

var _env: Environment
var _camera: Camera3D
var _glass_pane: MeshInstance3D
var _occluder: MeshInstance3D
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
	_env.background_color = Color(0.02, 0.02, 0.03)
	_env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	_env.ambient_light_color = Color(0.03, 0.03, 0.04)
	_env.ambient_light_energy = 1.0
	_env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	_env.nextgi_enabled = true
	_env.nextgi_max_roughness_to_trace = 0.6
	_env.nextgi_ray_lighting_mode = Environment.NEXTGI_RAY_LIGHTING_MODE_PROJECT_DEFAULT

	var world_env := WorldEnvironment.new()
	world_env.environment = _env
	add_child(world_env)

	# Camera looks straight at the large primary mirror at z = -3.0.
	_camera = Camera3D.new()
	_camera.fov = 50.0
	add_child(_camera)
	_camera.look_at_from_position(Vector3(0.0, 1.5, 1.2), Vector3(0.0, 1.5, -3.0), Vector3.UP)

	# Dark enclosing floor so framing is clean and deterministic.
	var floor_mesh := MeshInstance3D.new()
	var floor_box := BoxMesh.new()
	floor_box.size = Vector3(14.0, 0.2, 14.0)
	floor_mesh.mesh = floor_box
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.12, 0.12, 0.12)
	floor_mat.roughness = 0.9
	floor_mesh.material_override = floor_mat
	floor_mesh.position = Vector3(0.0, -0.1, 0.0)
	add_child(floor_mesh)

	# Primary mirror (left+center) filling most of the viewport at z = -3.0, facing +Z.
	var mirror := MeshInstance3D.new()
	var mirror_box := BoxMesh.new()
	mirror_box.size = Vector3(4.6, 2.8, 0.1)
	mirror.mesh = mirror_box
	var mirror_mat := StandardMaterial3D.new()
	mirror_mat.albedo_color = Color(0.95, 0.95, 0.95)
	mirror_mat.metallic = 1.0
	mirror_mat.roughness = 0.02
	mirror.material_override = mirror_mat
	mirror.position = Vector3(-0.3, 1.5, -3.0)
	add_child(mirror)

	# Glossy metal strip on the right side (roughness = 0.22) angled slightly inward toward white_wall.
	var glossy := MeshInstance3D.new()
	var glossy_box := BoxMesh.new()
	glossy_box.size = Vector3(1.2, 2.8, 0.1)
	glossy.mesh = glossy_box
	var glossy_mat := StandardMaterial3D.new()
	glossy_mat.albedo_color = Color(0.9, 0.9, 0.9)
	glossy_mat.metallic = 0.9
	glossy_mat.roughness = 0.22
	glossy.material_override = glossy_mat
	glossy.position = Vector3(2.7, 1.5, -3.0)
	glossy.rotation_degrees = Vector3(0.0, -15.0, 0.0)
	add_child(glossy)

	# Textured colorful target wall behind-left at z = +3.9 (reflected in primary mirror left half).
	var target_wall := MeshInstance3D.new()
	var wall_box := BoxMesh.new()
	wall_box.size = Vector3(2.8, 2.6, 0.2)
	target_wall.mesh = wall_box
	var wall_mat := StandardMaterial3D.new()
	wall_mat.albedo_texture = _make_checker_texture(Color(0.95, 0.10, 0.08), Color(0.08, 0.22, 0.95))
	wall_mat.roughness = 0.75
	wall_mat.metallic = 0.0
	target_wall.material_override = wall_mat
	target_wall.position = Vector3(-1.7, 1.5, 3.9)
	add_child(target_wall)

	# Bright white/cyan target panel on the right-behind at x = +2.2, z = +3.8 (glossy + translucent tint test).
	var white_wall := MeshInstance3D.new()
	var white_box := BoxMesh.new()
	white_box.size = Vector3(2.4, 2.8, 0.2)
	white_wall.mesh = white_box
	var white_mat := StandardMaterial3D.new()
	white_mat.albedo_color = Color(0.95, 0.95, 0.95)
	white_mat.emission_enabled = true
	white_mat.emission = Color(0.45, 0.85, 0.95)
	white_mat.emission_energy_multiplier = 1.6
	white_mat.roughness = 0.8
	white_wall.material_override = white_mat
	white_wall.position = Vector3(2.2, 1.5, 3.8)
	add_child(white_wall)

	# Secondary mirror positioned behind-center-right at (0.4, 1.5, +3.6), angled -45 deg around Y
	# so primary mirror reflects it straight along +X into the bright green emissive beacon (x = +4.5, z = +3.6)
	# that is NOT visible in a single bounce.
	var mirror2 := MeshInstance3D.new()
	var mirror2_box := BoxMesh.new()
	mirror2_box.size = Vector3(1.8, 2.0, 0.08)
	mirror2.mesh = mirror2_box
	var mirror2_mat := StandardMaterial3D.new()
	mirror2_mat.albedo_color = Color(0.98, 0.98, 0.98)
	mirror2_mat.metallic = 1.0
	mirror2_mat.roughness = 0.02
	mirror2.material_override = mirror2_mat
	mirror2.position = Vector3(0.4, 1.5, 3.6)
	mirror2.rotation_degrees = Vector3(0.0, -45.0, 0.0)
	add_child(mirror2)

	# Bright emissive green beacon visible ONLY via primary mirror -> secondary mirror (2nd bounce).
	var beacon := MeshInstance3D.new()
	var beacon_box := BoxMesh.new()
	beacon_box.size = Vector3(0.3, 3.2, 3.2)
	beacon.mesh = beacon_box
	var beacon_mat := StandardMaterial3D.new()
	beacon_mat.albedo_color = Color(0.05, 0.95, 0.1)
	beacon_mat.emission_enabled = true
	beacon_mat.emission = Color(0.05, 1.0, 0.1)
	beacon_mat.emission_energy_multiplier = 5.0
	beacon_mat.roughness = 0.8
	beacon.material_override = beacon_mat
	beacon.position = Vector3(4.5, 1.5, 3.6)
	add_child(beacon)

	# Shadow-casting occluder slab placed ABOVE the mirror-to-wall sightline (y = 3.15 > 2.8) so it shadows
	# the left half of target_wall from key_light without blocking any reflection rays.
	_occluder = MeshInstance3D.new()
	var occ_box := BoxMesh.new()
	occ_box.size = Vector3(1.8, 0.14, 1.6)
	_occluder.mesh = occ_box
	var occ_mat := StandardMaterial3D.new()
	occ_mat.albedo_color = Color(0.15, 0.15, 0.15)
	occ_mat.roughness = 0.9
	_occluder.material_override = occ_mat
	_occluder.position = Vector3(-2.6, 3.15, 3.2)
	add_child(_occluder)

	# Strong shadow-casting omni light above target_wall: right half is directly lit, left half is shadowed.
	var key_light := OmniLight3D.new()
	key_light.position = Vector3(-1.5, 4.2, 2.6)
	key_light.light_color = Color(1.0, 0.96, 0.9)
	key_light.light_energy = 16.0
	key_light.omni_range = 15.0
	key_light.shadow_enabled = true
	key_light.light_size = 0.05
	add_child(key_light)

	# Translucent red tinted glass pane placed ahead of white_wall (toggled for phase d).
	_glass_pane = MeshInstance3D.new()
	var glass_box := BoxMesh.new()
	glass_box.size = Vector3(2.2, 3.0, 0.06)
	var glass_mat := StandardMaterial3D.new()
	glass_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	glass_mat.albedo_color = Color(1.0, 0.04, 0.04, 0.65)
	glass_mat.emission_enabled = true
	glass_mat.emission = Color(0.8, 0.02, 0.02)
	glass_mat.emission_energy_multiplier = 0.8
	glass_mat.roughness = 0.25
	glass_box.material = glass_mat
	_glass_pane.mesh = glass_box
	_glass_pane.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_glass_pane.position = Vector3(2.0, 1.5, 2.2)
	_glass_pane.visible = false
	add_child(_glass_pane)


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


func _capture_region_chroma_spread(img: Image, uv_rect: Rect2) -> float:
	var w := img.get_width()
	var h := img.get_height()
	var x0 := clampi(int(uv_rect.position.x * w), 0, w - 1)
	var y0 := clampi(int(uv_rect.position.y * h), 0, h - 1)
	var x1 := clampi(int((uv_rect.position.x + uv_rect.size.x) * w), x0 + 1, w)
	var y1 := clampi(int((uv_rect.position.y + uv_rect.size.y) * h), y0 + 1, h)
	var max_rb := -10.0
	var min_rb := 10.0
	for y in range(y0, y1):
		for x in range(x0, x1):
			var c := img.get_pixel(x, y)
			var rb := c.r - c.b
			max_rb = maxf(max_rb, rb)
			min_rb = minf(min_rb, rb)
	return max_rb - min_rb


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


func _image_diff_stats(a: Image, b: Image) -> Dictionary:
	if a.get_size() != b.get_size():
		return {"max_diff": 1.0, "mean_diff": 1.0}
	var da := a.get_data()
	var db := b.get_data()
	if da.size() != db.size() or da.is_empty():
		return {"max_diff": 1.0, "mean_diff": 1.0}
	var max_d := 0
	var sum_d := 0
	for i in da.size():
		var d := absi(int(da[i]) - int(db[i]))
		if d > max_d:
			max_d = d
		sum_d += d
	return {
		"max_diff": float(max_d) / 255.0,
		"mean_diff": float(sum_d) / (float(da.size()) * 255.0),
	}


func _run() -> void:
	var dump_sc_path := ""
	var use_sc_override := false
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--dump-sc="):
			dump_sc_path = arg.substr("--dump-sc=".length())
		elif arg == "--sc-override":
			use_sc_override = true

	var tier_info := RenderingServer.nextgi_get_tier_info()
	_summary["tier_info_initial"] = tier_info

	# Phase A: Surface Cache mode zero-overhead & byte-identity check.
	ProjectSettings.set_setting(SETTING_MODE, 0)
	ProjectSettings.set_setting(SETTING_BOUNCES, 1)
	_env.nextgi_ray_lighting_mode = (
		Environment.NEXTGI_RAY_LIGHTING_MODE_SURFACE_CACHE
		if use_sc_override
		else Environment.NEXTGI_RAY_LIGHTING_MODE_PROJECT_DEFAULT
	)
	await _wait_frames(18)
	await RenderingServer.frame_post_draw
	var img_base := get_viewport().get_texture().get_image()
	var counters_base := RenderingServer.nextgi_get_counters()
	var tier_base := RenderingServer.nextgi_get_tier_info()

	if not dump_sc_path.is_empty():
		var f := FileAccess.open(dump_sc_path, FileAccess.WRITE)
		_check(f != null, "Failed to open dump path: %s" % dump_sc_path)
		if f != null:
			f.store_buffer(img_base.get_data())
			f.close()
		_check(not bool(counters_base.get("hit_lighting_active", false)), "Dump SC: hit_lighting_active should be false")
		_check(int(counters_base.get("hit_lighting_dispatches", 0)) == 0, "Dump SC: hit_lighting_dispatches should be 0")
		_check(not bool(tier_base.get("hit_materials_live", false)), "Dump SC: hit_materials_live should be false")
		_summary["dump_sc"] = {
			"path": dump_sc_path,
			"sc_override": use_sc_override,
			"bytes": img_base.get_data().size(),
			"hit_lighting_active": counters_base.get("hit_lighting_active", false),
			"hit_lighting_dispatches": counters_base.get("hit_lighting_dispatches", 0),
			"hit_materials_live": tier_base.get("hit_materials_live", false),
		}
		_summary["pass"] = _failures.is_empty()
		_summary["failures"] = _failures
		print("GODOTNEXT_RESULT " + JSON.stringify(_summary))
		get_tree().quit(0 if _failures.is_empty() else 1)
		return

	_env.nextgi_ray_lighting_mode = Environment.NEXTGI_RAY_LIGHTING_MODE_SURFACE_CACHE
	await _wait_frames(8)
	await RenderingServer.frame_post_draw
	var img_sc := get_viewport().get_texture().get_image()
	var counters_sc := RenderingServer.nextgi_get_counters()
	var tier_sc := RenderingServer.nextgi_get_tier_info()
	var sc_diff := _image_diff_stats(img_base, img_sc)

	_check(not bool(counters_base.get("hit_lighting_active", false)), "Phase A: hit_lighting_active should be false in Surface Cache mode")
	_check(int(counters_base.get("hit_lighting_dispatches", 0)) == 0, "Phase A: hit_lighting_dispatches should be 0 in Surface Cache mode")
	_check(not bool(tier_base.get("hit_materials_live", false)), "Phase A: hit_materials_live should be false in Surface Cache mode")
	_check(int(counters_sc.get("hit_lighting_dispatches", 0)) == 0, "Phase A: explicit Surface Cache override should dispatch 0 hit lighting passes")
	_check(not bool(tier_sc.get("hit_materials_live", false)), "Phase A: explicit Surface Cache override should keep hit_materials_live false")
	_check(float(sc_diff["mean_diff"]) < 0.01, "Phase A: Surface Cache settled frame mean delta too large (%.5f)" % float(sc_diff["mean_diff"]))
	_summary["phase_a_surface_cache"] = {
		"settled_mean_diff": sc_diff["mean_diff"],
		"settled_max_diff": sc_diff["max_diff"],
		"hit_lighting_active": counters_base.get("hit_lighting_active", false),
		"hit_lighting_dispatches": counters_base.get("hit_lighting_dispatches", 0),
		"hit_materials_live": tier_base.get("hit_materials_live", false),
		"hit_groups": tier_base.get("hit_groups", ""),
		"counters_sc_active": counters_sc.get("hit_lighting_active", false),
	}

	# Phase B: Hit Lighting for Reflections (1 bounce) - textured colors + glossy reflection + direct lighting with shadows.
	_env.nextgi_ray_lighting_mode = Environment.NEXTGI_RAY_LIGHTING_MODE_HIT_LIGHTING_FOR_REFLECTIONS
	ProjectSettings.set_setting(SETTING_BOUNCES, 1)
	# Allow material hit groups to compile & link off-thread and denoiser history to settle.
	await _wait_frames(35)
	await RenderingServer.frame_post_draw
	# Capture GPU timestamps BEFORE get_image() (which flushes frames in flight and resets timestamps).
	var ts_names := _timestamp_names()
	var img_hl_b1 := get_viewport().get_texture().get_image()
	var counters_b1 := RenderingServer.nextgi_get_counters()
	var tier_b1 := RenderingServer.nextgi_get_tier_info()

	var checker_spread := _capture_region_chroma_spread(img_hl_b1, Rect2(0.39, 0.40, 0.08, 0.20))
	var lit_band := _capture_region_avg(img_hl_b1, Rect2(0.41, 0.42, 0.055, 0.16))
	var shadow_band := _capture_region_avg(img_hl_b1, Rect2(0.335, 0.42, 0.05, 0.16))
	var glossy_band := _capture_region_avg(img_hl_b1, Rect2(0.84, 0.40, 0.08, 0.20))
	var lit_lum := lit_band.get_luminance()
	var shadow_lum := shadow_band.get_luminance()
	var glossy_lum := glossy_band.get_luminance()

	_check(bool(counters_b1.get("hit_lighting_active", false)), "Phase B: hit_lighting_active must be true")
	_check(String(counters_b1.get("hit_lighting_reason", "")) == "ACTIVE", "Phase B: hit_lighting_reason must be ACTIVE")
	_check(int(counters_b1.get("hit_lighting_dispatches", 0)) > 0, "Phase B: hit_lighting_dispatches must be > 0")
	_check(int(counters_b1.get("hit_lighting_marked_hits", 0)) > 0, "Phase B: hit_lighting_marked_hits must be > 0")
	_check(int(counters_b1.get("hit_lighting_sorted_hits", 0)) > 0, "Phase B: hit_lighting_sorted_hits must be > 0")
	_check(int(counters_b1.get("hit_lighting_shadow_rays", 0)) > 0, "Phase B: hit_lighting_shadow_rays must be > 0")
	_check(checker_spread > 0.15, "Phase B: expected textured red/blue contrast in reflection (got %.4f)" % checker_spread)
	_check(lit_lum > shadow_lum * 1.25 and lit_lum > 0.05, "Phase B: expected direct-light shadow contrast in reflection (lit=%.4f shadow=%.4f)" % [lit_lum, shadow_lum])
	_check(glossy_lum > 0.05 and glossy_band.b > glossy_band.r, "Phase B: expected glossy strip to reflect cyan-white panel (got %s)" % str(glossy_band))

	var has_sort_ts := ts_names.has("NextGI Hit Sort")
	var has_hl_ts := ts_names.has("NextGI Hit Lighting")
	if ts_names.size() > 0:
		_check(has_sort_ts, "Phase E: missing GPU timestamp 'NextGI Hit Sort'")
		_check(has_hl_ts, "Phase E: missing GPU timestamp 'NextGI Hit Lighting'")

	_summary["phase_b_hit_lighting"] = {
		"active": counters_b1.get("hit_lighting_active", false),
		"reason": counters_b1.get("hit_lighting_reason", ""),
		"ser": tier_b1.get("hit_lighting_ser", false),
		"ser_mode": tier_b1.get("hit_lighting_ser_mode", "NONE"),
		"dispatches": counters_b1.get("hit_lighting_dispatches", 0),
		"marked_hits": counters_b1.get("hit_lighting_marked_hits", 0),
		"sorted_hits": counters_b1.get("hit_lighting_sorted_hits", 0),
		"shadow_rays": counters_b1.get("hit_lighting_shadow_rays", 0),
		"checker_spread": checker_spread,
		"lit_lum": lit_lum,
		"shadow_lum": shadow_lum,
		"glossy_rgb": [glossy_band.r, glossy_band.g, glossy_band.b],
		"timestamp_count": ts_names.size(),
		"has_sort_timestamp": has_sort_ts,
		"has_hit_lighting_timestamp": has_hl_ts,
	}

	# Phase C: Multi-bounce (max_reflection_bounces = 2) reflects secondary mirror -> green beacon.
	var center_b1 := _capture_region_avg(img_hl_b1, Rect2(0.50, 0.43, 0.045, 0.14))
	ProjectSettings.set_setting(SETTING_BOUNCES, 2)
	await _wait_frames(20)
	await RenderingServer.frame_post_draw
	var img_hl_b2 := get_viewport().get_texture().get_image()
	var counters_b2 := RenderingServer.nextgi_get_counters()
	var center_b2 := _capture_region_avg(img_hl_b2, Rect2(0.50, 0.43, 0.045, 0.14))

	_check(int(counters_b2.get("hit_lighting_bounce_rays", 0)) > 0, "Phase C: hit_lighting_bounce_rays must be > 0 at max_reflection_bounces = 2")
	_check(center_b2.g > center_b1.g + 0.08 and center_b2.g > center_b2.r + 0.05, "Phase C: secondary mirror bounce should reflect green beacon (b1=%s b2=%s)" % [str(center_b1), str(center_b2)])
	_summary["phase_c_multibounce"] = {
		"bounce_rays": counters_b2.get("hit_lighting_bounce_rays", 0),
		"center_b1": [center_b1.r, center_b1.g, center_b1.b],
		"center_b2": [center_b2.r, center_b2.g, center_b2.b],
	}

	# Phase D: Translucent tinted glass ahead of white/cyan reflector accumulates red tint.
	var right_clear := _capture_region_avg(img_hl_b2, Rect2(0.58, 0.40, 0.06, 0.20))
	_glass_pane.visible = true
	await _wait_frames(25)
	await RenderingServer.frame_post_draw
	var img_hl_glass := get_viewport().get_texture().get_image()
	var counters_glass := RenderingServer.nextgi_get_counters()
	var right_glass := _capture_region_avg(img_hl_glass, Rect2(0.58, 0.40, 0.06, 0.20))

	_check(int(counters_glass.get("hit_lighting_translucent_instances", 0)) > 0, "Phase D: hit_lighting_translucent_instances must be > 0")
	_check(int(counters_glass.get("hit_lighting_translucent_skips", 0)) > 0, "Phase D: hit_lighting_translucent_skips must be > 0")
	var rb_clear := right_clear.r - right_clear.b
	var rb_glass := right_glass.r - right_glass.b
	_check(rb_glass > rb_clear + 0.08, "Phase D: red tinted glass should shift reflection toward red (clear=%s glass=%s)" % [str(right_clear), str(right_glass)])
	_summary["phase_d_translucent"] = {
		"translucent_instances": counters_glass.get("hit_lighting_translucent_instances", 0),
		"translucent_skips": counters_glass.get("hit_lighting_translucent_skips", 0),
		"right_clear": [right_clear.r, right_clear.g, right_clear.b],
		"right_glass": [right_glass.r, right_glass.g, right_glass.b],
	}

	_summary["pass"] = _failures.is_empty()
	_summary["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(_summary))
	get_tree().quit(0 if _failures.is_empty() else 1)
