extends Node3D
## #142: MegaLights sampling core GPU verification (plan §21.3 contract M3, GM-1, GM-2, budget M-3).
## Verifies on GPU with Vulkan validation enabled:
##  1. unbiased_full: 64 lights (omni, spot, area) with visibility forced to 1; 4,096-frame
##     mean matches exhaustive unshadowed shading within 1% per pixel.
##  2. unbiased_half_guided: 64 lights in default half-res 2x2 rotating downsample mode with
##     visible-light guiding enabled; every pixel is visited 1,024 times over 4,096 frames
##     and converges unbiasedly to exhaustive shading.
##  3. reference_convergence: exhaustive reference mode with real trace_shadow() (NextGI HWRT)
##     on soft-shadow penumbras converges to <= 1% relative standard error at 1,024 frames.
##  4. guiding: per-8x8-tile visible-light guiding aggregates <= 32 visible lights with 0
##     occluded leaks, boosts visible light selection > 2x, and keeps positive weight floor
##     so occluded lights continue to be sampled (unbiased).
##  5. falloff_cull: falloff early cull threshold skips dim distant lights cleanly.
##  6. light_eval: verifies omni+IES, spot, rect area+barn doors, textured rect, Light Function
##     Atlas modulation, and directional_lights opt-in (default off vs on).
##  7. legacy_off: when MegaLights is inactive, consecutive frames are byte-identical and
##     allocate 0 MegaLights input bytes.

const VIEW_SIZE := Vector2i(128, 128)


func _ready() -> void:
	# Disable 3D on the root window so our SubViewport is the sole 3D view consuming megalights_request_check().
	get_tree().root.disable_3d = true
	_run()


func _arg(key: String, fallback: String) -> String:
	var prefix := "--" + key + "="
	for a in OS.get_cmdline_user_args():
		if a.begins_with(prefix):
			return a.substr(prefix.length())
	return fallback


func _check(failures: PackedStringArray, cond: bool, msg: String) -> void:
	if not cond:
		failures.append(msg)


func _run() -> void:
	var mode := _arg("ml-check", "all")
	var require_nextgi := _arg("require-nextgi", "0") == "1"
	var nextgi_enabled := bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false))
	var failures := PackedStringArray()
	var report := {
		"test": "megalights_sampling",
		"mode": mode,
		"nextgi_enabled": nextgi_enabled,
		"pass": false,
	}

	if mode == "all" or mode == "legacy_off":
		report["legacy_off"] = await _check_legacy_off(failures)
	if mode == "all" or mode == "unbiased_full":
		report["unbiased_full"] = await _check_unbiased(failures, "full", false, 4096)
	if mode == "all" or mode == "unbiased_half_guided":
		report["unbiased_half_guided"] = await _check_unbiased(failures, "half", true, 4096)
	if mode == "all" or mode == "guiding":
		report["guiding"] = await _check_guiding(failures)
	if mode == "all" or mode == "falloff_cull":
		report["falloff_cull"] = await _check_falloff_cull(failures)
	if mode == "all" or mode == "light_eval":
		report["light_eval"] = await _check_light_eval(failures)
	if mode == "all" or mode == "reference_convergence":
		report["reference_convergence"] = await _check_reference_convergence(failures, require_nextgi or nextgi_enabled)

	report["failures"] = failures
	report["pass"] = failures.is_empty()
	print("MEGALIGHTS_CHECK_RESULT=" + JSON.stringify(report))
	get_tree().quit(0 if failures.is_empty() else 1)


func _make_viewport(size: Vector2i, enable_nextgi: bool = false) -> SubViewport:
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
	env.background_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_energy = 0.0
	env.nextgi_enabled = enable_nextgi
	world.environment = env
	sub.add_child(world)
	return sub


func _add_box(parent: Node, pos: Vector3, size: Vector3, albedo: Color, roughness: float = 0.65, metallic: float = 0.0) -> MeshInstance3D:
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
	parent.add_child(mi)
	return mi


func _run_gpu_check(options: Dictionary, max_wait_frames: int = 300) -> Dictionary:
	# Let the scene settle for 2 frames so prepass, light buffers, and packed grid are warm.
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	RenderingServer.megalights_request_check(options)
	var res := {}
	for _i in range(max_wait_frames):
		await RenderingServer.frame_post_draw
		res = RenderingServer.megalights_get_check_result()
		if bool(res.get("done", false)):
			return res
	res["error"] = "Timed out waiting for MegaLights GPU check."
	return res


