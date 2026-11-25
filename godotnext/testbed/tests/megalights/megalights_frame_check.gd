extends Node3D
## #143: MegaLights frame integration GPU verification (plan §21.3 contracts M1 & M6, GM-9, GM-10).
## Verifies on GPU with Vulkan validation (VVL + SyncVal) enabled:
##  1. gm9_off_identity: rendering/megalights/enabled = false allocates 0 MegaLights bytes
##     (both RenderSceneBuffersRD and megalights_get_tier_info().memory_bytes == 0), reports
##     Production status on HWRT, and renders byte-identical consecutive frames.
##  2. gm9_fallback_and_composite: with rendering/megalights/enabled = true, custom light()
##     fallback materials stay pixel-identical to MegaLights off (0 diff on single-light ROI,
##     <= 1/255 on multi-light ROI), standard PBR diffuse/specular surfaces composite MegaLights
##     accurately without double-lighting and cast ray-traced shadows, and toggling enabled back
##     to false frees all buffers (memory_bytes == 0) and restores byte-identical OFF output.
##  3. per_light_opt_out_and_shadow_method: Light3D.MEGALIGHTS_DISALLOW keeps a light in the
##     normal Forward+ light loop while MEGALIGHTS_PROJECT (RAY_TRACED and VIRTUAL) and
##     DirectionalLight3D.MEGALIGHTS_ALLOW render through MegaLights composite.
##  4. gm10_src_ray_cap: with NextGI (High) + MegaLights active and 16 shadowed MegaLights
##     lights, NextGI SRC cards relight via ray-traced card shadows (src_relight_rays > 0)
##     while staying strictly within the R2 cap (src_relight_rays <= 256000, max <= 256000).

const VIEW_SIZE := Vector2i(128, 128)


func _ready() -> void:
	get_tree().root.disable_3d = true
	_run()


func _check(failures: PackedStringArray, cond: bool, msg: String) -> void:
	if not cond:
		failures.append(msg)


func _run() -> void:
	var failures := PackedStringArray()
	var report := {
		"test": "megalights_frame",
		"pass": false,
	}

	var orig_enabled: bool = bool(ProjectSettings.get_setting("rendering/megalights/enabled"))
	var orig_spp: int = int(ProjectSettings.get_setting("rendering/megalights/samples_per_pixel"))
	var orig_ds: int = int(ProjectSettings.get_setting("rendering/megalights/downsample"))
	var orig_dir: bool = bool(ProjectSettings.get_setting("rendering/megalights/directional_lights"))
	var orig_ngi_q: int = int(ProjectSettings.get_setting("rendering/global_illumination/nextgi/quality"))

	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	ProjectSettings.set_setting("rendering/megalights/samples_per_pixel", 0)
	ProjectSettings.set_setting("rendering/megalights/downsample", 2)
	ProjectSettings.set_setting("rendering/megalights/directional_lights", false)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/quality", 2)

	report["gm9_off_identity"] = await _check_gm9_off_identity(failures)
	report["gm9_fallback_and_composite"] = await _check_gm9_fallback_and_composite(failures)
	report["per_light_opt_out_and_shadow_method"] = await _check_per_light_opt_out_and_shadow_method(failures)
	report["gm10_src_ray_cap"] = await _check_gm10_src_ray_cap(failures)

	ProjectSettings.set_setting("rendering/megalights/enabled", orig_enabled)
	ProjectSettings.set_setting("rendering/megalights/samples_per_pixel", orig_spp)
	ProjectSettings.set_setting("rendering/megalights/downsample", orig_ds)
	ProjectSettings.set_setting("rendering/megalights/directional_lights", orig_dir)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/quality", orig_ngi_q)

	report["failures"] = failures
	report["pass"] = failures.is_empty()
	print("MEGALIGHTS_FRAME_RESULT=" + JSON.stringify(report))
	get_tree().quit(0 if failures.is_empty() else 1)


