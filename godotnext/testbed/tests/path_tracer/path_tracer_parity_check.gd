extends Node3D

# GodotNext: path_tracer_lights_camera (#169). Feature Parity Room verification suite for the
# product path tracer: exercises every Godot light type (directional + soft angle, omni + radius
# + bake-disabled R14 + >8 lights, spot + cone + projector, area/rect + barn doors + texture,
# and #140 IES profile), sky radiance octmap 2D CDF importance sampling (plus Concave Courtyard
# outdoor check), volumetric fog + FogVolume delta tracking, thin-lens DOF (CameraAttributesPractical),
# and per-instance shutter motion blur. Prints one GODOTNEXT_RESULT JSON line and exits 0/1.

const SAMPLES_PER_CASE := 16
const MAX_WAIT_FRAMES := 80

var _vp: Viewport
var _cam: Camera3D
var _world_env: WorldEnvironment
var _env: Environment
var _floor_mesh: MeshInstance3D
var _wall_mesh: MeshInstance3D
var _dof_left: MeshInstance3D
var _dof_right: MeshInstance3D
var _blocker: MeshInstance3D
var _mover: MeshInstance3D
var _light_root: Node3D
var _fog_vol: FogVolume
var _failures: Array[String] = []
var _checks: Dictionary = {}


func _smoke_scene_arg() -> String:
	for token in OS.get_cmdline_user_args():
		var s := String(token)
		if s.begins_with("--smoke-scene="):
			return s.substr("--smoke-scene=".length())
	return ""


func _ready() -> void:
	_vp = get_viewport()
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/debug/path_tracer_readback", true)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/debug/path_tracer_reference_match", false)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/path_tracing_motion_blur_shutter", 1.0)
	var smoke_path := _smoke_scene_arg()
	if not smoke_path.is_empty():
		_vp.path_tracing_denoiser = Viewport.PATH_TRACING_DENOISER_BUILTIN
		_vp.path_tracing_max_bounces = 4
		_vp.path_tracing_samples = 8
		_vp.path_tracing_enabled = true
		call_deferred("_run_smoke_scene", smoke_path)
		return
	_vp.path_tracing_denoiser = Viewport.PATH_TRACING_DENOISER_OFF
	_vp.path_tracing_max_bounces = 4
	_vp.path_tracing_samples = SAMPLES_PER_CASE
	_vp.path_tracing_enabled = true

	_build_room()
	call_deferred("_run_suite")


func _run_smoke_scene(p_path: String) -> void:
	var packed := load(p_path) as PackedScene
	if packed == null:
		print("GODOTNEXT_RESULT " + JSON.stringify({"pass": false, "scene": p_path, "error": "load_failed"}))
		get_tree().quit(1)
		return
	var inst := packed.instantiate()
	add_child(inst)
	# Ensure NextGI is enabled on the scene's WorldEnvironment if present.
	for child in inst.find_children("*", "WorldEnvironment", true, false):
		var we := child as WorldEnvironment
		if we != null and we.environment != null:
			we.environment.nextgi_enabled = true
	var info := await _wait_samples(8)
	var samples := int(info.get("samples", 0))
	var mean := float(info.get("mean", 0.0))
	var nonfinite := int(info.get("nonfinite", 0))
	var ok := samples >= 8 and nonfinite == 0 and mean > 0.02
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"pass": ok,
		"scene": p_path,
		"backend": String(info.get("backend", "")),
		"samples": samples,
		"mean": mean,
		"nonfinite": nonfinite,
		"light_count": int(info.get("light_count", 0)),
		"sky_importance_active": bool(info.get("sky_importance_active", false)),
		"volumetric_fog_active": bool(info.get("volumetric_fog_active", false)),
		"denoised": bool(info.get("denoised", false)),
	}))
	get_tree().quit(0 if ok else 1)


func _make_mat(col: Color, rough: float = 0.9, tex: Texture2D = null) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = col
	m.roughness = rough
	m.metallic = 0.0
	if tex != null:
		m.albedo_texture = tex
	return m


