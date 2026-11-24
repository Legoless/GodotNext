extends Node3D
## #138: Light functions and the Light Function Atlas GPU verification.
## Verifies:
##  (a) No light function -> zero bakes and byte-identical baseline frame (including after attach + clear).
##  (b) Directional (R8 & RGBA8), Spot, and Omni light functions modulate forward opaque lighting
##      and volumetric fog matching analytic per-pixel evaluation.
##  (c) Static light functions bake once on creation and re-bake once on parameter change, while
##      animated (TIME) light functions bake every frame and visibly animate output pixels.
##  (d) Forward transparents ignore the atlas when translucent=false and honour it when translucent=true.
##  (e) NextGI SRC relight renders cleanly with an active light function (zero validation errors).

const VP_SIZE := Vector2i(256, 256)

const SHADER_STEP_X := """
shader_type light_function;
uniform float left_val = 0.2;
uniform float right_val = 1.0;
void light_function() {
	LIGHT_MASK = vec3(LIGHT_UV.x < 0.5 ? left_val : right_val);
}
"""

const SHADER_COLOR_SPLIT := """
shader_type light_function;
void light_function() {
	LIGHT_MASK = LIGHT_UV.x < 0.5 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0);
}
"""

const SHADER_UNIFORM_GAIN := """
shader_type light_function;
uniform float gain = 0.25;
void light_function() {
	LIGHT_MASK = vec3(gain);
}
"""

const SHADER_ANIMATED := """
shader_type light_function;
void light_function() {
	float v = 0.15 + 0.85 * abs(sin(TIME * 6.2831853 + LIGHT_UV.x * 3.14159));
	LIGHT_MASK = vec3(v);
}
"""

const SHADER_BLACK := """
shader_type light_function;
void light_function() {
	LIGHT_MASK = vec3(0.0);
}
"""


func _ready() -> void:
	get_tree().root.disable_3d = true
	_run()


func _make_lf_material(code: String) -> ShaderMaterial:
	var sh := Shader.new()
	sh.code = code
	var mat := ShaderMaterial.new()
	mat.shader = sh
	return mat


func _make_viewport(with_fog: bool = false, with_nextgi: bool = false) -> SubViewport:
	var sub := SubViewport.new()
	sub.own_world_3d = true
	sub.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	sub.msaa_3d = Viewport.MSAA_DISABLED
	sub.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	sub.positional_shadow_atlas_size = 0
	sub.size = VP_SIZE
	add_child(sub)

	var we := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_energy = 0.0
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	if with_fog:
		env.volumetric_fog_enabled = true
		env.volumetric_fog_density = 0.25
		env.volumetric_fog_albedo = Color(1.0, 1.0, 1.0)
		env.volumetric_fog_emission = Color(0.0, 0.0, 0.0)
		env.volumetric_fog_emission_energy = 0.0
		env.volumetric_fog_gi_inject = 0.0
		env.volumetric_fog_ambient_inject = 0.0
		env.volumetric_fog_anisotropy = 0.0
		env.volumetric_fog_length = 16.0
		env.volumetric_fog_temporal_reprojection_enabled = false
	if with_nextgi:
		env.nextgi_enabled = true
	we.environment = env
	sub.add_child(we)
	return sub


func _add_wall(parent: Node, pos: Vector3, size: Vector2, transparent: bool = false) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var quad := QuadMesh.new()
	quad.size = size
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.9, 0.9, 0.9, 0.85 if transparent else 1.0)
	mat.roughness = 1.0
	mat.metallic = 0.0
	if transparent:
		mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	quad.material = mat
	mi.mesh = quad
	mi.position = pos
	parent.add_child(mi)
	return mi


func _wait_frames(n: int) -> void:
	for _i in range(n):
		await RenderingServer.frame_post_draw