func _make_viewport(size: Vector2i, enable_nextgi: bool, shadow_atlas_size: int = 2048) -> SubViewport:
	var sub := SubViewport.new()
	sub.own_world_3d = true
	sub.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	sub.msaa_3d = Viewport.MSAA_DISABLED
	sub.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	sub.positional_shadow_atlas_size = shadow_atlas_size
	sub.size = size
	add_child(sub)

	var world := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_energy = 0.0
	env.nextgi_enabled = enable_nextgi
	world.environment = env
	sub.add_child(world)
	return sub


func _add_pbr_box(parent: Node, pos: Vector3, size: Vector3, albedo: Color, roughness: float = 0.55, metallic: float = 0.0) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = size
	var mat := StandardMaterial3D.new()
	mat.albedo_color = albedo
	mat.roughness = roughness
	mat.metallic = metallic
	box.material = mat
	mi.mesh = box
	mi.position = pos
	mi.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	parent.add_child(mi)
	return mi


func _add_fallback_box(parent: Node, pos: Vector3, size: Vector3) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = size
	var shader := Shader.new()
	shader.code = """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.75, 0.35, 0.25);
	ROUGHNESS = 0.5;
	METALLIC = 0.0;
}
void light() {
	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	DIFFUSE_LIGHT += ALBEDO * LIGHT_COLOR * ATTENUATION * ndotl / 3.14159265;
}
"""
	var mat := ShaderMaterial.new()
	mat.shader = shader
	box.material = mat
	mi.mesh = box
	mi.position = pos
	mi.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	parent.add_child(mi)
	return mi


func _roi_stats(img: Image, rect: Rect2i) -> Dictionary:
	var sum_r := 0.0
	var sum_g := 0.0
	var sum_b := 0.0
	var sum_luma := 0.0
	var count := 0
	for y in range(rect.position.y, rect.position.y + rect.size.y):
		for x in range(rect.position.x, rect.position.x + rect.size.x):
			var c := img.get_pixel(x, y)
			sum_r += c.r
			sum_g += c.g
			sum_b += c.b
			sum_luma += c.r * 0.2126 + c.g * 0.7152 + c.b * 0.0722
			count += 1
	var inv := 1.0 / maxf(float(count), 1.0)
	return {
		"r": sum_r * inv,
		"g": sum_g * inv,
		"b": sum_b * inv,
		"luma": sum_luma * inv,
	}


func _roi_max_byte_diff(a: Image, b: Image, rect: Rect2i) -> int:
	var max_diff := 0
	for y in range(rect.position.y, rect.position.y + rect.size.y):
		for x in range(rect.position.x, rect.position.x + rect.size.x):
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			var dr := absi(int(round(ca.r * 255.0)) - int(round(cb.r * 255.0)))
			var dg := absi(int(round(ca.g * 255.0)) - int(round(cb.g * 255.0)))
			var db := absi(int(round(ca.b * 255.0)) - int(round(cb.b * 255.0)))
			max_diff = maxi(max_diff, maxi(dr, maxi(dg, db)))
	return max_diff