func _make_checker_tex(w: int, h: int, cell: int, c0: Color, c1: Color) -> ImageTexture:
	var img := Image.create(w, h, false, Image.FORMAT_RGBA8)
	for y in range(h):
		for x in range(w):
			var parity := ((x / cell) + (y / cell)) & 1
			img.set_pixel(x, y, c0 if parity == 0 else c1)
	return ImageTexture.create_from_image(img)


func _build_room() -> void:
	_env = Environment.new()
	_env.background_mode = Environment.BG_COLOR
	_env.background_color = Color(0.0, 0.0, 0.0)
	_env.background_energy_multiplier = 1.0
	_env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	_env.nextgi_enabled = true
	_env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	_world_env = WorldEnvironment.new()
	_world_env.environment = _env
	add_child(_world_env)

	_cam = Camera3D.new()
	_cam.fov = 60.0
	add_child(_cam)
	_cam.look_at_from_position(Vector3(0.0, 2.0, 5.0), Vector3(0.0, 1.2, 0.0), Vector3.UP)

	var floor_box := BoxMesh.new()
	floor_box.size = Vector3(12.0, 0.2, 12.0)
	_floor_mesh = MeshInstance3D.new()
	_floor_mesh.mesh = floor_box
	_floor_mesh.position = Vector3(0.0, -0.1, 0.0)
	_floor_mesh.material_override = _make_mat(Color(0.8, 0.8, 0.8))
	add_child(_floor_mesh)

	var wall_box := BoxMesh.new()
	wall_box.size = Vector3(12.0, 6.0, 0.2)
	_wall_mesh = MeshInstance3D.new()
	_wall_mesh.mesh = wall_box
	_wall_mesh.position = Vector3(0.0, 3.0, -3.5)
	_wall_mesh.material_override = _make_mat(Color(0.8, 0.8, 0.8))
	add_child(_wall_mesh)

	# Side-by-side white/black step edge panels at z = -3.35 for backend-independent (HIT_GROUPS + INLINE) DOF test.
	var half_box := BoxMesh.new()
	half_box.size = Vector3(4.0, 4.0, 0.2)
	_dof_left = MeshInstance3D.new()
	_dof_left.mesh = half_box
	_dof_left.position = Vector3(-2.0, 2.0, -3.35)
	var white_emit := _make_mat(Color(1.0, 1.0, 1.0))
	white_emit.emission_enabled = true
	white_emit.emission = Color(1.0, 1.0, 1.0)
	white_emit.emission_energy_multiplier = 2.0
	_dof_left.material_override = white_emit
	_dof_left.visible = false
	add_child(_dof_left)

	_dof_right = MeshInstance3D.new()
	_dof_right.mesh = half_box
	_dof_right.position = Vector3(2.0, 2.0, -3.35)
	var black_mat := _make_mat(Color(0.0, 0.0, 0.0), 1.0)
	black_mat.metallic_specular = 0.0
	_dof_right.material_override = black_mat
	_dof_right.visible = false
	add_child(_dof_right)

	var blocker_box := BoxMesh.new()
	blocker_box.size = Vector3(1.2, 1.2, 1.2)
	_blocker = MeshInstance3D.new()
	_blocker.mesh = blocker_box
	_blocker.position = Vector3(0.0, 0.8, 0.0)
	_blocker.material_override = _make_mat(Color(0.7, 0.7, 0.7))
	add_child(_blocker)

	var mover_box := BoxMesh.new()
	mover_box.size = Vector3(0.8, 0.8, 0.8)
	_mover = MeshInstance3D.new()
	_mover.mesh = mover_box
	_mover.position = Vector3(-2.0, 1.4, 1.0)
	var mover_mat := _make_mat(Color(1.0, 0.2, 0.1))
	mover_mat.emission_enabled = true
	mover_mat.emission = Color(1.0, 0.25, 0.1)
	mover_mat.emission_energy_multiplier = 3.0
	_mover.material_override = mover_mat
	_mover.visible = false
	add_child(_mover)

	_light_root = Node3D.new()
	add_child(_light_root)