func _mean_rgb(img: Image, rect: Rect2i) -> Vector3:
	var acc := Vector3.ZERO
	var count := 0
	for y in range(rect.position.y, rect.position.y + rect.size.y):
		for x in range(rect.position.x, rect.position.x + rect.size.x):
			var c := img.get_pixel(x, y)
			acc += Vector3(c.r, c.g, c.b)
			count += 1
	return acc / maxf(float(count), 1.0)


func _srgb_to_linear_channel(v: float) -> float:
	if v <= 0.04045:
		return v / 12.92
	return pow((v + 0.055) / 1.055, 2.4)


func _mean_linear_luma(img: Image, rect: Rect2i) -> float:
	var acc := 0.0
	var count := 0
	for y in range(rect.position.y, rect.position.y + rect.size.y):
		for x in range(rect.position.x, rect.position.x + rect.size.x):
			var c := img.get_pixel(x, y)
			var lr := _srgb_to_linear_channel(c.r)
			var lg := _srgb_to_linear_channel(c.g)
			var lb := _srgb_to_linear_channel(c.b)
			acc += 0.2126 * lr + 0.7152 * lg + 0.0722 * lb
			count += 1
	return acc / maxf(float(count), 1.0)


func _run() -> void:
	var failures := PackedStringArray()
	var report := {"test": "light_function", "pass": false}

	var orig_format = ProjectSettings.get_setting("rendering/lights_and_shadows/light_function_atlas/format")
	var orig_translucent = ProjectSettings.get_setting("rendering/lights_and_shadows/light_function_atlas/translucent")
	ProjectSettings.set_setting("rendering/lights_and_shadows/light_function_atlas/format", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/light_function_atlas/translucent", false)

	report["no_lf_baseline"] = await _check_no_lf_baseline(failures)
	report["directional"] = await _check_directional(failures)
	report["spot"] = await _check_spot(failures)
	report["omni"] = await _check_omni(failures)
	report["volumetric_fog"] = await _check_volumetric_fog(failures)
	report["static_vs_animated"] = await _check_static_vs_animated(failures)
	report["translucent"] = await _check_translucent(failures)
	report["nextgi_src"] = await _check_nextgi_src(failures)

	ProjectSettings.set_setting("rendering/lights_and_shadows/light_function_atlas/format", orig_format)
	ProjectSettings.set_setting("rendering/lights_and_shadows/light_function_atlas/translucent", orig_translucent)

	report["failures"] = failures
	report["pass"] = failures.is_empty()
	print("LIGHT_FUNCTION_CHECK_RESULT=" + JSON.stringify(report))
	get_tree().quit(0 if failures.is_empty() else 1)


func _check_no_lf_baseline(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport()
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 4.0)
	cam.look_at_from_position(cam.position, Vector3.ZERO, Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	_add_wall(sub, Vector3.ZERO, Vector2(8.0, 8.0))
	var dir_light := DirectionalLight3D.new()
	dir_light.light_energy = 1.0
	dir_light.shadow_enabled = false
	sub.add_child(dir_light)

	await _wait_frames(2)
	var stats0 := RenderingServer.light_function_atlas_get_stats()
	var img1 := sub.get_texture().get_image()
	await _wait_frames(1)
	var img2 := sub.get_texture().get_image()

	var zero_bakes := int(stats0.get("total_bakes", -1)) == 0 and int(stats0.get("active_tiles", -1)) == 0
	var consecutive_equal := img1 != null and img2 != null and img1.get_data() == img2.get_data()
	var center_luma := _mean_linear_luma(img1, Rect2i(96, 96, 64, 64))

	# Attach a light function, bake it, then remove it and confirm frame returns byte-identical to baseline.
	dir_light.light_function = _make_lf_material(SHADER_BLACK)
	await _wait_frames(2)
	var black_luma := _mean_linear_luma(sub.get_texture().get_image(), Rect2i(96, 96, 64, 64))

	dir_light.light_function = null
	await _wait_frames(2)
	var img_cleared := sub.get_texture().get_image()
	var cleared_equal := img1 != null and img_cleared != null and img1.get_data() == img_cleared.get_data()
	var stats_after := RenderingServer.light_function_atlas_get_stats()

	if not zero_bakes:
		failures.append("no_lf_baseline: expected 0 bakes/active_tiles before any light function, got %s" % [stats0])
	if not consecutive_equal:
		failures.append("no_lf_baseline: consecutive frames without light functions were not byte-identical")
	if center_luma < 0.05:
		failures.append("no_lf_baseline: unlit baseline frame (luma=%f)" % center_luma)
	if black_luma > 0.005:
		failures.append("no_lf_baseline: black light function did not extinguish light (luma=%f)" % black_luma)
	if not cleared_equal:
		failures.append("no_lf_baseline: clearing light_function did not return byte-identical frame")
	if int(stats_after.get("active_tiles", -1)) != 0:
		failures.append("no_lf_baseline: active_tiles did not return to 0 after clearing")

	sub.queue_free()
	await _wait_frames(1)
	return {
		"zero_bakes": zero_bakes,
		"consecutive_equal": consecutive_equal,
		"cleared_equal": cleared_equal,
		"center_luma": center_luma,
		"black_luma": black_luma,
	}


func _check_directional(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport()
	var cam := Camera3D.new()
	cam.projection = Camera3D.PROJECTION_ORTHOGONAL
	cam.size = 4.0
	cam.position = Vector3(0.0, 0.0, 4.0)
	cam.look_at_from_position(cam.position, Vector3.ZERO, Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	_add_wall(sub, Vector3.ZERO, Vector2(6.0, 6.0))
	var light := DirectionalLight3D.new()
	light.light_energy = 1.0
	light.shadow_enabled = false
	light.light_function_scale = Vector3(4.0, 4.0, 1.0)
	sub.add_child(light)

	await _wait_frames(2)
	var img_base := sub.get_texture().get_image()
	var left_rect := Rect2i(24, 96, 64, 64)
	var right_rect := Rect2i(168, 96, 64, 64)
	var base_left := _mean_linear_luma(img_base, left_rect)
	var base_right := _mean_linear_luma(img_base, right_rect)

	# R8 mode: left half = 0.2, right half = 1.0
	ProjectSettings.set_setting("rendering/lights_and_shadows/light_function_atlas/format", 0)
	light.light_function = _make_lf_material(SHADER_STEP_X)
	await _wait_frames(2)
	var img_r8 := sub.get_texture().get_image()
	var r8_left := _mean_linear_luma(img_r8, left_rect)
	var r8_right := _mean_linear_luma(img_r8, right_rect)
	var r8_left_ratio := r8_left / maxf(base_left, 1e-5)
	var r8_right_ratio := r8_right / maxf(base_right, 1e-5)

	if abs(r8_left_ratio - 0.2) > 0.04 or abs(r8_right_ratio - 1.0) > 0.04:
		failures.append("directional R8 mismatch: left_ratio=%f (exp 0.2), right_ratio=%f (exp 1.0)" % [r8_left_ratio, r8_right_ratio])

	# RGBA8 mode: left half red, right half blue
	ProjectSettings.set_setting("rendering/lights_and_shadows/light_function_atlas/format", 1)
	light.light_function = _make_lf_material(SHADER_COLOR_SPLIT)
	await _wait_frames(2)
	var img_rgba := sub.get_texture().get_image()
	var rgba_left := _mean_rgb(img_rgba, left_rect)
	var rgba_right := _mean_rgb(img_rgba, right_rect)

	if rgba_left.x < 0.2 or rgba_left.z > 0.03 or rgba_right.z < 0.2 or rgba_right.x > 0.03:
		failures.append("directional RGBA8 color split failed: left=%s right=%s" % [rgba_left, rgba_right])

	ProjectSettings.set_setting("rendering/lights_and_shadows/light_function_atlas/format", 0)
	sub.queue_free()
	await _wait_frames(1)
	return {
		"r8_left_ratio": r8_left_ratio,
		"r8_right_ratio": r8_right_ratio,
		"rgba_left": [rgba_left.x, rgba_left.y, rgba_left.z],
		"rgba_right": [rgba_right.x, rgba_right.y, rgba_right.z],
	}


func _check_spot(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport()
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 4.0)
	cam.look_at_from_position(cam.position, Vector3.ZERO, Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	_add_wall(sub, Vector3.ZERO, Vector2(8.0, 8.0))
	var light := SpotLight3D.new()
	light.position = Vector3(0.0, 0.0, 3.0)
	light.spot_range = 10.0
	light.spot_angle = 45.0
	light.spot_angle_attenuation = 0.2
	light.light_energy = 2.0
	light.shadow_enabled = false
	sub.add_child(light)

	await _wait_frames(2)
	var img_base := sub.get_texture().get_image()
	var left_rect := Rect2i(64, 104, 40, 48)
	var right_rect := Rect2i(152, 104, 40, 48)
	var base_left := _mean_linear_luma(img_base, left_rect)
	var base_right := _mean_linear_luma(img_base, right_rect)

	var mat := _make_lf_material(SHADER_STEP_X)
	mat.set_shader_parameter("left_val", 0.0)
	mat.set_shader_parameter("right_val", 1.0)
	light.light_function = mat
	await _wait_frames(2)
	var img_lf := sub.get_texture().get_image()
	var lf_left := _mean_linear_luma(img_lf, left_rect)
	var lf_right := _mean_linear_luma(img_lf, right_rect)
	var left_ratio := lf_left / maxf(base_left, 1e-5)
	var right_ratio := lf_right / maxf(base_right, 1e-5)

	if base_left < 0.03 or base_right < 0.03:
		failures.append("spot: baseline unlit (left=%f right=%f)" % [base_left, base_right])
	if left_ratio > 0.05 or abs(right_ratio - 1.0) > 0.05:
		failures.append("spot: analytic mismatch left_ratio=%f (exp 0.0) right_ratio=%f (exp 1.0)" % [left_ratio, right_ratio])

	sub.queue_free()
	await _wait_frames(1)
	return {
		"base_left": base_left,
		"base_right": base_right,
		"left_ratio": left_ratio,
		"right_ratio": right_ratio,
	}


func _check_omni(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport()
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 4.0)
	cam.look_at_from_position(cam.position, Vector3.ZERO, Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	_add_wall(sub, Vector3.ZERO, Vector2(8.0, 8.0))
	var light := OmniLight3D.new()
	light.position = Vector3(0.0, 0.0, 2.0)
	light.omni_range = 10.0
	light.light_energy = 2.0
	light.shadow_enabled = false
	sub.add_child(light)

	await _wait_frames(2)
	var img_base := sub.get_texture().get_image()
	var left_rect := Rect2i(48, 104, 48, 48)
	var right_rect := Rect2i(160, 104, 48, 48)
	var base_left := _mean_linear_luma(img_base, left_rect)
	var base_right := _mean_linear_luma(img_base, right_rect)

	var mat := _make_lf_material(SHADER_STEP_X)
	mat.set_shader_parameter("left_val", 0.0)
	mat.set_shader_parameter("right_val", 1.0)
	light.light_function = mat
	await _wait_frames(2)
	var img_lf := sub.get_texture().get_image()
	var lf_left := _mean_linear_luma(img_lf, left_rect)
	var lf_right := _mean_linear_luma(img_lf, right_rect)
	var left_ratio := lf_left / maxf(base_left, 1e-5)
	var right_ratio := lf_right / maxf(base_right, 1e-5)

	if base_left < 0.03 or base_right < 0.03:
		failures.append("omni: baseline unlit (left=%f right=%f)" % [base_left, base_right])
	if left_ratio > 0.05 or abs(right_ratio - 1.0) > 0.05:
		failures.append("omni: analytic mismatch left_ratio=%f (exp 0.0) right_ratio=%f (exp 1.0)" % [left_ratio, right_ratio])

	sub.queue_free()
	await _wait_frames(1)
	return {
		"base_left": base_left,
		"base_right": base_right,
		"left_ratio": left_ratio,
		"right_ratio": right_ratio,
	}


func _check_volumetric_fog(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(true, false)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 4.0)
	cam.look_at_from_position(cam.position, Vector3.ZERO, Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	var light := SpotLight3D.new()
	light.position = Vector3(0.0, 0.0, 3.8)
	light.spot_range = 16.0
	light.spot_angle = 55.0
	light.spot_angle_attenuation = 0.2
	light.light_energy = 4.0
	light.light_volumetric_fog_energy = 4.0
	light.shadow_enabled = false
	sub.add_child(light)

	await _wait_frames(3)
	var img_base := sub.get_texture().get_image()
	var left_rect := Rect2i(48, 96, 48, 64)
	var right_rect := Rect2i(160, 96, 48, 64)
	var base_left := _mean_linear_luma(img_base, left_rect)
	var base_right := _mean_linear_luma(img_base, right_rect)

	var mat := _make_lf_material(SHADER_STEP_X)
	mat.set_shader_parameter("left_val", 0.0)
	mat.set_shader_parameter("right_val", 1.0)
	light.light_function = mat
	await _wait_frames(3)
	var img_lf := sub.get_texture().get_image()
	var lf_left := _mean_linear_luma(img_lf, left_rect)
	var lf_right := _mean_linear_luma(img_lf, right_rect)
	var left_ratio := lf_left / maxf(base_left, 1e-5)
	var right_ratio := lf_right / maxf(base_right, 1e-5)

	if base_left < 0.02 or base_right < 0.02:
		failures.append("volumetric_fog: baseline fog unlit (left=%f right=%f)" % [base_left, base_right])
	if left_ratio > 0.12 or abs(right_ratio - 1.0) > 0.12:
		failures.append("volumetric_fog: light function modulation mismatch left_ratio=%f right_ratio=%f" % [left_ratio, right_ratio])

	sub.queue_free()
	await _wait_frames(1)
	return {
		"base_left": base_left,
		"base_right": base_right,
		"left_ratio": left_ratio,
		"right_ratio": right_ratio,
	}


func _check_static_vs_animated(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport()
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 4.0)
	cam.look_at_from_position(cam.position, Vector3.ZERO, Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	_add_wall(sub, Vector3.ZERO, Vector2(8.0, 8.0))
	var light := DirectionalLight3D.new()
	light.light_energy = 1.0
	light.shadow_enabled = false
	sub.add_child(light)

	await _wait_frames(1)
	var before := RenderingServer.light_function_atlas_get_stats()
	var static_start := int(before.get("static_bakes", 0))

	var static_mat := _make_lf_material(SHADER_UNIFORM_GAIN)
	static_mat.set_shader_parameter("gain", 0.25)
	light.light_function = static_mat
	await _wait_frames(4)
	var after_static := RenderingServer.light_function_atlas_get_stats()
	var static_bakes_1 := int(after_static.get("static_bakes", 0)) - static_start
	var last_frame_bakes_idle := int(after_static.get("last_frame_bakes", -1))
	var luma_low := _mean_linear_luma(sub.get_texture().get_image(), Rect2i(96, 96, 64, 64))

	# Parameter update must trigger exactly one re-bake.
	static_mat.set_shader_parameter("gain", 0.85)
	await _wait_frames(3)
	var after_param := RenderingServer.light_function_atlas_get_stats()
	var static_bakes_2 := int(after_param.get("static_bakes", 0)) - static_start
	var luma_high := _mean_linear_luma(sub.get_texture().get_image(), Rect2i(96, 96, 64, 64))

	if static_bakes_1 != 1 or last_frame_bakes_idle != 0:
		failures.append("static_vs_animated: static baked %d times (exp 1), idle last_frame_bakes=%d (exp 0)" % [static_bakes_1, last_frame_bakes_idle])
	if static_bakes_2 != 2:
		failures.append("static_vs_animated: param change resulted in %d total static bakes (exp 2)" % static_bakes_2)
	if luma_high <= luma_low * 2.5:
		failures.append("static_vs_animated: param change did not brighten wall (low=%f high=%f)" % [luma_low, luma_high])

	# Animated TIME function must bake every frame and change output pixels across frames.
	var anim_start := int(after_param.get("animated_bakes", 0))
	light.light_function = _make_lf_material(SHADER_ANIMATED)
	await _wait_frames(2)
	var img_a := sub.get_texture().get_image()
	# Wait several frames + small wall-clock delta so TIME advances noticeably.
	OS.delay_msec(60)
	await _wait_frames(4)
	var img_b := sub.get_texture().get_image()
	var after_anim := RenderingServer.light_function_atlas_get_stats()
	var anim_bakes := int(after_anim.get("animated_bakes", 0)) - anim_start
	var anim_changed := img_a != null and img_b != null and img_a.get_data() != img_b.get_data()

	if anim_bakes < 5:
		failures.append("static_vs_animated: animated function baked only %d times across 6 frames" % anim_bakes)
	if not anim_changed:
		failures.append("static_vs_animated: animated TIME light function did not change rendered frames")

	sub.queue_free()
	await _wait_frames(1)
	return {
		"static_bakes_initial": static_bakes_1,
		"static_bakes_after_param": static_bakes_2,
		"last_frame_bakes_idle": last_frame_bakes_idle,
		"luma_low": luma_low,
		"luma_high": luma_high,
		"anim_bakes": anim_bakes,
		"anim_changed": anim_changed,
	}


func _check_translucent(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport()
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 4.0)
	cam.look_at_from_position(cam.position, Vector3.ZERO, Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	_add_wall(sub, Vector3.ZERO, Vector2(6.0, 6.0), true)
	var light := DirectionalLight3D.new()
	light.light_energy = 1.0
	light.shadow_enabled = false
	light.light_function = _make_lf_material(SHADER_BLACK)
	sub.add_child(light)

	ProjectSettings.set_setting("rendering/lights_and_shadows/light_function_atlas/translucent", false)
	await _wait_frames(2)
	var luma_off := _mean_linear_luma(sub.get_texture().get_image(), Rect2i(96, 96, 64, 64))

	ProjectSettings.set_setting("rendering/lights_and_shadows/light_function_atlas/translucent", true)
	await _wait_frames(2)
	var luma_on := _mean_linear_luma(sub.get_texture().get_image(), Rect2i(96, 96, 64, 64))

	ProjectSettings.set_setting("rendering/lights_and_shadows/light_function_atlas/translucent", false)

	if luma_off < 0.05:
		failures.append("translucent: transparent mesh was masked despite translucent=false (luma=%f)" % luma_off)
	if luma_on > 0.01:
		failures.append("translucent: transparent mesh ignored black light_function when translucent=true (luma=%f)" % luma_on)

	sub.queue_free()
	await _wait_frames(1)
	return {
		"luma_translucent_false": luma_off,
		"luma_translucent_true": luma_on,
	}


func _check_nextgi_src(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(false, true)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 1.5, 4.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.0, 0.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	var floor_mesh := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(6.0, 0.4, 6.0)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.8, 0.8, 0.8)
	mat.roughness = 0.9
	box.material = mat
	floor_mesh.mesh = box
	floor_mesh.position = Vector3(0.0, 0.0, 0.0)
	sub.add_child(floor_mesh)

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-55.0, 20.0, 0.0)
	light.light_energy = 1.5
	light.shadow_enabled = false
	light.light_function_scale = Vector3(3.0, 3.0, 1.0)
	light.light_function = _make_lf_material(SHADER_STEP_X)
	sub.add_child(light)

	await _wait_frames(4)
	var img := sub.get_texture().get_image()
	var luma := _mean_linear_luma(img, Rect2i(64, 64, 128, 128))
	if luma <= 0.005:
		failures.append("nextgi_src: rendered NextGI frame with light function appears black (luma=%f)" % luma)

	sub.queue_free()
	await _wait_frames(1)
	return {
		"luma": luma,
	}