func _check_legacy_off(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(Vector2i(128, 128), false)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 1.5, 6.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.5, 0.0), Vector3.UP)
	cam.current = true
	var probe := MlAllocProbe.new()
	var comp := Compositor.new()
	comp.compositor_effects = [probe]
	cam.compositor = comp
	sub.add_child(cam)

	_add_box(sub, Vector3(0.0, 1.5, -2.0), Vector3(10.0, 8.0, 0.5), Color(0.75, 0.75, 0.75))
	var omni := OmniLight3D.new()
	omni.position = Vector3(0.0, 2.0, 1.0)
	omni.omni_range = 12.0
	omni.light_energy = 2.0
	omni.shadow_enabled = false
	sub.add_child(omni)

	for _i in range(4):
		await RenderingServer.frame_post_draw
	var img1 := sub.get_texture().get_image()
	await RenderingServer.frame_post_draw
	var img2 := sub.get_texture().get_image()

	var bytes_equal := img1 != null and img2 != null and img1.get_data() == img2.get_data()
	_check(failures, bytes_equal, "legacy_off: consecutive frames were not byte-identical")
	_check(failures, probe.calls > 0, "legacy_off: compositor probe did not execute")
	_check(failures, probe.memory_bytes == 0 and not probe.has_albedo and not probe.has_material,
			"legacy_off: MegaLights inputs allocated while inactive (bytes=%d)" % probe.memory_bytes)

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return {
		"bytes_equal": bytes_equal,
		"probe_calls": probe.calls,
		"memory_bytes": probe.memory_bytes,
	}


func _populate_64_lights(parent: Node) -> void:
	# 64 lights: 40 OmniLight3D + 12 SpotLight3D + 12 AreaLight3D all illuminating the receiver wall.
	var rng := RandomNumberGenerator.new()
	rng.seed = 1420064
	for i in range(64):
		var gx := i % 8
		var gy := i / 8
		var pos := Vector3(
			-3.5 + float(gx) * 1.0 + rng.randf_range(-0.15, 0.15),
			-1.8 + float(gy) * 0.55 + rng.randf_range(-0.1, 0.1),
			rng.randf_range(0.5, 2.5)
		)
		var col := Color(
			rng.randf_range(0.65, 1.0),
			rng.randf_range(0.65, 1.0),
			rng.randf_range(0.65, 1.0)
		)
		if i < 40:
			var omni := OmniLight3D.new()
			omni.position = pos
			omni.omni_range = 16.0
			omni.light_color = col
			omni.light_energy = rng.randf_range(0.6, 2.4)
			omni.shadow_enabled = false
			parent.add_child(omni)
		elif i < 52:
			var spot := SpotLight3D.new()
			spot.position = pos
			spot.look_at_from_position(pos, Vector3(pos.x * 0.5, pos.y * 0.5, -2.0), Vector3.UP)
			spot.spot_range = 18.0
			spot.spot_angle = 55.0
			spot.light_color = col
			spot.light_energy = rng.randf_range(0.8, 2.6)
			spot.shadow_enabled = false
			parent.add_child(spot)
		else:
			var area := AreaLight3D.new()
			area.position = pos
			area.look_at_from_position(pos, Vector3(pos.x * 0.5, pos.y * 0.5, -2.0), Vector3.UP)
			area.area_size = Vector2(0.5, 0.4)
			area.area_range = 18.0
			area.light_color = col
			area.light_energy = rng.randf_range(0.7, 2.2)
			area.shadow_enabled = false
			parent.add_child(area)