func _check_gm9_off_identity(failures: PackedStringArray) -> Dictionary:
	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	var tier_info := RenderingServer.megalights_get_tier_info()
	_check(failures, not bool(tier_info.get("enabled", true)),
			"gm9_off_identity: expected enabled=false in tier_info")
	_check(failures, int(tier_info.get("memory_bytes", -1)) == 0,
			"gm9_off_identity: expected memory_bytes=0 when disabled, got %d" % int(tier_info.get("memory_bytes", -1)))
	var is_metal := RenderingServer.get_current_rendering_driver_name() == "metal"
	var expected_tier := "HWRT_INLINE_ONLY" if is_metal else "HWRT"
	var expected_status := "Experimental" if is_metal else "Production"
	_check(failures, str(tier_info.get("tier", "")) == expected_tier,
			"gm9_off_identity: expected tier='%s', got '%s'" % [expected_tier, str(tier_info.get("tier", ""))])
	_check(failures, str(tier_info.get("status_label", "")) == expected_status,
			"gm9_off_identity: expected status_label='%s', got '%s'" % [expected_status, str(tier_info.get("status_label", ""))])

	var sub := _make_viewport(VIEW_SIZE, false, 2048)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 1.5, 5.5)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.5, -2.0), Vector3.UP)
	cam.current = true
	var probe := MlAllocProbe.new()
	var comp := Compositor.new()
	comp.compositor_effects = [probe]
	cam.compositor = comp
	sub.add_child(cam)

	_add_fallback_box(sub, Vector3(-2.0, 1.5, -2.0), Vector3(3.2, 4.0, 0.4))
	_add_pbr_box(sub, Vector3(2.0, 1.5, -2.0), Vector3(3.2, 4.0, 0.4), Color(0.65, 0.72, 0.80), 0.35, 0.15)
	var light := OmniLight3D.new()
	light.position = Vector3(0.0, 2.2, 1.5)
	light.omni_range = 16.0
	light.light_energy = 2.5
	light.shadow_enabled = true
	sub.add_child(light)

	for _i in range(4):
		await RenderingServer.frame_post_draw
	var img1 := sub.get_texture().get_image()
	await RenderingServer.frame_post_draw
	var img2 := sub.get_texture().get_image()
	var tier_after := RenderingServer.megalights_get_tier_info()

	var bytes_equal := img1 != null and img2 != null and img1.get_data() == img2.get_data()
	_check(failures, bytes_equal, "gm9_off_identity: consecutive OFF frames were not byte-identical")
	_check(failures, probe.calls > 0, "gm9_off_identity: compositor probe did not run")
	_check(failures, probe.memory_bytes == 0 and not probe.has_albedo and not probe.has_material,
			"gm9_off_identity: MegaLights buffers allocated while disabled (bytes=%d)" % probe.memory_bytes)
	_check(failures, int(tier_after.get("memory_bytes", -1)) == 0,
			"gm9_off_identity: tier_info memory_bytes=%d after OFF frames" % int(tier_after.get("memory_bytes", -1)))

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return {
		"tier_info": tier_after,
		"bytes_equal": bytes_equal,
		"probe_memory_bytes": probe.memory_bytes,
	}


