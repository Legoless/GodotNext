extends Node3D
## #135: MegaLights denoiser GPU verification (plan §21.3 contract M5, GM-1, GM-7, budget M-5).
## Verifies on GPU with Vulkan validation (VVL + SyncVal) enabled:
##  1. legacy_off: when MegaLights is disabled, consecutive frames are byte-identical and
##     allocate 0 MegaLights bytes, and megalights_get_tier_info() reports denoiser="builtin".
##  2. full_res_denoise: full-res shade + temporal + 2-pass variance-guided spatial denoise
##     produces finite, non-zero demodulated diffuse/specular matching #142/#146 reference
##     luminance while measurably reducing spatial noise over 16 frames with clamped_energy_ratio <= 0.02.
##  3. half_res_denoise: half-res 2x2 joint-bilateral fill (MODE_FILL_HALF) + temporal + spatial
##     denoise produces finite, smooth, reference-matching output with clamped_energy_ratio <= 0.02.
##  4. rr_bypass: rendering/megalights/denoiser = 1 skips temporal & spatial passes, allocates
##     zero temporal history textures, and exposes exact noisy demodulated targets.
##  5. reset_regions: denoise_reset_regions resets history_len to 1 inside the dirty tile rect
##     while preserving accumulated history (> 1) outside.
##  6. viewport_debug_draws (requires GODOTNEXT_MEGALIGHTS_INPUTS=1): Viewport.DEBUG_DRAW_ML_NOISY,
##     DEBUG_DRAW_ML_DENOISED, and DEBUG_DRAW_ML_VARIANCE render distinct, non-black, finite frames.
##  7. upscaler_handoff (requires GODOTNEXT_MEGALIGHTS_INPUTS=1): MegaLights denoiser + #37 prepass
##     velocity + camera movement / cut hand off cleanly to TSR and FSR2 with 0 validation errors.

const VIEW_SIZE := Vector2i(128, 128)


func _ready() -> void:
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
	var mode := _arg("ml-denoise-check", "core")
	var failures := PackedStringArray()
	var report := {
		"test": "megalights_denoiser",
		"mode": mode,
		"pass": false,
	}

	if mode == "core" or mode == "all":
		report["legacy_off"] = await _check_legacy_off(failures)
		report["full_res_denoise"] = await _check_denoise_pipeline(failures, "full", false, false)
		report["half_res_denoise"] = await _check_denoise_pipeline(failures, "half", false, false)
		report["rr_bypass"] = await _check_rr_bypass(failures)
		report["reset_regions"] = await _check_denoise_pipeline(failures, "full", false, true)
	if mode == "viewport_views" or mode == "all":
		report["viewport_debug_draws"] = await _check_viewport_debug_draws(failures)
		report["upscaler_handoff"] = await _check_upscaler_handoff(failures)

	report["failures"] = failures
	report["pass"] = failures.is_empty()
	print("MEGALIGHTS_DENOISE_RESULT=" + JSON.stringify(report))
	get_tree().quit(0 if failures.is_empty() else 1)


func _make_viewport(size: Vector2i, enable_nextgi: bool) -> SubViewport:
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


func _add_box(parent: Node, pos: Vector3, size: Vector3, albedo: Color, roughness: float = 0.55, metallic: float = 0.0) -> MeshInstance3D:
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