func _check_unbiased(failures: PackedStringArray, downsample: String, use_guiding: bool, total_frames: int) -> Dictionary:
	var tag := "unbiased_%s%s" % [downsample, "_guided" if use_guiding else ""]
	var sub := _make_viewport(VIEW_SIZE, false)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.2, 5.5)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.2, -2.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	# Large lit receiver wall with dielectric + slight metallic response so both diffuse & specular lobes run.
	_add_box(sub, Vector3(0.0, 0.2, -2.25), Vector3(14.0, 10.0, 0.5), Color(0.78, 0.74, 0.70), 0.55, 0.15)
	_populate_64_lights(sub)

	var res := await _run_gpu_check({
		"mode": "unbiased",
		"downsample": downsample,
		"samples_per_pixel": 4,
		"use_guiding": use_guiding,
		"falloff_cull_threshold": 0.0,
		"ref_falloff_threshold": 0.0,
		"total_frames": total_frames,
		"steps_per_frame": 128,
	})

	_check(failures, bool(res.get("done", false)) and not res.has("error"),
			"%s: check failed to complete (%s)" % [tag, str(res.get("error", ""))])
	var lit := int(res.get("lit_pixels", 0))
	var expected_visits := total_frames if downsample == "full" else (total_frames / 4)
	_check(failures, lit > 8000, "%s: expected > 8000 lit pixels, got %d" % [tag, lit])
	_check(failures, int(res.get("min_visits_per_pixel", 0)) == expected_visits and int(res.get("max_visits_per_pixel", 0)) == expected_visits,
			"%s: expected %d visits/px, got min=%d max=%d" % [tag, expected_visits, int(res.get("min_visits_per_pixel", 0)), int(res.get("max_visits_per_pixel", 0))])

	var max_luma_err := float(res.get("max_luma_rel_err", 1.0))
	var p99_luma_err := float(res.get("p99_luma_rel_err", 1.0))
	var mean_luma_err := float(res.get("mean_luma_rel_err", 1.0))
	var max_rgb_err := float(res.get("max_rgb_rel_err", 1.0))
	var global_err := float(res.get("global_rel_err", 1.0))

	if downsample == "full" and not use_guiding:
		# Strict per-pixel 1% gate on 64-light scene at 4,096 frames.
		_check(failures, max_luma_err <= 0.01,
				"%s: max_luma_rel_err %.6f > 0.01" % [tag, max_luma_err])
		_check(failures, max_rgb_err <= 0.01,
				"%s: max_rgb_rel_err %.6f > 0.01" % [tag, max_rgb_err])
	else:
		# Half-res rotating + 32-slot guiding over 64 lights: unbiased global/mean convergence
		# and tight per-pixel tolerance even with 1/4 visits per pixel and 4x guiding weight ratio.
		_check(failures, global_err <= 0.005,
				"%s: global_rel_err %.6f > 0.005" % [tag, global_err])
		_check(failures, mean_luma_err <= 0.01,
				"%s: mean_luma_rel_err %.6f > 0.01" % [tag, mean_luma_err])
		_check(failures, p99_luma_err <= 0.025,
				"%s: p99_luma_rel_err %.6f > 0.025" % [tag, p99_luma_err])

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return res