func _check_gm9_fallback_and_composite(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(VIEW_SIZE, false, 2048)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 1.5, 5.5)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.5, -2.0), Vector3.UP)
	cam.current = true
	var probe := MlAllocProbe.new()
	var comp := Compositor.new()
	comp.compositor_effects = [probe]
	cam.compositor = comp
	sub.add_child(cam)

	# Left half: custom light() fallback box (M1 shading model 3).
	_add_fallback_box(sub, Vector3(-2.1, 1.5, -2.0), Vector3(3.6, 5.0, 0.4))
	# Right half: standard PBR box + occluder casting a shadow on its lower portion.
	_add_pbr_box(sub, Vector3(2.1, 1.5, -2.0), Vector3(3.6, 5.0, 0.4), Color(0.70, 0.75, 0.82), 0.40, 0.10)
	_add_pbr_box(sub, Vector3(2.1, 0.7, -0.3), Vector3(1.5, 0.9, 0.35), Color(0.25, 0.25, 0.28), 0.70, 0.0)

	# Single bright shadowed OmniLight3D first for exact 0-diff fallback verification.
	var key_light := OmniLight3D.new()
	key_light.position = Vector3(0.2, 2.8, 1.8)
	key_light.omni_range = 18.0
	key_light.light_energy = 3.2
	key_light.light_color = Color(1.0, 0.95, 0.90)
	key_light.shadow_enabled = true
	sub.add_child(key_light)

	var fallback_roi := Rect2i(14, 34, 34, 52)
	var pbr_lit_roi := Rect2i(76, 26, 34, 24)
	var pbr_shadow_roi := Rect2i(82, 74, 22, 18)

	# Phase A: MegaLights OFF baseline.
	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	for _i in range(5):
		await RenderingServer.frame_post_draw
	var img_off := sub.get_texture().get_image()
	var off_bytes := img_off.get_data()
	var off_fallback := _roi_stats(img_off, fallback_roi)
	var off_pbr_lit := _roi_stats(img_off, pbr_lit_roi)
	var off_pbr_shadow := _roi_stats(img_off, pbr_shadow_roi)

	# Phase B: MegaLights ON (Auto High = 4 spp, Half).
	ProjectSettings.set_setting("rendering/megalights/enabled", true)
	for _i in range(18):
		await RenderingServer.frame_post_draw
	var img_on := sub.get_texture().get_image()
	var tier_on := RenderingServer.megalights_get_tier_info()
	var on_fallback := _roi_stats(img_on, fallback_roi)
	var on_pbr_lit := _roi_stats(img_on, pbr_lit_roi)
	var on_pbr_shadow := _roi_stats(img_on, pbr_shadow_roi)
	var fallback_max_diff := _roi_max_byte_diff(img_off, img_on, fallback_roi)

	_check(failures, bool(tier_on.get("enabled", false)), "gm9_composite: tier_info.enabled was false")
	_check(failures, int(tier_on.get("memory_bytes", 0)) > 0 and probe.memory_bytes > 0,
			"gm9_composite: expected > 0 MegaLights memory when enabled (tier=%d probe=%d)" % [int(tier_on.get("memory_bytes", 0)), probe.memory_bytes])
	_check(failures, float(off_fallback["luma"]) > 0.08,
			"gm9_composite: fallback ROI unexpectedly dark (%.4f)" % float(off_fallback["luma"]))
	_check(failures, fallback_max_diff == 0,
			"gm9_composite: custom light() fallback ROI differed ON vs OFF (max_byte_diff=%d, off_luma=%.4f, on_luma=%.4f)" % [fallback_max_diff, float(off_fallback["luma"]), float(on_fallback["luma"])])

	# PBR lit region must be lit accurately by MegaLights composite (neither dark nor double-lit!).
	var lit_ratio := float(on_pbr_lit["luma"]) / maxf(float(off_pbr_lit["luma"]), 1e-4)
	_check(failures, float(on_pbr_lit["luma"]) > 0.10,
			"gm9_composite: PBR lit ROI too dark with MegaLights ON (%.4f)" % float(on_pbr_lit["luma"]))
	_check(failures, lit_ratio >= 0.72 and lit_ratio <= 1.35,
			"gm9_composite: PBR lit luminance ratio ON/OFF=%.3f out of [0.72, 1.35] (off=%.4f on=%.4f; double-lit or missing composite?)" % [lit_ratio, float(off_pbr_lit["luma"]), float(on_pbr_lit["luma"])])
	# Shadow region behind occluder must be significantly darker than lit PBR region.
	_check(failures, float(on_pbr_shadow["luma"]) < float(on_pbr_lit["luma"]) * 0.55,
			"gm9_composite: PBR shadow contrast too weak (lit=%.4f shadow=%.4f)" % [float(on_pbr_lit["luma"]), float(on_pbr_shadow["luma"])])

	# Phase C: Multi-light check (add 3 more shadowed local lights -> 4 total) still preserves fallback parity.
	for i in range(3):
		var extra := OmniLight3D.new()
		extra.position = Vector3(-1.8 + float(i) * 1.8, 2.2 + float(i % 2) * 0.6, 1.6)
		extra.omni_range = 16.0
		extra.light_energy = 1.4 - float(i) * 0.2
		extra.light_color = Color(0.95, 0.90, 0.85)
		extra.shadow_enabled = true
		sub.add_child(extra)

	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	for _i in range(5):
		await RenderingServer.frame_post_draw
	var img_multi_off := sub.get_texture().get_image()
	ProjectSettings.set_setting("rendering/megalights/enabled", true)
	for _i in range(18):
		await RenderingServer.frame_post_draw
	var img_multi_on := sub.get_texture().get_image()
	var multi_fallback_diff := _roi_max_byte_diff(img_multi_off, img_multi_on, fallback_roi)
	var multi_off_lit := _roi_stats(img_multi_off, pbr_lit_roi)
	var multi_on_lit := _roi_stats(img_multi_on, pbr_lit_roi)
	var multi_ratio := float(multi_on_lit["luma"]) / maxf(float(multi_off_lit["luma"]), 1e-4)
	_check(failures, multi_fallback_diff <= 1,
			"gm9_composite: multi-light fallback ROI differed ON vs OFF (max_byte_diff=%d)" % multi_fallback_diff)
	_check(failures, multi_ratio >= 0.72 and multi_ratio <= 1.35,
			"gm9_composite: multi-light PBR lit ratio ON/OFF=%.3f out of [0.72, 1.35] (off=%.4f on=%.4f)" % [multi_ratio, float(multi_off_lit["luma"]), float(multi_on_lit["luma"])])

	# Phase D: Remove extra lights, toggle OFF again -> must free buffers (0 bytes) and match baseline OFF byte-for-byte!
	for child in sub.get_children():
		if child is OmniLight3D and child != key_light:
			child.queue_free()
	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	for _i in range(6):
		await RenderingServer.frame_post_draw
	var img_off_restored := sub.get_texture().get_image()
	var tier_restored := RenderingServer.megalights_get_tier_info()
	var restored_exact := img_off_restored.get_data() == off_bytes
	_check(failures, restored_exact, "gm9_composite: toggling MegaLights back OFF was not byte-identical to initial OFF baseline")
	_check(failures, probe.memory_bytes == 0 and int(tier_restored.get("memory_bytes", -1)) == 0,
			"gm9_composite: toggling MegaLights back OFF leaked buffers (probe=%d tier=%d)" % [probe.memory_bytes, int(tier_restored.get("memory_bytes", -1))])

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return {
		"fallback_max_diff": fallback_max_diff,
		"multi_fallback_diff": multi_fallback_diff,
		"pbr_lit_off": off_pbr_lit["luma"],
		"pbr_lit_on": on_pbr_lit["luma"],
		"pbr_lit_ratio": lit_ratio,
		"pbr_shadow_off": off_pbr_shadow["luma"],
		"pbr_shadow_on": on_pbr_shadow["luma"],
		"multi_pbr_ratio": multi_ratio,
		"restored_exact": restored_exact,
		"memory_on_bytes": int(tier_on.get("memory_bytes", 0)),
		"memory_restored_bytes": int(tier_restored.get("memory_bytes", -1)),
	}