func _clear_lights() -> void:
	for c in _light_root.get_children():
		_light_root.remove_child(c)
		c.queue_free()
	if _fog_vol != null:
		remove_child(_fog_vol)
		_fog_vol.queue_free()
		_fog_vol = null
	_env.background_mode = Environment.BG_COLOR
	_env.background_color = Color(0.0, 0.0, 0.0)
	_env.sky = null
	_env.volumetric_fog_enabled = false
	_env.fog_enabled = false
	_cam.attributes = null
	_dof_left.visible = false
	_dof_right.visible = false
	_blocker.visible = true
	_mover.visible = false


func _wait_samples(target_spp: int) -> Dictionary:
	_vp.path_tracing_samples = target_spp
	for _i in range(MAX_WAIT_FRAMES):
		await get_tree().process_frame
		var info := _vp.get_path_tracing_info()
		if int(info.get("samples", 0)) >= target_spp:
			return info
	return _vp.get_path_tracing_info()


func _mean_rgb(info: Dictionary, x0_frac: float = 0.0, y0_frac: float = 0.0, x1_frac: float = 1.0, y1_frac: float = 1.0) -> Vector3:
	var data: PackedByteArray = info.get("accumulation", PackedByteArray())
	var w := int(info.get("width", 0))
	var h := int(info.get("height", 0))
	if w <= 0 or h <= 0 or data.size() < w * h * 16:
		return Vector3.ZERO
	var floats := data.to_float32_array()
	var x0 := clampi(int(floor(x0_frac * w)), 0, w - 1)
	var x1 := clampi(int(ceil(x1_frac * w)), x0 + 1, w)
	var y0 := clampi(int(floor(y0_frac * h)), 0, h - 1)
	var y1 := clampi(int(ceil(y1_frac * h)), y0 + 1, h)
	var sum := Vector3.ZERO
	var count := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			var idx := (y * w + x) * 4
			var a := floats[idx + 3]
			if a > 0.0:
				sum += Vector3(floats[idx], floats[idx + 1], floats[idx + 2]) / a
				count += 1
	return sum / float(maxi(count, 1))


func _region_contrast(info: Dictionary, x0_frac: float, y0_frac: float, x1_frac: float, y1_frac: float) -> float:
	var data: PackedByteArray = info.get("accumulation", PackedByteArray())
	var w := int(info.get("width", 0))
	var h := int(info.get("height", 0))
	if w <= 0 or h <= 0 or data.size() < w * h * 16:
		return 0.0
	var floats := data.to_float32_array()
	var x0 := clampi(int(floor(x0_frac * w)), 0, w - 2)
	var x1 := clampi(int(ceil(x1_frac * w)), x0 + 1, w - 1)
	var y0 := clampi(int(floor(y0_frac * h)), 0, h - 2)
	var y1 := clampi(int(ceil(y1_frac * h)), y0 + 1, h - 1)
	var grad_sum := 0.0
	var count := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			var i0 := (y * w + x) * 4
			var i1 := (y * w + x + 1) * 4
			var a0 := maxf(floats[i0 + 3], 1e-6)
			var a1 := maxf(floats[i1 + 3], 1e-6)
			var l0 := (floats[i0] + floats[i0 + 1] + floats[i0 + 2]) / (3.0 * a0)
			var l1 := (floats[i1] + floats[i1 + 1] + floats[i1 + 2]) / (3.0 * a1)
			grad_sum += absf(l1 - l0)
			count += 1
	return grad_sum / float(maxi(count, 1))


func _record(case_name: String, ok: bool, detail: Dictionary) -> void:
	detail["pass"] = ok
	_checks[case_name] = detail
	if not ok:
		_failures.append(case_name)


func _run_suite() -> void:
	# Warm up TLAS and hit groups before measuring cases.
	for _i in range(4):
		await get_tree().process_frame

	await _check_directional()
	await _check_omni_r14_and_many_lights()
	await _check_spot_and_projector()
	await _check_area_barndoor_and_texture()
	await _check_ies_profile()
	await _check_sky_importance()
	await _check_volumetric_fog_and_volume()
	await _check_thin_lens_dof()
	await _check_motion_blur()

	var result := {
		"pass": _failures.is_empty(),
		"failures": _failures,
		"checks": _checks,
	}
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if _failures.is_empty() else 1)