func _build_multi_light_scene(sub: SubViewport) -> Camera3D:
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 2.0, 5.5)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.0, -2.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	# Matte receiver wall + glossy floor + metallic block + contact/mid-room shadow casters.
	_add_box(sub, Vector3(0.0, 2.0, -2.5), Vector3(14.0, 8.0, 0.5), Color(0.75, 0.72, 0.68), 0.75, 0.0)
	_add_box(sub, Vector3(0.0, -0.25, 0.0), Vector3(14.0, 0.5, 10.0), Color(0.65, 0.65, 0.70), 0.35, 0.0)
	_add_box(sub, Vector3(-1.4, 0.6, -1.8), Vector3(0.7, 1.2, 0.6), Color(0.85, 0.70, 0.35), 0.25, 0.9)
	_add_box(sub, Vector3(1.5, 0.5, -1.8), Vector3(0.6, 1.0, 0.6), Color(0.35, 0.55, 0.80), 0.45, 0.0)
	_add_box(sub, Vector3(0.0, 1.2, -0.2), Vector3(0.65, 2.4, 0.45), Color(0.60, 0.60, 0.60), 0.60, 0.0)

	# 16 local lights (8 Omni + 4 Spot + 4 Area) so 4 spp has strong per-frame Monte Carlo variance
	# that temporal accumulation + 2-pass à-trous spatial denoising must cleanly suppress.
	var palette := [
		Color(1.0, 0.85, 0.70),
		Color(0.75, 0.90, 1.0),
		Color(0.95, 0.80, 0.95),
		Color(0.85, 1.0, 0.80),
	]
	for i in range(8):
		var omni := OmniLight3D.new()
		omni.position = Vector3(-3.5 + float(i) * 1.0, 2.4 + float(i % 2) * 0.9, 1.8 + float(i % 3) * 0.4)
		omni.light_color = palette[i % palette.size()]
		omni.omni_range = 16.0
		omni.light_energy = 1.6
		omni.shadow_enabled = true
		sub.add_child(omni)

	for i in range(4):
		var spot := SpotLight3D.new()
		spot.position = Vector3(-3.0 + float(i) * 2.0, 3.4, 2.6)
		spot.look_at_from_position(spot.position, Vector3(-1.5 + float(i) * 1.0, 0.8, -2.2), Vector3.UP)
		spot.light_color = palette[(i + 1) % palette.size()]
		spot.spot_range = 18.0
		spot.spot_angle = 55.0
		spot.light_energy = 2.2
		spot.shadow_enabled = true
		sub.add_child(spot)

	for i in range(4):
		var area := AreaLight3D.new()
		area.position = Vector3(-2.25 + float(i) * 1.5, 3.1, 1.6)
		area.look_at_from_position(area.position, Vector3(0.0, 0.8, -2.2), Vector3.UP)
		area.light_color = palette[(i + 2) % palette.size()]
		area.area_size = Vector2(0.45, 0.45)
		area.area_range = 18.0
		area.light_energy = 1.8
		area.shadow_enabled = true
		sub.add_child(area)

	return cam


func _run_gpu_check(options: Dictionary, max_wait_frames: int = 180) -> Dictionary:
	for _w in range(3):
		await RenderingServer.frame_post_draw
	RenderingServer.megalights_request_check(options)
	var res := {}
	for _i in range(max_wait_frames):
		await RenderingServer.frame_post_draw
		res = RenderingServer.megalights_get_check_result()
		if bool(res.get("done", false)):
			return res
	res["error"] = "Timed out waiting for MegaLights denoiser GPU check."
	return res


func _check_legacy_off(failures: PackedStringArray) -> Dictionary:
	var tier_info := RenderingServer.megalights_get_tier_info()
	_check(failures, str(tier_info.get("denoiser", "")) == "builtin",
			"legacy_off: expected default denoiser='builtin', got %s" % str(tier_info.get("denoiser", "")))
	_check(failures, not bool(tier_info.get("rr_bypass", true)),
			"legacy_off: expected default rr_bypass=false")

	var sub := _make_viewport(VIEW_SIZE, false)
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
			"legacy_off: MegaLights buffers allocated while inactive (bytes=%d)" % probe.memory_bytes)

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return {
		"tier_info": tier_info,
		"bytes_equal": bytes_equal,
		"probe_calls": probe.calls,
		"memory_bytes": probe.memory_bytes,
	}