func _check_per_light_opt_out_and_shadow_method(failures: PackedStringArray) -> Dictionary:
	ProjectSettings.set_setting("rendering/megalights/enabled", true)
	ProjectSettings.set_setting("rendering/megalights/directional_lights", false)

	var sub := _make_viewport(VIEW_SIZE, false, 2048)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 1.5, 5.5)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.5, -2.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	# Full matte white wall + small shadow caster on the right.
	_add_pbr_box(sub, Vector3(0.0, 1.5, -2.0), Vector3(12.0, 8.0, 0.5), Color(0.80, 0.80, 0.80), 0.75, 0.0)
	_add_pbr_box(sub, Vector3(1.5, 1.2, -0.4), Vector3(0.9, 1.4, 0.35), Color(0.30, 0.30, 0.30), 0.70, 0.0)

	# 1. MegaLights RAY_TRACED shadowed red-warm omni light.
	var ml_rt := OmniLight3D.new()
	ml_rt.position = Vector3(1.5, 2.6, 1.8)
	ml_rt.light_color = Color(1.0, 0.45, 0.30)
	ml_rt.omni_range = 16.0
	ml_rt.light_energy = 2.4
	ml_rt.shadow_enabled = true
	ml_rt.megalights = Light3D.MEGALIGHTS_PROJECT
	ml_rt.megalights_shadow_method = Light3D.MEGALIGHTS_SHADOW_METHOD_RAY_TRACED
	sub.add_child(ml_rt)

	# 2. MegaLights VIRTUAL shadowed warm omni light (exercises VSM path alongside RT path).
	var ml_vsm := OmniLight3D.new()
	ml_vsm.position = Vector3(2.8, 2.2, 1.8)
	ml_vsm.light_color = Color(1.0, 0.85, 0.50)
	ml_vsm.omni_range = 16.0
	ml_vsm.light_energy = 1.8
	ml_vsm.shadow_enabled = true
	ml_vsm.megalights = Light3D.MEGALIGHTS_PROJECT
	ml_vsm.megalights_shadow_method = Light3D.MEGALIGHTS_SHADOW_METHOD_VIRTUAL
	sub.add_child(ml_vsm)

	# 3. DirectionalLight3D opted IN via MEGALIGHTS_ALLOW despite directional_lights=false.
	var dir_allow := DirectionalLight3D.new()
	dir_allow.rotation_degrees = Vector3(-30.0, 15.0, 0.0)
	dir_allow.light_color = Color(0.25, 0.45, 1.0)
	dir_allow.light_energy = 0.6
	dir_allow.shadow_enabled = true
	dir_allow.megalights = Light3D.MEGALIGHTS_ALLOW
	sub.add_child(dir_allow)

	# 4. Disallowed green omni light -> MUST still shade via normal Forward+ light loop!
	var disallowed := OmniLight3D.new()
	disallowed.position = Vector3(-2.0, 1.6, 1.2)
	disallowed.light_color = Color(0.05, 1.0, 0.10)
	disallowed.omni_range = 14.0
	disallowed.light_energy = 3.5
	disallowed.shadow_enabled = true
	disallowed.megalights = Light3D.MEGALIGHTS_DISALLOW
	sub.add_child(disallowed)

	var left_roi := Rect2i(14, 42, 34, 42)
	var right_lit_roi := Rect2i(82, 22, 28, 20)
	var right_shadow_roi := Rect2i(76, 62, 20, 20)

	for _i in range(16):
		await RenderingServer.frame_post_draw
	var img_with_disallow := sub.get_texture().get_image()
	var left_with := _roi_stats(img_with_disallow, left_roi)
	var right_lit := _roi_stats(img_with_disallow, right_lit_roi)
	var right_shadow := _roi_stats(img_with_disallow, right_shadow_roi)

	disallowed.visible = false
	for _i in range(12):
		await RenderingServer.frame_post_draw
	var img_without_disallow := sub.get_texture().get_image()
	var left_without := _roi_stats(img_without_disallow, left_roi)

	var green_delta := float(left_with["g"]) - float(left_without["g"])
	_check(failures, green_delta > 0.15,
			"per_light_opt_out: MEGALIGHTS_DISALLOW green light did not contribute through Forward+ (with_g=%.4f without_g=%.4f delta=%.4f)" % [float(left_with["g"]), float(left_without["g"]), green_delta])
	_check(failures, float(right_lit["r"]) > 0.15,
			"per_light_opt_out: MegaLights RAY_TRACED/VIRTUAL lights too dark on right ROI (r=%.4f)" % float(right_lit["r"]))
	_check(failures, float(right_lit["b"]) > 0.08,
			"per_light_opt_out: DirectionalLight3D MEGALIGHTS_ALLOW contributed too little blue (b=%.4f)" % float(right_lit["b"]))
	_check(failures, float(right_shadow["luma"]) < float(right_lit["luma"]) * 0.72,
			"per_light_opt_out: shadow contrast behind right caster too weak (lit=%.4f shadow=%.4f)" % [float(right_lit["luma"]), float(right_shadow["luma"])])

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return {
		"left_with_g": left_with["g"],
		"left_without_g": left_without["g"],
		"green_delta": green_delta,
		"right_lit": right_lit,
		"right_shadow": right_shadow,
	}