func _check_directional() -> void:
	_clear_lights()
	var sun := DirectionalLight3D.new()
	sun.light_energy = 1.5
	sun.light_color = Color(1.0, 0.95, 0.85)
	sun.light_angular_distance = 0.0
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-45.0, 30.0, 0.0)
	_light_root.add_child(sun)
	var hard := await _wait_samples(SAMPLES_PER_CASE)
	var hard_mean := float(hard.get("mean", 0.0))
	var hard_hash := String(hard.get("hash", ""))

	sun.light_angular_distance = 12.0
	var soft := await _wait_samples(SAMPLES_PER_CASE)
	var soft_mean := float(soft.get("mean", 0.0))
	var soft_hash := String(soft.get("hash", ""))
	var nonfinite := int(hard.get("nonfinite", 0)) + int(soft.get("nonfinite", 0))
	var ok := nonfinite == 0 and hard_mean > 0.05 and soft_mean > 0.05 and hard_hash != soft_hash and int(soft.get("light_count", 0)) == 1
	_record("directional_soft", ok, {
		"hard_mean": hard_mean,
		"soft_mean": soft_mean,
		"light_count": int(soft.get("light_count", 0)),
		"nonfinite": nonfinite,
	})


func _check_omni_r14_and_many_lights() -> void:
	_clear_lights()
	# Add 10 omni lights (>8 cap from #167 R14!), including bake-disabled lights behind the camera.
	var disabled_lights: Array[OmniLight3D] = []
	for i in range(10):
		var omni := OmniLight3D.new()
		omni.omni_range = 14.0
		omni.light_energy = 0.35
		omni.light_size = 0.2
		omni.shadow_enabled = true
		omni.light_bake_mode = Light3D.BAKE_DISABLED if (i % 2 == 0) else Light3D.BAKE_DYNAMIC
		# Place half of them behind the camera (z > 6) so camera frustum culling would drop them!
		omni.position = Vector3(-3.0 + float(i) * 0.65, 2.5, 6.5 if (i >= 5) else 1.5)
		_light_root.add_child(omni)
		if omni.light_bake_mode == Light3D.BAKE_DISABLED:
			disabled_lights.append(omni)
	var info_on := await _wait_samples(SAMPLES_PER_CASE)
	var count_on := int(info_on.get("light_count", 0))
	var mean_on := float(info_on.get("mean", 0.0))

	# Zero out only the bake-disabled lights to prove they actively illuminate the path tracer.
	for dl in disabled_lights:
		dl.light_energy = 0.0
	var info_off := await _wait_samples(SAMPLES_PER_CASE)
	var mean_off := float(info_off.get("mean", 0.0))
	var nonfinite := int(info_on.get("nonfinite", 0)) + int(info_off.get("nonfinite", 0))
	var ok := nonfinite == 0 and count_on == 10 and mean_on > mean_off + 0.05
	_record("omni_r14_bake_disabled_gt8", ok, {
		"light_count": count_on,
		"mean_all_on": mean_on,
		"mean_disabled_zeroed": mean_off,
		"nonfinite": nonfinite,
	})


func _check_spot_and_projector() -> void:
	_clear_lights()
	_blocker.visible = false
	var spot := SpotLight3D.new()
	spot.position = Vector3(0.0, 2.0, 1.0)
	spot.look_at_from_position(Vector3(0.0, 2.0, 1.0), Vector3(0.0, 2.0, -3.5), Vector3.UP)
	spot.spot_range = 12.0
	spot.spot_angle = 22.0
	spot.spot_attenuation = 1.0
	spot.light_energy = 4.0
	spot.shadow_enabled = true
	_light_root.add_child(spot)
	var plain := await _wait_samples(SAMPLES_PER_CASE)
	var center_plain := _mean_rgb(plain, 0.42, 0.35, 0.58, 0.60)
	var edge_plain := _mean_rgb(plain, 0.02, 0.05, 0.12, 0.25)

	# Attach a red/black projector checkerboard to verify perspective projector modulation.
	spot.light_projector = _make_checker_tex(64, 64, 8, Color(1.0, 0.1, 0.1), Color(0.0, 0.0, 0.0))
	var proj := await _wait_samples(SAMPLES_PER_CASE)
	var center_proj := _mean_rgb(proj, 0.42, 0.35, 0.58, 0.60)
	var nonfinite := int(plain.get("nonfinite", 0)) + int(proj.get("nonfinite", 0))
	var cone_ok := center_plain.x > edge_plain.x * 3.0 + 0.02
	var proj_red_shift := center_proj.x > center_proj.y * 2.5 and center_proj.x > 0.02
	_record("spot_cone_projector", nonfinite == 0 and cone_ok and proj_red_shift, {
		"center_plain": [center_plain.x, center_plain.y, center_plain.z],
		"edge_plain": [edge_plain.x, edge_plain.y, edge_plain.z],
		"center_proj": [center_proj.x, center_proj.y, center_proj.z],
		"nonfinite": nonfinite,
	})