func _check_denoise_pipeline(failures: PackedStringArray, downsample: String, rr_bypass: bool, test_reset_region: bool) -> Dictionary:
	var tag := ("reset_regions" if test_reset_region else (downsample + "_res_denoise"))
	var sub := _make_viewport(VIEW_SIZE, true)
	_build_multi_light_scene(sub)

	var res := await _run_gpu_check({
		"mode": "denoise",
		"downsample": downsample,
		"samples_per_pixel": 4,
		"frames": 16,
		"rr_bypass": rr_bypass,
		"screen_traces": true,
		"dedupe": true,
		"compare_reference": not test_reset_region,
		"ref_frames": 16,
		"test_reset_region": test_reset_region,
		"debug_vis_mode": 1 if (downsample == "full" and not test_reset_region) else -1,
		"wait_for_tlas": true,
	})

	_check(failures, bool(res.get("done", false)) and not res.has("error"),
			"%s: check failed (%s)" % [tag, str(res.get("error", ""))])
	_check(failures, bool(res.get("tlas_ready", false)), "%s: TLAS not ready" % tag)
	_check(failures, int(res.get("valid_pixels", 0)) > 8000,
			"%s: expected > 8000 valid pixels, got %d" % [tag, int(res.get("valid_pixels", 0))])
	_check(failures, int(res.get("non_finite_noisy_diff", 1)) == 0 and int(res.get("non_finite_noisy_spec", 1)) == 0,
			"%s: non-finite values in noisy shade targets" % tag)
	_check(failures, int(res.get("non_finite_denoised_diff", 1)) == 0 and int(res.get("non_finite_denoised_spec", 1)) == 0,
			"%s: non-finite values in denoised targets" % tag)
	_check(failures, float(res.get("noisy_diff_mean", 0.0)) > 0.05 and float(res.get("denoised_diff_mean", 0.0)) > 0.05,
			"%s: diffuse radiance too dark (noisy=%.4f den=%.4f)" % [tag, float(res.get("noisy_diff_mean", 0.0)), float(res.get("denoised_diff_mean", 0.0))])
	_check(failures, float(res.get("noisy_spec_mean", 0.0)) > 0.001 and float(res.get("denoised_spec_mean", 0.0)) > 0.001,
			"%s: specular radiance too dark (noisy=%.4f den=%.4f)" % [tag, float(res.get("noisy_spec_mean", 0.0)), float(res.get("denoised_spec_mean", 0.0))])

	var noisy_std := float(res.get("noisy_spatial_std", 0.0))
	var den_std := float(res.get("denoised_spatial_std", 0.0))
	_check(failures, int(res.get("smooth_interior_pixels", 0)) > 2000,
			"%s: expected > 2000 smooth interior pixels, got %d" % [tag, int(res.get("smooth_interior_pixels", 0))])
	_check(failures, noisy_std > 0.01 and den_std < noisy_std * 0.70,
			"%s: insufficient noise reduction (noisy_std=%.5f den_std=%.5f)" % [tag, noisy_std, den_std])

	var clamp_ratio := float(res.get("clamped_energy_ratio", 1.0))
	_check(failures, int(res.get("total_direct_energy_fp", 0)) > 0,
			"%s: total_direct_energy_fp == 0" % tag)
	_check(failures, clamp_ratio >= 0.0 and clamp_ratio <= 0.02,
			"%s: clamped_energy_ratio %.5f exceeds 0.02 PQ limit" % [tag, clamp_ratio])

	if test_reset_region:
		_check(failures, int(res.get("reset_inside_pixels", 0)) > 0 and int(res.get("reset_inside_max_len", 0)) == 1,
				"%s: dirty reset region did not reset history_len to 1 (inside_max=%d)" % [tag, int(res.get("reset_inside_max_len", 0))])
		_check(failures, int(res.get("reset_outside_pixels", 0)) > 0 and int(res.get("reset_outside_max_len", 0)) >= 10,
				"%s: outside region did not preserve accumulated history (outside_max=%d)" % [tag, int(res.get("reset_outside_max_len", 0))])
	else:
		_check(failures, int(res.get("max_history_len_diff", 0)) >= 12,
				"%s: expected temporal history_len >= 12, got %d" % [tag, int(res.get("max_history_len_diff", 0))])
		var ref_luma := float(res.get("ref_mean_luma", 0.0))
		var ref_err := float(res.get("ref_rel_err", 1.0))
		var max_err := 0.20 if downsample == "half" else 0.15
		_check(failures, ref_luma > 0.05, "%s: reference luminance too low (%.4f)" % [tag, ref_luma])
		_check(failures, ref_err <= max_err,
				"%s: shaded physical luminance diverged from #142/#146 reference (ref=%.4f shaded=%.4f rel_err=%.4f)" % [tag, ref_luma, float(res.get("physical_shaded_mean_luma", 0.0)), ref_err])
		if downsample == "full":
			_check(failures, int(res.get("debug_non_black_pixels", 0)) > 4000 and int(res.get("debug_non_finite_count", 1)) == 0,
					"%s: debug vis output invalid (non_black=%d non_finite=%d)" % [tag, int(res.get("debug_non_black_pixels", 0)), int(res.get("debug_non_finite_count", 1))])

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return res