func _check_gm10_src_ray_cap(failures: PackedStringArray) -> Dictionary:
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/quality", 2) # High
	ProjectSettings.set_setting("rendering/megalights/enabled", true)

	var sub := _make_viewport(VIEW_SIZE, true, 1024)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 2.2, 6.2)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.5, -2.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	# Enclosed room + interior geometry so NextGI SRC captures and relights many cards.
	_add_pbr_box(sub, Vector3(0.0, -0.25, 0.0), Vector3(14.0, 0.5, 14.0), Color(0.75, 0.75, 0.75), 0.8, 0.0)
	_add_pbr_box(sub, Vector3(0.0, 5.25, 0.0), Vector3(14.0, 0.5, 14.0), Color(0.75, 0.75, 0.75), 0.8, 0.0)
	_add_pbr_box(sub, Vector3(0.0, 2.5, -4.5), Vector3(14.0, 6.0, 0.5), Color(0.75, 0.72, 0.70), 0.8, 0.0)
	_add_pbr_box(sub, Vector3(-5.0, 2.5, 0.0), Vector3(0.5, 6.0, 14.0), Color(0.78, 0.30, 0.25), 0.8, 0.0)
	_add_pbr_box(sub, Vector3(5.0, 2.5, 0.0), Vector3(0.5, 6.0, 14.0), Color(0.25, 0.72, 0.32), 0.8, 0.0)
	for i in range(6):
		_add_pbr_box(sub, Vector3(-3.0 + float(i) * 1.2, 1.0 + float(i % 2) * 0.5, -1.8 + float(i % 3) * 0.8),
				Vector3(0.8, 2.0, 0.8), Color(0.72, 0.72, 0.72), 0.7, 0.0)

	# 16 shadowed MegaLights local lights -> all route to MegaLights (no shadow-atlas reuse in SRC),
	# forcing SRC cards to trace ray-traced shadows within the R2 <= 256,000 ray cap!
	for i in range(16):
		var omni := OmniLight3D.new()
		var gx := float(i % 4) - 1.5
		var gz := float(i / 4) - 1.5
		omni.position = Vector3(gx * 2.2, 3.6 + float(i % 2) * 0.6, gz * 1.8)
		omni.omni_range = 14.0
		omni.light_energy = 1.6
		omni.light_color = Color(1.0, 0.90, 0.80)
		omni.shadow_enabled = true
		omni.light_bake_mode = Light3D.BAKE_DYNAMIC
		omni.megalights = Light3D.MEGALIGHTS_PROJECT
		sub.add_child(omni)

	var max_observed_rays := 0
	var last_info := {}
	for _i in range(28):
		await RenderingServer.frame_post_draw
		last_info = RenderingServer.nextgi_get_tier_info()
		max_observed_rays = maxi(max_observed_rays, int(last_info.get("src_relight_rays", 0)))

	var ml_count := int(last_info.get("src_light_megalights_count", 0))
	var ray_budget := int(last_info.get("src_relight_ray_budget", 0))
	var last_rays := int(last_info.get("src_relight_rays", 0))
	var max_rays := int(last_info.get("src_relight_max_rays", 0))
	var max_mask_evals := int(last_info.get("src_max_mask_evals", 0))

	_check(failures, str(last_info.get("tier", "")) == "HWRT",
			"gm10_src_ray_cap: expected NextGI tier='HWRT', got '%s'" % str(last_info.get("tier", "")))
	_check(failures, ml_count >= 16,
			"gm10_src_ray_cap: expected src_light_megalights_count >= 16, got %d" % ml_count)
	_check(failures, ray_budget > 0 and ray_budget <= 256000,
			"gm10_src_ray_cap: expected src_relight_ray_budget in (0, 256000], got %d" % ray_budget)
	_check(failures, max_observed_rays > 0 or max_rays > 0,
			"gm10_src_ray_cap: expected SRC to trace card shadow rays for MegaLights lights (last=%d max=%d)" % [last_rays, max_rays])
	_check(failures, last_rays <= 256000 and max_rays <= 256000 and max_observed_rays <= 256000,
			"gm10_src_ray_cap: SRC relight rays exceeded R2 256,000 cap (last=%d max=%d observed=%d)" % [last_rays, max_rays, max_observed_rays])
	_check(failures, max_mask_evals <= 2,
			"gm10_src_ray_cap: expected <= 2 shadowed light evaluations per texel per frame, got %d" % max_mask_evals)

	sub.queue_free()
	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	await RenderingServer.frame_post_draw
	return {
		"nextgi_tier": str(last_info.get("tier", "")),
		"src_light_megalights_count": ml_count,
		"src_relight_ray_budget": ray_budget,
		"src_relight_rays": last_rays,
		"src_relight_max_rays": max_rays,
		"max_observed_rays": max_observed_rays,
		"src_max_mask_evals": max_mask_evals,
	}


class MlAllocProbe extends CompositorEffect:
	var calls := 0
	var has_albedo := false
	var has_material := false
	var memory_bytes := 0

	func _init() -> void:
		effect_callback_type = EFFECT_CALLBACK_TYPE_POST_OPAQUE

	func _render_callback(_callback_type: int, render_data: RenderData) -> void:
		var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
		if buffers == null:
			return
		calls += 1
		has_albedo = buffers.has_texture(&"megalights", &"ml_albedo")
		has_material = buffers.has_texture(&"megalights", &"ml_material")
		memory_bytes = int(buffers.get_context_memory_bytes(&"megalights"))