func _check_area_barndoor_and_texture() -> void:
	_clear_lights()
	_blocker.visible = false
	var area := AreaLight3D.new()
	area.position = Vector3(0.0, 2.2, 0.5)
	area.look_at_from_position(Vector3(0.0, 2.2, 0.5), Vector3(0.0, 2.2, -3.5), Vector3.UP)
	area.area_size = Vector2(2.0, 1.5)
	area.area_range = 14.0
	area.area_normalize_energy = false
	area.light_energy = 2.0
	area.shadow_enabled = true
	_light_root.add_child(area)
	var open_info := await _wait_samples(SAMPLES_PER_CASE)
	var open_mean := float(open_info.get("mean", 0.0))

	# Close barn doors tightly: outer radiance drops sharply.
	area.barn_door_angle = 12.0
	area.barn_door_length = 2.0
	var barn_info := await _wait_samples(SAMPLES_PER_CASE)
	var barn_mean := float(barn_info.get("mean", 0.0))

	# Open barn doors and attach green-tinted area texture.
	area.barn_door_angle = 90.0
	area.barn_door_length = 0.0
	area.area_texture = _make_checker_tex(32, 32, 8, Color(0.05, 1.0, 0.1), Color(0.02, 0.6, 0.05))
	var tex_info := await _wait_samples(SAMPLES_PER_CASE)
	var tex_rgb := _mean_rgb(tex_info, 0.3, 0.25, 0.7, 0.75)
	var nonfinite := int(open_info.get("nonfinite", 0)) + int(barn_info.get("nonfinite", 0)) + int(tex_info.get("nonfinite", 0))
	var ok := nonfinite == 0 and open_mean > 0.05 and barn_mean < open_mean * 0.75 and tex_rgb.y > tex_rgb.x * 2.5 and tex_rgb.y > 0.02
	_record("area_barndoor_texture", ok, {
		"open_mean": open_mean,
		"barn_mean": barn_mean,
		"tex_rgb": [tex_rgb.x, tex_rgb.y, tex_rgb.z],
		"nonfinite": nonfinite,
	})


func _check_ies_profile() -> void:
	_clear_lights()
	_blocker.visible = false
	var omni := OmniLight3D.new()
	omni.position = Vector3(0.0, 2.2, 0.5)
	omni.look_at_from_position(Vector3(0.0, 2.2, 0.5), Vector3(0.0, 2.2, -3.5), Vector3.UP)
	omni.omni_range = 14.0
	omni.light_energy = 3.0
	omni.shadow_enabled = false
	_light_root.add_child(omni)
	var plain := await _wait_samples(SAMPLES_PER_CASE)
	var plain_mean := float(plain.get("mean", 0.0))

	# Synthetic narrow-beam IES profile (bright at theta=0, 0 past 30 deg).
	var ies_text := "IESNA:LM-63-2002\nTILT=NONE\n1 1000 1 5 1 1 2 0 0 0\n1 1 100\n0 15 30 90 180\n0\n1000 800 0 0 0\n"
	var ies := IESProfile.new()
	var err: Error = ies.load_from_ies_string(ies_text)
	omni.ies_profile = ies
	var with_ies := await _wait_samples(SAMPLES_PER_CASE)
	var ies_mean := float(with_ies.get("mean", 0.0))
	var nonfinite := int(plain.get("nonfinite", 0)) + int(with_ies.get("nonfinite", 0))
	var ok := err == OK and nonfinite == 0 and plain_mean > 0.05 and ies_mean > 0.005 and ies_mean < plain_mean * 0.65
	_record("ies_profile", ok, {
		"parse_ok": err == OK,
		"plain_mean": plain_mean,
		"ies_mean": ies_mean,
		"nonfinite": nonfinite,
	})