func _check_rr_bypass(failures: PackedStringArray) -> Dictionary:
	var prev_denoiser: int = int(ProjectSettings.get_setting("rendering/megalights/denoiser"))
	ProjectSettings.set_setting("rendering/megalights/denoiser", 1)
	var tier_rr := RenderingServer.megalights_get_tier_info()
	_check(failures, str(tier_rr.get("denoiser", "")) == "rr_bypass" and bool(tier_rr.get("rr_bypass", false)),
			"rr_bypass: tier_info did not report rr_bypass=true")

	var sub := _make_viewport(VIEW_SIZE, true)
	_build_multi_light_scene(sub)

	var res := await _run_gpu_check({
		"mode": "denoise",
		"downsample": "full",
		"samples_per_pixel": 4,
		"frames": 4,
		"rr_bypass": true,
		"screen_traces": true,
		"dedupe": true,
		"compare_reference": false,
		"debug_vis_mode": 0,
		"wait_for_tlas": true,
	})

	_check(failures, bool(res.get("done", false)) and not res.has("error"),
			"rr_bypass: check failed (%s)" % str(res.get("error", "")))
	_check(failures, bool(res.get("rr_bypass_identical", false)),
			"rr_bypass: denoised output was not identical to noisy target or allocated temporal buffers")
	_check(failures, int(res.get("non_finite_noisy_diff", 1)) == 0 and int(res.get("non_finite_noisy_spec", 1)) == 0,
			"rr_bypass: non-finite values in noisy demodulated outputs")
	_check(failures, float(res.get("noisy_diff_mean", 0.0)) > 0.05,
			"rr_bypass: noisy diffuse mean too low (%.4f)" % float(res.get("noisy_diff_mean", 0.0)))
	_check(failures, int(res.get("debug_non_black_pixels", 0)) > 4000 and int(res.get("debug_non_finite_count", 1)) == 0,
			"rr_bypass: debug vis output invalid under rr_bypass")

	sub.queue_free()
	await RenderingServer.frame_post_draw
	ProjectSettings.set_setting("rendering/megalights/denoiser", prev_denoiser)
	res["tier_info"] = tier_rr
	return res


func _image_stats(img: Image) -> Dictionary:
	var non_black := 0
	var sum_luma := 0.0
	var sum_chroma := 0.0
	var w := img.get_width()
	var h := img.get_height()
	for y in range(h):
		for x in range(w):
			var c := img.get_pixel(x, y)
			var l := c.r * 0.2126 + c.g * 0.7152 + c.b * 0.0722
			if l > 0.01:
				non_black += 1
			sum_luma += l
			sum_chroma += absf(c.r - c.g) + absf(c.g - c.b) + absf(c.b - c.r)
	var n := maxf(float(w * h), 1.0)
	return {
		"non_black": non_black,
		"mean_luma": sum_luma / n,
		"mean_chroma": sum_chroma / n,
		"bytes_hash": hash(img.get_data()),
	}