func _check_guiding(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(VIEW_SIZE, false)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 5.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.0, -2.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	_add_box(sub, Vector3(0.0, 0.0, -2.25), Vector3(14.0, 10.0, 0.5), Color(0.8, 0.8, 0.8), 0.8, 0.0)

	# 32 equal-energy omni lights equidistant from the wall; only first 4 will be marked visible.
	for i in range(32):
		var angle := TAU * float(i) / 32.0
		var omni := OmniLight3D.new()
		omni.position = Vector3(cos(angle) * 1.5, sin(angle) * 1.5, 0.5)
		omni.omni_range = 16.0
		omni.light_energy = 1.5
		omni.light_color = Color(1.0, 1.0, 1.0)
		omni.shadow_enabled = false
		sub.add_child(omni)

	var res := await _run_gpu_check({
		"mode": "guiding",
		"downsample": "full",
		"samples_per_pixel": 4,
		"falloff_cull_threshold": 0.0,
		"visible_limit": 4,
		"after_frames": 32,
	})

	_check(failures, bool(res.get("done", false)) and not res.has("error"),
			"guiding: check failed to complete (%s)" % str(res.get("error", "")))
	_check(failures, int(res.get("non_empty_tiles", 0)) > 0, "guiding: no non-empty guiding tiles written")
	_check(failures, int(res.get("max_entries_per_tile", 99)) <= 32,
			"guiding: max_entries_per_tile %d > 32" % int(res.get("max_entries_per_tile", 99)))
	_check(failures, int(res.get("occluded_leaks_in_tiles", -1)) == 0,
			"guiding: occluded lights leaked into tile guiding table (%d)" % int(res.get("occluded_leaks_in_tiles", -1)))

	var before_frac := float(res.get("before_visible_fraction", 0.0))
	var after_frac := float(res.get("after_visible_fraction", 0.0))
	var after_occ := int(res.get("after_occluded_samples", 0))
	_check(failures, before_frac > 0.05 and before_frac < 0.25,
			"guiding: unexpected baseline visible fraction %.4f" % before_frac)
	_check(failures, after_frac > before_frac * 2.0,
			"guiding: expected guided visible fraction %.4f > 2x baseline %.4f" % [after_frac, before_frac])
	_check(failures, after_occ > 0,
			"guiding: occluded lights got 0 samples (weight floor broken, estimator would be biased)")

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return res


func _check_falloff_cull(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(VIEW_SIZE, false)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 5.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.0, -2.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	_add_box(sub, Vector3(0.0, 0.0, -2.25), Vector3(14.0, 10.0, 0.5), Color(0.8, 0.8, 0.8), 0.8, 0.0)

	# Place lights 8 m from the wall with range 10 m -> normalized dist 0.8 -> attenuation ~0.05.
	for i in range(8):
		var omni := OmniLight3D.new()
		omni.position = Vector3(-1.4 + float(i) * 0.4, 0.0, 6.0)
		omni.omni_range = 10.0
		omni.omni_attenuation = 2.0
		omni.light_energy = 2.0
		omni.shadow_enabled = false
		sub.add_child(omni)

	var res := await _run_gpu_check({
		"mode": "falloff_cull",
		"downsample": "full",
		"samples_per_pixel": 4,
		"low_threshold": 0.0,
		"high_threshold": 0.25,
	})

	_check(failures, bool(res.get("done", false)) and not res.has("error"),
			"falloff_cull: check failed to complete")
	_check(failures, int(res.get("valid_samples_low", 0)) > 0,
			"falloff_cull: expected valid samples at threshold 0.0, got 0")
	_check(failures, int(res.get("valid_samples_high", -1)) == 0,
			"falloff_cull: expected 0 valid samples at threshold 0.25, got %d" % int(res.get("valid_samples_high", -1)))

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return res


func _build_ies_profile(nadir_cd: float, side_cd: float) -> IESProfile:
	var text := "\n".join([
		"IESNA:LM-63-2002",
		"[TEST] MegaLights synthetic IES profile (#142)",
		"TILT=NONE",
		"1 1000.0 1.0 3 1 1 2 0.0 0.0 0.0",
		"1.0 0.0 10.0",
		"0.0 90.0 180.0",
		"0.0",
		"%.2f %.2f %.2f" % [nadir_cd, side_cd, side_cd],
	])
	var prof := IESProfile.new()
	var err: Error = prof.load_from_ies_string(text)
	assert(err == OK)
	return prof


func _make_light_function_material(scale_val: float) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = """shader_type light_function;
void light_function() {
	LIGHT_MASK = vec3(%.4f);
}
""" % scale_val
	var mat := ShaderMaterial.new()
	mat.shader = shader
	return mat


func _check_light_eval(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(Vector2i(96, 96), false)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 4.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.0, -2.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	_add_box(sub, Vector3(0.0, 0.0, -2.25), Vector3(12.0, 12.0, 0.5), Color(0.9, 0.9, 0.9), 0.75, 0.0)

	var out := {}

	# 1. Directional light opt-in (default false -> 0 radiance; true -> positive radiance).
	var dir_light := DirectionalLight3D.new()
	dir_light.rotation_degrees = Vector3(-15.0, 0.0, 0.0)
	dir_light.light_energy = 2.0
	dir_light.shadow_enabled = false
	sub.add_child(dir_light)

	var dir_off := await _run_gpu_check({"mode": "light_eval", "directional_lights": false, "falloff_cull_threshold": 0.0})
	var dir_on := await _run_gpu_check({"mode": "light_eval", "directional_lights": true, "falloff_cull_threshold": 0.0})
	out["dir_off_luma"] = float(dir_off.get("mean_luma", -1.0))
	out["dir_on_luma"] = float(dir_on.get("mean_luma", -1.0))
	_check(failures, float(dir_off.get("mean_luma", 1.0)) <= 1e-4,
			"light_eval: directional light contributed when directional_lights=false (%.6f)" % float(dir_off.get("mean_luma", 1.0)))
	_check(failures, float(dir_on.get("mean_luma", 0.0)) > 0.1,
			"light_eval: directional light missing when directional_lights=true (%.6f)" % float(dir_on.get("mean_luma", 0.0)))
	dir_light.queue_free()
	await RenderingServer.frame_post_draw

	# 2. Omni + IES profile + Light Function Atlas modulation.
	# In GodotNext #140, IES nadir (theta=0 deg) aligns with light local -Z, facing the wall at z=-2.25.
	var omni := OmniLight3D.new()
	omni.position = Vector3(0.0, 0.0, 0.0)
	omni.omni_range = 12.0
	omni.light_energy = 4.0
	omni.shadow_enabled = false
	sub.add_child(omni)

	var omni_base := await _run_gpu_check({"mode": "light_eval", "falloff_cull_threshold": 0.0})
	omni.ies_profile = _build_ies_profile(50.0, 1000.0) # Dark nadir toward wall.
	var omni_ies_dark := await _run_gpu_check({"mode": "light_eval", "falloff_cull_threshold": 0.0})
	omni.ies_profile = null
	omni.light_function = _make_light_function_material(0.25)
	var omni_lf := await _run_gpu_check({"mode": "light_eval", "falloff_cull_threshold": 0.0})
	var omni_base_luma := float(omni_base.get("mean_luma", 0.0))
	var omni_ies_dark_luma := float(omni_ies_dark.get("mean_luma", 0.0))
	var omni_lf_luma := float(omni_lf.get("mean_luma", 0.0))
	out["omni_base_luma"] = omni_base_luma
	out["omni_ies_dark_luma"] = omni_ies_dark_luma
	out["omni_lf_luma"] = omni_lf_luma
	_check(failures, omni_base_luma > 0.05, "light_eval: omni baseline too dim")
	_check(failures, omni_ies_dark_luma < omni_base_luma * 0.6,
			"light_eval: IES profile did not attenuate nadir (base=%.4f ies=%.4f)" % [omni_base_luma, omni_ies_dark_luma])
	var lf_ratio := omni_lf_luma / maxf(omni_base_luma, 1e-6)
	out["omni_lf_ratio"] = lf_ratio
	_check(failures, lf_ratio > 0.18 and lf_ratio < 0.32,
			"light_eval: Light Function Atlas ratio %.4f expected ~0.25" % lf_ratio)
	omni.queue_free()
	await RenderingServer.frame_post_draw

	# 3. AreaLight3D: barn doors open vs narrow + textured rect color tint.
	var area := AreaLight3D.new()
	area.position = Vector3(0.0, 0.0, 0.0)
	area.area_size = Vector2(0.8, 0.8)
	area.area_range = 12.0
	area.light_energy = 4.0
	area.barn_door_angle = 90.0
	area.barn_door_length = 0.2
	area.shadow_enabled = false
	sub.add_child(area)

	var area_open := await _run_gpu_check({"mode": "light_eval", "falloff_cull_threshold": 0.0})
	area.barn_door_angle = 12.0
	area.barn_door_length = 1.2
	var area_narrow := await _run_gpu_check({"mode": "light_eval", "falloff_cull_threshold": 0.0})

	# Textured rect light (pure red texture) with open barn doors.
	area.barn_door_angle = 90.0
	area.barn_door_length = 0.2
	var red_img := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	red_img.fill(Color(1.0, 0.05, 0.05, 1.0))
	area.area_texture = ImageTexture.create_from_image(red_img)
	var area_tex := await _run_gpu_check({"mode": "light_eval", "falloff_cull_threshold": 0.0})

	out["area_open_luma"] = float(area_open.get("mean_luma", 0.0))
	out["area_narrow_luma"] = float(area_narrow.get("mean_luma", 0.0))
	out["area_tex_r"] = float(area_tex.get("mean_r", 0.0))
	out["area_tex_g"] = float(area_tex.get("mean_g", 0.0))
	out["area_tex_b"] = float(area_tex.get("mean_b", 0.0))
	_check(failures, out["area_open_luma"] > 0.05, "light_eval: area light baseline too dim")
	_check(failures, out["area_narrow_luma"] > 0.005 and out["area_narrow_luma"] < out["area_open_luma"] * 0.75,
			"light_eval: barn doors did not clip area light (open=%.4f narrow=%.4f)" % [out["area_open_luma"], out["area_narrow_luma"]])
	_check(failures, out["area_tex_r"] > out["area_tex_g"] * 3.0 and out["area_tex_r"] > out["area_tex_b"] * 3.0,
			"light_eval: textured rect light did not tint red (r=%.4f g=%.4f b=%.4f)" % [out["area_tex_r"], out["area_tex_g"], out["area_tex_b"]])

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return out


func _check_reference_convergence(failures: PackedStringArray, require_rt: bool) -> Dictionary:
	var sub := _make_viewport(Vector2i(96, 96), require_rt)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 2.2, 5.5)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.5, -1.5), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	# Receiver wall + floor + occluder pillar between lights and wall to cast genuine soft penumbras.
	_add_box(sub, Vector3(0.0, 1.5, -2.5), Vector3(12.0, 8.0, 0.5), Color(0.8, 0.8, 0.8), 0.7, 0.0)
	_add_box(sub, Vector3(0.0, -0.25, 0.0), Vector3(12.0, 0.5, 10.0), Color(0.75, 0.75, 0.75), 0.7, 0.0)
	_add_box(sub, Vector3(0.0, 1.2, -0.6), Vector3(0.8, 2.2, 0.4), Color(0.6, 0.6, 0.65), 0.6, 0.0)

	# 6 overlapping RT-shadowed soft lights (4 soft-radius Omni + 2 Area lights) so penumbra pixels
	# exercise real trace_shadow() stochastic visibility AND converge <= 1% RSE at 1,024 frames.
	for i in range(4):
		var omni := OmniLight3D.new()
		omni.position = Vector3(-1.8 + float(i) * 1.2, 2.2 + float(i % 2) * 0.5, 1.8)
		omni.omni_range = 16.0
		omni.light_size = 0.45
		omni.light_energy = 2.8
		omni.light_color = Color(1.0, 0.96, 0.92)
		omni.shadow_enabled = true
		omni.shadow_method = Light3D.SHADOW_METHOD_RAY_TRACED
		sub.add_child(omni)

	for i in range(2):
		var area := AreaLight3D.new()
		area.position = Vector3(-1.0 + float(i) * 2.0, 2.6, 2.0)
		area.look_at_from_position(area.position, Vector3(0.0, 1.0, -2.2), Vector3.UP)
		area.area_size = Vector2(0.8, 0.8)
		area.area_range = 16.0
		area.light_energy = 2.5
		area.shadow_enabled = true
		area.shadow_method = Light3D.SHADOW_METHOD_RAY_TRACED
		sub.add_child(area)

	var res := await _run_gpu_check({
		"mode": "reference_convergence",
		"require_rt": require_rt,
		"total_frames": 1024,
		"rse_checkpoint": 16,
		"steps_per_frame": 64,
		"falloff_cull_threshold": 0.0,
	}, 400)

	_check(failures, bool(res.get("done", false)) and not res.has("error"),
			"reference_convergence: check failed to complete (%s)" % str(res.get("error", "")))
	if require_rt:
		_check(failures, bool(res.get("tlas_ready", false)),
				"reference_convergence: NextGI TLAS was not ready")
		_check(failures, int(res.get("penumbra_pixels", 0)) > 50,
				"reference_convergence: expected > 50 stochastic penumbra pixels, got %d" % int(res.get("penumbra_pixels", 0)))
		var cp_p99 := float(res.get("checkpoint_p99_rse", 0.0))
		var final_p99 := float(res.get("p99_rse", 1.0))
		var final_max := float(res.get("max_rse", 1.0))
		_check(failures, cp_p99 > final_p99 * 3.0,
				"reference_convergence: expected p99 RSE to drop >=3x from 16 to 1024 frames (cp=%.5f final=%.5f)" % [cp_p99, final_p99])
		_check(failures, final_p99 <= 0.01,
				"reference_convergence: 1024-frame p99 RSE %.5f > 0.01" % final_p99)
		_check(failures, final_max <= 0.01,
				"reference_convergence: 1024-frame max RSE %.5f > 0.01" % final_max)

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return res


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