func _check_sky_importance() -> void:
	_clear_lights()
	var sky_mat := ProceduralSkyMaterial.new()
	sky_mat.sky_top_color = Color(0.35, 0.55, 0.95)
	sky_mat.sky_horizon_color = Color(0.75, 0.80, 0.90)
	sky_mat.ground_bottom_color = Color(0.2, 0.2, 0.2)
	sky_mat.sky_energy_multiplier = 1.5
	var sky_res := Sky.new()
	sky_res.sky_material = sky_mat
	_env.background_mode = Environment.BG_SKY
	_env.sky = sky_res
	_env.background_energy_multiplier = 1.0
	# Wait an extra frame so sky radiance octmap is baked, then accumulate sky-lit samples (no lights!).
	await get_tree().process_frame
	var info := await _wait_samples(SAMPLES_PER_CASE)
	var floor_rgb := _mean_rgb(info, 0.3, 0.65, 0.7, 0.92)
	var sky_active := bool(info.get("sky_importance_active", false))
	var nonfinite := int(info.get("nonfinite", 0))
	var ok := nonfinite == 0 and sky_active and floor_rgb.length() > 0.08
	_record("sky_importance_sampling", ok, {
		"sky_importance_active": sky_active,
		"floor_rgb": [floor_rgb.x, floor_rgb.y, floor_rgb.z],
		"mean": float(info.get("mean", 0.0)),
		"nonfinite": nonfinite,
	})


func _check_volumetric_fog_and_volume() -> void:
	_clear_lights()
	var sun := DirectionalLight3D.new()
	sun.light_energy = 1.5
	sun.rotation_degrees = Vector3(-40.0, 20.0, 0.0)
	_light_root.add_child(sun)
	var clear_info := await _wait_samples(SAMPLES_PER_CASE)
	var clear_rgb := _mean_rgb(clear_info, 0.35, 0.35, 0.65, 0.65)

	_env.volumetric_fog_enabled = true
	_env.volumetric_fog_density = 0.08
	_env.volumetric_fog_albedo = Color(0.85, 0.9, 1.0)
	_env.volumetric_fog_anisotropy = 0.4
	_env.volumetric_fog_length = 30.0
	_env.volumetric_fog_emission = Color(0.0, 0.0, 0.0)

	_fog_vol = FogVolume.new()
	_fog_vol.shape = RenderingServer.FOG_VOLUME_SHAPE_BOX
	_fog_vol.size = Vector3(3.0, 3.0, 2.0)
	_fog_vol.position = Vector3(0.0, 1.5, 2.0)
	var fmat := FogMaterial.new()
	fmat.density = 0.6
	fmat.albedo = Color(0.2, 0.9, 0.3)
	fmat.emission = Color(0.05, 0.45, 0.1)
	fmat.edge_fade = 0.1
	_fog_vol.material = fmat
	add_child(_fog_vol)

	var fog_info := await _wait_samples(SAMPLES_PER_CASE)
	var fog_rgb := _mean_rgb(fog_info, 0.35, 0.35, 0.65, 0.65)
	var nonfinite := int(clear_info.get("nonfinite", 0)) + int(fog_info.get("nonfinite", 0))
	var vfog_active := bool(fog_info.get("volumetric_fog_active", false))
	var fvol_count := int(fog_info.get("fog_volume_count", 0))
	# Green emissive/scattering FogVolume in front of camera shifts center green ratio upward.
	var green_shift := (fog_rgb.y - fog_rgb.x) > (clear_rgb.y - clear_rgb.x) + 0.05
	_record("volumetric_fog_and_volume", nonfinite == 0 and vfog_active and fvol_count >= 1 and green_shift, {
		"volumetric_fog_active": vfog_active,
		"fog_volume_count": fvol_count,
		"clear_rgb": [clear_rgb.x, clear_rgb.y, clear_rgb.z],
		"fog_rgb": [fog_rgb.x, fog_rgb.y, fog_rgb.z],
		"nonfinite": nonfinite,
	})