func _check_viewport_debug_draws(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(VIEW_SIZE, true)
	_build_multi_light_scene(sub)

	var modes := {
		"ml_noisy": Viewport.DEBUG_DRAW_ML_NOISY,
		"ml_denoised": Viewport.DEBUG_DRAW_ML_DENOISED,
		"ml_variance": Viewport.DEBUG_DRAW_ML_VARIANCE,
	}
	var results := {}
	for key in modes:
		sub.debug_draw = modes[key]
		for _i in range(10):
			await RenderingServer.frame_post_draw
		var img := sub.get_texture().get_image()
		_check(failures, img != null, "viewport_debug_draws: null image for %s" % key)
		if img != null:
			var st := _image_stats(img)
			results[key] = st
			_check(failures, int(st["non_black"]) > 4000,
					"viewport_debug_draws: %s produced too few non-black pixels (%d)" % [key, int(st["non_black"])])
			_check(failures, float(st["mean_luma"]) > 0.02,
					"viewport_debug_draws: %s mean_luma too low (%.4f)" % [key, float(st["mean_luma"])])

	if results.has("ml_noisy") and results.has("ml_denoised") and results.has("ml_variance"):
		_check(failures, int(results["ml_noisy"]["bytes_hash"]) != int(results["ml_variance"]["bytes_hash"]),
				"viewport_debug_draws: ML_NOISY and ML_VARIANCE images were identical")
		_check(failures, int(results["ml_denoised"]["bytes_hash"]) != int(results["ml_variance"]["bytes_hash"]),
				"viewport_debug_draws: ML_DENOISED and ML_VARIANCE images were identical")
		_check(failures, float(results["ml_variance"]["mean_chroma"]) > 0.05,
				"viewport_debug_draws: ML_VARIANCE heatmap had unexpectedly low chroma (%.4f)" % float(results["ml_variance"]["mean_chroma"]))

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return results


func _check_upscaler_handoff(failures: PackedStringArray) -> Dictionary:
	# Exercises MegaLights denoiser + #37 prepass velocity (dense for TSR, sentinel for FSR2)
	# + camera motion and abrupt camera-cut reset before temporal upscaling.
	var results := {}

	for mode_name in ["tsr", "fsr2"]:
		var sub := _make_viewport(VIEW_SIZE, true)
		sub.use_taa = false
		sub.scaling_3d_scale = 0.75
		if mode_name == "tsr":
			sub.scaling_3d_mode = Viewport.SCALING_3D_MODE_TSR
		else:
			sub.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
		sub.debug_draw = Viewport.DEBUG_DRAW_ML_DENOISED
		var cam := _build_multi_light_scene(sub)

		# Smooth motion frames (exercises #37 velocity reprojection + temporal accumulation).
		for i in range(6):
			cam.position = Vector3(float(i) * 0.08, 2.0, 5.5)
			cam.look_at_from_position(cam.position, Vector3(0.0, 1.0, -2.0), Vector3.UP)
			await RenderingServer.frame_post_draw

		# Abrupt camera cut (> 10m teleport -> tests camera-cut history reset cleanly).
		cam.position = Vector3(0.0, 2.0, 18.0)
		cam.look_at_from_position(cam.position, Vector3(0.0, 1.0, -2.0), Vector3.UP)
		await RenderingServer.frame_post_draw
		cam.position = Vector3(0.0, 2.0, 5.5)
		cam.look_at_from_position(cam.position, Vector3(0.0, 1.0, -2.0), Vector3.UP)
		for _j in range(4):
			await RenderingServer.frame_post_draw

		var img := sub.get_texture().get_image()
		_check(failures, img != null, "upscaler_handoff(%s): null viewport image" % mode_name)
		if img != null:
			var st := _image_stats(img)
			results[mode_name] = st
			_check(failures, int(st["non_black"]) > 3000,
					"upscaler_handoff(%s): too few non-black pixels (%d)" % [mode_name, int(st["non_black"])])

		sub.queue_free()
		await RenderingServer.frame_post_draw

	return results


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