func _check_thin_lens_dof() -> void:
	_clear_lights()
	_blocker.visible = false
	_dof_left.visible = true
	_dof_right.visible = true

	# Pinhole baseline: narrow dark strip right inside the CoC band next to center edge (x in [0.503, 0.518]).
	var sharp := await _wait_samples(24)
	var bleed_sharp := _mean_rgb(sharp, 0.503, 0.30, 0.518, 0.70).x

	# Out-of-focus thin-lens DOF (focus = 2.0m, wall = ~8.35m): bright left panel smears across the edge.
	var attr := CameraAttributesPractical.new()
	attr.dof_blur_far_enabled = true
	attr.dof_blur_far_distance = 2.0
	attr.dof_blur_far_transition = 2.0
	attr.dof_blur_amount = 0.7
	_cam.attributes = attr
	var blurred := await _wait_samples(24)
	var bleed_blur := _mean_rgb(blurred, 0.503, 0.30, 0.518, 0.70).x

	# Focused right on the wall (~8.35m): circle of confusion collapses back to sharp!
	attr.dof_blur_far_distance = 8.35
	var focused := await _wait_samples(24)
	var bleed_focused := _mean_rgb(focused, 0.503, 0.30, 0.518, 0.70).x

	var nonfinite := int(sharp.get("nonfinite", 0)) + int(blurred.get("nonfinite", 0)) + int(focused.get("nonfinite", 0))
	var dof_active := bool(blurred.get("dof_active", false))
	var ok := nonfinite == 0 and dof_active and bleed_blur > bleed_sharp + 0.15 and bleed_focused < bleed_blur * 0.25
	_record("thin_lens_dof", ok, {
		"dof_active": dof_active,
		"bleed_sharp": bleed_sharp,
		"bleed_blur": bleed_blur,
		"bleed_focused": bleed_focused,
		"nonfinite": nonfinite,
	})


func _check_motion_blur() -> void:
	_clear_lights()
	_blocker.visible = false
	_mover.visible = true
	_mover.position = Vector3(-2.5, 1.4, 1.0)
	_vp.path_tracing_samples = 4
	await get_tree().process_frame

	# Move continuously for 3 frames so motion streak >= 2 activates 8-slice shutter motion blur.
	var positions := [Vector3(-1.5, 1.4, 1.0), Vector3(-0.2, 1.4, 1.0), Vector3(1.2, 1.4, 1.0)]
	var blur_info: Dictionary = {}
	for p in positions:
		_mover.position = p
		await get_tree().process_frame
		blur_info = _vp.get_path_tracing_info()
	var motion_active := bool(blur_info.get("motion_blur_active", false))
	var moving_count := int(blur_info.get("motion_moving_instances", 0))
	# Sample center strip (where the cube swept through between -0.2 and +1.2, behind its resting end at +1.2).
	var trail_blur := _mean_rgb(blur_info, 0.42, 0.35, 0.56, 0.65)

	# Repeat identical motion with shutter = 0.0 (no motion blur): center strip behind x=1.2 must be dark.
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/path_tracing_motion_blur_shutter", 0.0)
	_mover.position = Vector3(-2.5, 1.4, 1.0)
	await get_tree().process_frame
	var sharp_info: Dictionary = {}
	for p in positions:
		_mover.position = p
		await get_tree().process_frame
		sharp_info = _vp.get_path_tracing_info()
	var trail_sharp := _mean_rgb(sharp_info, 0.42, 0.35, 0.56, 0.65)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/path_tracing_motion_blur_shutter", 1.0)

	var nonfinite := int(blur_info.get("nonfinite", 0)) + int(sharp_info.get("nonfinite", 0))
	var ok := nonfinite == 0 and motion_active and moving_count >= 1 and trail_blur.x > trail_sharp.x + 0.05
	_record("motion_blur", ok, {
		"motion_blur_active": motion_active,
		"motion_moving_instances": moving_count,
		"trail_blur_r": trail_blur.x,
		"trail_sharp_r": trail_sharp.x,
		"nonfinite": nonfinite,
	})
