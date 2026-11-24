extends Node3D
## #140: Light-source parity GPU verification (plan §21.3 contract M8, GM-11).
## Verifies:
##  (a) GM-11 calibrated wall IES intensity matches closed-form analytic profiles
##      within 2% mean / 5% max error across 4 generated synthetic profiles
##      (isotropic, cosine_lobe, batwing, asymmetric), plus SpotLight3D cone
##      clipping, ies_use_intensity / ies_intensity_scale scaling, and volumetric
##      fog IES sampling without validation errors.
##  (b) AreaLight3D barn-door shading: barn_door_angle >= 89.99 deg matches
##      unclipped baseline; narrowing to 20 deg visibly cuts off illumination
##      outside the barn-door frustum while keeping center lit.
##  (c) Textured rect lights: AreaLight3D.area_texture color modulates Forward+
##      shading AND (when NextGI is enabled via run_light_source_parity_check.sh
##      or override.cfg) modulates NextGI SRC world-space relight page probes.
##  (d) Per-light contact shadows: contact_shadow_length = 0.0 is byte-identical
##      to contact shadows off; casting vs non-casting intensities affect only
##      their respective pixels; local light + world-space length works cleanly.

const WALL_SIZE := 256
const WALL_ORTHO_EXTENT := 4.0 # Visible wall covers x, y in [-2, +2].
const WALL_Z := -2.0
const LIGHT_Z := -1.0 # Distance from light to wall = 1.0 m.


func _ready() -> void:
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
	var mode := _arg("check", "all")
	var require_nextgi := _arg("require-nextgi", "0") == "1"
	var failures := PackedStringArray()
	var report := {
		"test": "light_source_parity",
		"mode": mode,
		"nextgi_enabled": bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false)),
		"pass": false,
	}

	# Keep root 3D disabled for SubViewport calibrated checks; re-enable for NextGI SRC check.
	get_tree().root.disable_3d = true

	if mode == "all" or mode == "ies":
		report["ies"] = await _check_ies_wall(failures)
	if mode == "all" or mode == "barn_door":
		report["barn_door"] = await _check_barn_door(failures)
	if mode == "all" or mode == "textured_rect":
		report["textured_rect"] = await _check_textured_rect(failures, require_nextgi)
	if mode == "all" or mode == "contact_shadow":
		report["contact_shadow"] = await _check_contact_shadows(failures)

	report["failures"] = failures
	report["pass"] = failures.is_empty()
	print("LIGHT_SOURCE_PARITY_RESULT=" + JSON.stringify(report))
	get_tree().quit(0 if failures.is_empty() else 1)


func _wait_frames(count: int = 3) -> void:
	for _i in range(count):
		await RenderingServer.frame_post_draw


func _make_calibrated_viewport(size: Vector2i, use_hdr: bool) -> SubViewport:
	var sub := SubViewport.new()
	sub.own_world_3d = true
	sub.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	sub.msaa_3d = Viewport.MSAA_DISABLED
	sub.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	sub.positional_shadow_atlas_size = 0
	sub.use_hdr_2d = use_hdr
	sub.size = size
	add_child(sub)

	var world := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_energy = 0.0
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.tonemap_exposure = 1.0
	world.environment = env
	world.name = "WorldEnvironment"
	sub.add_child(world)
	return sub


func _add_calibrated_wall(parent: Node) -> MeshInstance3D:
	var wall := MeshInstance3D.new()
	var quad := QuadMesh.new()
	quad.size = Vector2(6.0, 6.0)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(1.0, 1.0, 1.0)
	mat.roughness = 1.0
	mat.metallic = 0.0
	mat.metallic_specular = 0.0
	mat.specular_mode = BaseMaterial3D.SPECULAR_DISABLED
	quad.material = mat
	wall.mesh = quad
	wall.position = Vector3(0.0, 0.0, WALL_Z)
	parent.add_child(wall)
	return wall


func _add_ortho_wall_camera(parent: Node) -> Camera3D:
	var cam := Camera3D.new()
	cam.projection = Camera3D.PROJECTION_ORTHOGONAL
	cam.size = WALL_ORTHO_EXTENT
	cam.position = Vector3(0.0, 0.0, 2.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.0, WALL_Z), Vector3.UP)
	cam.current = true
	parent.add_child(cam)
	return cam


func _world_to_pixel(wx: float, wy: float, size: int) -> Vector2i:
	var u := (wx / WALL_ORTHO_EXTENT) + 0.5
	var v := 0.5 - (wy / WALL_ORTHO_EXTENT)
	var px := clampi(int(round(u * float(size) - 0.5)), 0, size - 1)
	var py := clampi(int(round(v * float(size) - 0.5)), 0, size - 1)
	return Vector2i(px, py)


func _pixel_to_wall_world(px: int, py: int, size: int) -> Vector3:
	var u := (float(px) + 0.5) / float(size)
	var v := (float(py) + 0.5) / float(size)
	return Vector3((u - 0.5) * WALL_ORTHO_EXTENT, (0.5 - v) * WALL_ORTHO_EXTENT, WALL_Z)


func _build_ies_text(v_angles: PackedFloat32Array, h_angles: PackedFloat32Array, candela: PackedFloat32Array) -> String:
	var lines := PackedStringArray([
		"IESNA:LM-63-2002",
		"[TEST] GodotNext synthetic GPU verification profile (#140)",
		"TILT=NONE",
		"1 1000.0 1.0 %d %d 1 2 0.0 0.0 0.0" % [v_angles.size(), h_angles.size()],
		"1.0 0.0 10.0",
	])
	var vs := PackedStringArray()
	for a in v_angles:
		vs.append("%.3f" % a)
	lines.append(" ".join(vs))
	var hs := PackedStringArray()
	for a in h_angles:
		hs.append("%.3f" % a)
	lines.append(" ".join(hs))
	var cs := PackedStringArray()
	for c in candela:
		cs.append("%.4f" % c)
	lines.append(" ".join(cs))
	return "\n".join(lines) + "\n"


func _eval_analytic_profile(profile_name: String, dir_ls: Vector3) -> float:
	var d := dir_ls.normalized()
	var cos_theta := clampf(-d.z, -1.0, 1.0)
	var theta := acos(cos_theta)
	var phi := atan2(d.y, d.x)
	if phi < 0.0:
		phi += TAU
	match profile_name:
		"isotropic":
			return 1.0
		"cosine_lobe":
			return maxf(cos_theta, 0.0)
		"batwing":
			if theta >= PI * 0.5:
				return 0.0
			var s := sin(theta)
			var c := cos(theta)
			return 4.0 * s * s * c * c
		"asymmetric":
			if cos_theta <= 0.0:
				return 0.0
			# Smooth, pole-consistent asymmetric lobe (single-valued at theta = 0, strongly asymmetric off-axis).
			var s := sin(theta)
			var raw := cos_theta * (0.55 + s * (0.35 * cos(phi) + 0.25 * sin(phi)))
			return maxf(raw, 0.0) / 0.664043
	return 1.0


func _make_synthetic_profile(profile_name: String, peak_cd: float = 1000.0) -> IESProfile:
	var v_angles := PackedFloat32Array()
	for i in range(73):
		v_angles.append(float(i) * 2.5)
	var h_angles := PackedFloat32Array()
	if profile_name == "asymmetric":
		for i in range(37):
			h_angles.append(float(i) * 10.0)
	else:
		h_angles.append(0.0)
	var cd := PackedFloat32Array()
	for hi in range(h_angles.size()):
		var phi := deg_to_rad(h_angles[hi])
		for vi in range(v_angles.size()):
			var theta := deg_to_rad(v_angles[vi])
			var d := Vector3(sin(theta) * cos(phi), sin(theta) * sin(phi), -cos(theta))
			cd.append(_eval_analytic_profile(profile_name, d) * peak_cd)
	var prof := IESProfile.new()
	var err := prof.load_from_ies_string(_build_ies_text(v_angles, h_angles, cd))
	assert(err == OK)
	return prof


func _check_ies_wall(failures: PackedStringArray) -> Dictionary:
	var sub := _make_calibrated_viewport(Vector2i(WALL_SIZE, WALL_SIZE), true)
	_add_calibrated_wall(sub)
	_add_ortho_wall_camera(sub)

	var omni := OmniLight3D.new()
	omni.position = Vector3(0.0, 0.0, LIGHT_Z)
	omni.omni_range = 10.0
	omni.omni_attenuation = 2.0
	omni.light_energy = 1.0
	omni.shadow_enabled = false
	sub.add_child(omni)

	# Baseline render without IES profile.
	await _wait_frames(3)
	var img_base := sub.get_texture().get_image()
	var center_px := _world_to_pixel(0.0, 0.0, WALL_SIZE)
	var base_center := img_base.get_pixel(center_px.x, center_px.y).r
	_check(failures, base_center > 0.1, "IES baseline wall center lit (got %.4f)" % base_center)

	var profile_results := {}
	for pname in ["isotropic", "cosine_lobe", "batwing", "asymmetric"]:
		var prof := _make_synthetic_profile(pname, 1000.0)
		omni.ies_profile = prof
		omni.ies_use_intensity = false
		omni.ies_intensity_scale = 1.0
		await _wait_frames(3)
		var img_ies := sub.get_texture().get_image()

		var err_sum := 0.0
		var max_err := 0.0
		var samples := 0
		for gy in range(-5, 6):
			for gx in range(-5, 6):
				var target_wx := float(gx) * 0.3
				var target_wy := float(gy) * 0.3
				var px := _world_to_pixel(target_wx, target_wy, WALL_SIZE)
				var wall_pt := _pixel_to_wall_world(px.x, px.y, WALL_SIZE)
				var base_val := img_base.get_pixel(px.x, px.y).r
				if base_val <= 1e-4:
					continue
				var measured := img_ies.get_pixel(px.x, px.y).r / base_val
				var dir_ls := (wall_pt - omni.position).normalized()
				var expected := _eval_analytic_profile(pname, dir_ls)
				var err := absf(measured - expected)
				err_sum += err
				max_err = maxf(max_err, err)
				samples += 1

		var mean_err := err_sum / float(maxi(samples, 1))
		profile_results[pname] = {
			"samples": samples,
			"mean_err": snappedf(mean_err, 0.0001),
			"max_err": snappedf(max_err, 0.0001),
		}
		_check(failures, samples >= 100, "IES %s sample count (%d)" % [pname, samples])
		_check(failures, mean_err <= 0.02, "GM-11 IES %s mean error %.4f <= 0.02" % [pname, mean_err])
		_check(failures, max_err <= 0.05, "GM-11 IES %s max error %.4f <= 0.05" % [pname, max_err])

	# Verify ies_use_intensity & ies_intensity_scale scaling.
	var scale_prof := _make_synthetic_profile("isotropic", 4.0)
	omni.ies_profile = scale_prof
	omni.ies_use_intensity = false
	omni.ies_intensity_scale = 1.0
	await _wait_frames(3)
	var lum_no_int := sub.get_texture().get_image().get_pixel(center_px.x, center_px.y).r

	omni.ies_use_intensity = true
	omni.ies_intensity_scale = 0.5 # Effective scale = 4.0 * 0.5 = 2.0x.
	await _wait_frames(3)
	var lum_with_int := sub.get_texture().get_image().get_pixel(center_px.x, center_px.y).r
	var int_ratio := lum_with_int / maxf(lum_no_int, 1e-6)
	_check(failures, absf(int_ratio - 2.0) < 0.05, "IES use_intensity * scale ratio %.4f ~= 2.0" % int_ratio)

	# Verify SpotLight3D + IES clips cleanly to the spot cone.
	omni.visible = false
	var spot := SpotLight3D.new()
	spot.position = Vector3(0.0, 0.0, LIGHT_Z)
	spot.spot_range = 10.0
	spot.spot_angle = 25.0
	spot.spot_angle_attenuation = 1.0
	spot.light_energy = 1.0
	spot.shadow_enabled = false
	spot.ies_profile = _make_synthetic_profile("isotropic", 1000.0)
	sub.add_child(spot)
	await _wait_frames(3)
	var img_spot := sub.get_texture().get_image()
	var spot_center := img_spot.get_pixel(center_px.x, center_px.y).r
	var outside_px := _world_to_pixel(1.2, 0.0, WALL_SIZE) # atan(1.2/1.0) = 50.2 deg >> 25 deg cone.
	var spot_outside := img_spot.get_pixel(outside_px.x, outside_px.y).r
	_check(failures, spot_center > 0.1, "SpotLight3D + IES center lit (%.4f)" % spot_center)
	_check(failures, spot_outside < 0.001, "SpotLight3D + IES clips outside spot cone (%.6f)" % spot_outside)

	# Volumetric fog smoke check with IES profile (exercises fog Set 0 binding 27 under VVL SyncVal).
	var world_env := sub.get_node("WorldEnvironment") as WorldEnvironment
	world_env.environment.volumetric_fog_enabled = true
	world_env.environment.volumetric_fog_density = 0.08
	omni.visible = true
	await _wait_frames(3)
	var fog_center := sub.get_texture().get_image().get_pixel(center_px.x, center_px.y).r
	_check(failures, is_finite(fog_center) and fog_center > 0.0, "Volumetric fog + IES renders cleanly (%.4f)" % fog_center)

	sub.queue_free()
	await _wait_frames(2)
	return {
		"profiles": profile_results,
		"intensity_scale_ratio": snappedf(int_ratio, 0.0001),
		"spot_center": snappedf(spot_center, 0.0001),
		"spot_outside": snappedf(spot_outside, 0.0001),
	}


func _check_barn_door(failures: PackedStringArray) -> Dictionary:
	var sub := _make_calibrated_viewport(Vector2i(WALL_SIZE, WALL_SIZE), true)
	_add_calibrated_wall(sub)
	_add_ortho_wall_camera(sub)

	var area := AreaLight3D.new()
	area.position = Vector3(0.0, 0.0, LIGHT_Z)
	area.area_size = Vector2(0.8, 0.4)
	area.area_range = 10.0
	area.light_energy = 2.0
	area.shadow_enabled = false
	area.barn_door_angle = 90.0
	area.barn_door_length = 0.2
	sub.add_child(area)

	await _wait_frames(3)
	var img_open := sub.get_texture().get_image()

	# Angles >= 89.99 deg must match unclipped baseline.
	area.barn_door_angle = 89.995
	area.barn_door_length = 0.6
	await _wait_frames(3)
	var img_almost_open := sub.get_texture().get_image()
	var max_open_diff := 0.0
	for y in range(0, WALL_SIZE, 8):
		for x in range(0, WALL_SIZE, 8):
			max_open_diff = maxf(max_open_diff, absf(img_open.get_pixel(x, y).r - img_almost_open.get_pixel(x, y).r))
	_check(failures, max_open_diff < 1e-4, "Barn door >= 89.99 deg matches unclipped (diff=%.6f)" % max_open_diff)

	# Narrow barn doors: angle = 20 deg, length = 0.6 m -> at f = 1.0 m, max_x = 1.383 m.
	area.barn_door_angle = 20.0
	area.barn_door_length = 0.6
	await _wait_frames(3)
	var img_narrow := sub.get_texture().get_image()

	var center_px := _world_to_pixel(0.0, 0.0, WALL_SIZE)
	var out_pos_px := _world_to_pixel(1.75, 0.0, WALL_SIZE)
	var out_neg_px := _world_to_pixel(-1.75, 0.0, WALL_SIZE)

	var open_center := img_open.get_pixel(center_px.x, center_px.y).r
	var narrow_center := img_narrow.get_pixel(center_px.x, center_px.y).r
	var open_out := img_open.get_pixel(out_pos_px.x, out_pos_px.y).r
	var narrow_out_pos := img_narrow.get_pixel(out_pos_px.x, out_pos_px.y).r
	var narrow_out_neg := img_narrow.get_pixel(out_neg_px.x, out_neg_px.y).r

	_check(failures, open_center > 0.1, "Barn door open center lit (%.4f)" % open_center)
	_check(failures, narrow_center > open_center * 0.5, "Barn door narrow center remains lit (%.4f vs %.4f)" % [narrow_center, open_center])
	_check(failures, open_out > 0.03, "Barn door open outer wall lit (%.4f)" % open_out)
	_check(failures, narrow_out_pos < 0.002 and narrow_out_neg < 0.002, "Barn door narrow cuts off outside frustum (+x=%.6f, -x=%.6f)" % [narrow_out_pos, narrow_out_neg])

	sub.queue_free()
	await _wait_frames(2)
	return {
		"max_open_diff": snappedf(max_open_diff, 0.00001),
		"open_center": snappedf(open_center, 0.0001),
		"narrow_center": snappedf(narrow_center, 0.0001),
		"open_outside": snappedf(open_out, 0.0001),
		"narrow_outside_pos": snappedf(narrow_out_pos, 0.0001),
		"narrow_outside_neg": snappedf(narrow_out_neg, 0.0001),
	}


func _make_solid_texture(col: Color) -> ImageTexture:
	var img := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	img.fill(col)
	return ImageTexture.create_from_image(img)


func _sum_probes(probes: PackedFloat32Array) -> Vector3:
	var total := Vector3.ZERO
	var count := probes.size() / 3
	for i in range(count):
		total.x += probes[i * 3 + 0]
		total.y += probes[i * 3 + 1]
		total.z += probes[i * 3 + 2]
	return total


func _check_textured_rect(failures: PackedStringArray, require_nextgi: bool) -> Dictionary:
	var tex_red := _make_solid_texture(Color(1.0, 0.05, 0.05))
	var tex_blue := _make_solid_texture(Color(0.05, 0.05, 1.0))

	# 1) Forward+ textured AreaLight3D check.
	var sub := _make_calibrated_viewport(Vector2i(WALL_SIZE, WALL_SIZE), true)
	_add_calibrated_wall(sub)
	_add_ortho_wall_camera(sub)

	var area := AreaLight3D.new()
	area.position = Vector3(0.0, 0.0, LIGHT_Z)
	area.area_size = Vector2(1.0, 1.0)
	area.area_range = 10.0
	area.light_energy = 2.0
	area.shadow_enabled = false
	area.area_texture = tex_red
	sub.add_child(area)

	var center_px := _world_to_pixel(0.0, 0.0, WALL_SIZE)
	await _wait_frames(3)
	var fwd_red := sub.get_texture().get_image().get_pixel(center_px.x, center_px.y)

	area.area_texture = tex_blue
	await _wait_frames(3)
	var fwd_blue := sub.get_texture().get_image().get_pixel(center_px.x, center_px.y)

	_check(failures, fwd_red.r > fwd_red.b * 4.0 and fwd_red.r > 0.05, "Forward+ textured rect red dominates (r=%.4f b=%.4f)" % [fwd_red.r, fwd_red.b])
	_check(failures, fwd_blue.b > fwd_blue.r * 4.0 and fwd_blue.b > 0.05, "Forward+ textured rect blue dominates (r=%.4f b=%.4f)" % [fwd_blue.r, fwd_blue.b])

	sub.queue_free()
	await _wait_frames(2)

	var res := {
		"forward_red_rgb": [snappedf(fwd_red.r, 0.0001), snappedf(fwd_red.g, 0.0001), snappedf(fwd_red.b, 0.0001)],
		"forward_blue_rgb": [snappedf(fwd_blue.r, 0.0001), snappedf(fwd_blue.g, 0.0001), snappedf(fwd_blue.b, 0.0001)],
		"src_relight_checked": false,
	}

	# 2) NextGI SRC relight check (active when nextgi/enabled=true via run_light_source_parity_check.sh or override.cfg).
	var nextgi_on := bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false))
	if require_nextgi:
		_check(failures, nextgi_on, "NextGI must be enabled when --require-nextgi=1")
	if nextgi_on:
		get_tree().root.disable_3d = false
		var ng_root := Node3D.new()
		add_child(ng_root)

		var world := WorldEnvironment.new()
		var env := Environment.new()
		env.background_mode = Environment.BG_COLOR
		env.background_color = Color(0.0, 0.0, 0.0)
		env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		env.ambient_light_energy = 0.0
		env.nextgi_enabled = true
		world.environment = env
		ng_root.add_child(world)

		var floor_mi := MeshInstance3D.new()
		var floor_box := BoxMesh.new()
		floor_box.size = Vector3(8.0, 0.4, 8.0)
		var white_mat := StandardMaterial3D.new()
		white_mat.albedo_color = Color(0.9, 0.9, 0.9)
		white_mat.roughness = 0.9
		floor_box.material = white_mat
		floor_mi.mesh = floor_box
		floor_mi.position = Vector3(0.0, -0.2, 0.0)
		ng_root.add_child(floor_mi)

		var wall_mi := MeshInstance3D.new()
		var wall_box := BoxMesh.new()
		wall_box.size = Vector3(8.0, 4.0, 0.4)
		wall_box.material = white_mat
		wall_mi.mesh = wall_box
		wall_mi.position = Vector3(0.0, 2.0, -2.5)
		ng_root.add_child(wall_mi)

		var cam := Camera3D.new()
		cam.position = Vector3(0.0, 1.8, 3.5)
		cam.look_at_from_position(cam.position, Vector3(0.0, 0.8, -1.0), Vector3.UP)
		cam.current = true
		ng_root.add_child(cam)

		var src_area := AreaLight3D.new()
		src_area.position = Vector3(0.0, 2.0, -0.5)
		src_area.look_at_from_position(src_area.position, Vector3(0.0, 0.0, -2.0), Vector3.UP)
		src_area.area_size = Vector2(2.0, 2.0)
		src_area.area_range = 10.0
		src_area.light_energy = 12.0
		src_area.light_bake_mode = Light3D.BAKE_DYNAMIC
		src_area.shadow_enabled = false
		src_area.area_texture = tex_red
		ng_root.add_child(src_area)

		for _f in range(24):
			await get_tree().process_frame
		var info_red: Dictionary = RenderingServer.nextgi_get_tier_info()
		var probes_red: PackedFloat32Array = info_red.get("src_page_probes", PackedFloat32Array())
		var sum_red := _sum_probes(probes_red)

		src_area.area_texture = tex_blue
		for _f in range(24):
			await get_tree().process_frame
		var info_blue: Dictionary = RenderingServer.nextgi_get_tier_info()
		var probes_blue: PackedFloat32Array = info_blue.get("src_page_probes", PackedFloat32Array())
		var sum_blue := _sum_probes(probes_blue)

		res["src_relight_checked"] = true
		res["nextgi_tier"] = str(info_blue.get("tier", ""))
		res["src_relight_texels"] = int(info_blue.get("src_relight_texels", 0))
		res["src_red_sum"] = [snappedf(sum_red.x, 0.001), snappedf(sum_red.y, 0.001), snappedf(sum_red.z, 0.001)]
		res["src_blue_sum"] = [snappedf(sum_blue.x, 0.001), snappedf(sum_blue.y, 0.001), snappedf(sum_blue.z, 0.001)]

		_check(failures, int(info_blue.get("src_relight_texels", 0)) > 0, "NextGI SRC relit texels > 0")
		_check(failures, probes_red.size() > 0 and probes_blue.size() > 0, "NextGI SRC page probes readback populated")
		_check(failures, sum_red.x > sum_red.z * 2.5 and sum_red.x > 0.05, "SRC relight red area_texture dominates (r=%.4f b=%.4f)" % [sum_red.x, sum_red.z])
		_check(failures, sum_blue.z > sum_blue.x * 2.5 and sum_blue.z > 0.05, "SRC relight blue area_texture dominates (r=%.4f b=%.4f)" % [sum_blue.x, sum_blue.z])

		ng_root.queue_free()
		await _wait_frames(2)
		get_tree().root.disable_3d = true

	return res


func _mean_luma_rect(img: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	var total := 0.0
	var count := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			var c := img.get_pixel(x, y)
			total += (c.r + c.g + c.b) / 3.0
			count += 1
	return total / float(maxi(count, 1))


func _max_diff_rect(a: Image, b: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	var best := 0.0
	for y in range(y0, y1):
		for x in range(x0, x1):
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			best = maxf(best, absf(ca.r - cb.r))
			best = maxf(best, absf(ca.g - cb.g))
			best = maxf(best, absf(ca.b - cb.b))
	return best


func _check_contact_shadows(failures: PackedStringArray) -> Dictionary:
	var sub := _make_calibrated_viewport(Vector2i(256, 256), false)

	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 2.2, 2.2)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.0, 0.2), Vector3.UP)
	cam.fov = 50.0
	cam.current = true
	sub.add_child(cam)

	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.85, 0.85, 0.85)
	floor_mat.roughness = 0.95
	floor_mat.metallic = 0.0

	# Left floor panel: shadow-casting receiver (cast_shadow = ON).
	var left_floor := MeshInstance3D.new()
	var left_box := BoxMesh.new()
	left_box.size = Vector3(2.4, 0.1, 3.0)
	left_box.material = floor_mat
	left_floor.mesh = left_box
	left_floor.position = Vector3(-1.22, -0.05, 0.2)
	left_floor.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	sub.add_child(left_floor)

	# Right floor panel: non-shadow-casting receiver (cast_shadow = OFF).
	var right_floor := MeshInstance3D.new()
	var right_box := BoxMesh.new()
	right_box.size = Vector3(2.4, 0.1, 3.0)
	right_box.material = floor_mat
	right_floor.mesh = right_box
	right_floor.position = Vector3(1.22, -0.05, 0.2)
	right_floor.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	sub.add_child(right_floor)

	# Small occluder bar sitting directly on the floor spanning both panels.
	var bar := MeshInstance3D.new()
	var bar_box := BoxMesh.new()
	bar_box.size = Vector3(3.2, 0.24, 0.12)
	bar_box.material = floor_mat
	bar.mesh = bar_box
	bar.position = Vector3(0.0, 0.12, -0.05)
	bar.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	sub.add_child(bar)

	# Directional light tilted so its contact shadow falls in +Z behind the bar onto both panels.
	var sun := DirectionalLight3D.new()
	sun.look_at_from_position(Vector3(0.0, 2.0, -2.0), Vector3(0.0, 0.0, 0.0), Vector3.UP)
	sun.light_energy = 1.0
	sun.shadow_enabled = false
	sun.contact_shadow_length = 0.0
	sun.contact_shadow_casting_intensity = 1.0
	sun.contact_shadow_non_casting_intensity = 0.0
	sub.add_child(sun)

	var prev_proj_cs: bool = bool(ProjectSettings.get_setting("rendering/lights_and_shadows/contact_shadow/enabled", false))

	# 1) Byte-identity check: contact_shadow_length = 0.0 must be byte-identical to off even if project setting is on.
	ProjectSettings.set_setting("rendering/lights_and_shadows/contact_shadow/enabled", false)
	sun.contact_shadow_length = 0.0
	await _wait_frames(3)
	var img_off := sub.get_texture().get_image()

	ProjectSettings.set_setting("rendering/lights_and_shadows/contact_shadow/enabled", true)
	sun.contact_shadow_length = 0.0
	await _wait_frames(3)
	var img_zero := sub.get_texture().get_image()
	var zero_byte_identical := img_off.get_data() == img_zero.get_data()
	_check(failures, zero_byte_identical, "Contact shadow length=0.0 is byte-identical to off")

	# Locate the shadow band in screen space just in front (+Z) of the bar on left (x = -0.6) and right (x = +0.6).
	var left_pt := cam.unproject_position(Vector3(-0.6, 0.0, 0.12))
	var right_pt := cam.unproject_position(Vector3(0.6, 0.0, 0.12))
	var lx0 := clampi(int(left_pt.x) - 12, 0, 255)
	var lx1 := clampi(int(left_pt.x) + 12, 1, 256)
	var ly0 := clampi(int(left_pt.y) - 6, 0, 255)
	var ly1 := clampi(int(left_pt.y) + 10, 1, 256)
	var rx0 := clampi(int(right_pt.x) - 12, 0, 255)
	var rx1 := clampi(int(right_pt.x) + 12, 1, 256)
	var ry0 := clampi(int(right_pt.y) - 6, 0, 255)
	var ry1 := clampi(int(right_pt.y) + 10, 1, 256)

	var off_left := _mean_luma_rect(img_off, lx0, ly0, lx1, ly1)
	var off_right := _mean_luma_rect(img_off, rx0, ry0, rx1, ry1)

	# 2) Casting-only intensity (1.0 casting, 0.0 non-casting): only left panel darkens!
	ProjectSettings.set_setting("rendering/lights_and_shadows/contact_shadow/enabled", false)
	sun.contact_shadow_length = 0.15
	sun.contact_shadow_length_world_space = false
	sun.contact_shadow_casting_intensity = 1.0
	sun.contact_shadow_non_casting_intensity = 0.0
	await _wait_frames(3)
	var img_cast_only := sub.get_texture().get_image()
	var cast_drop_left := off_left - _mean_luma_rect(img_cast_only, lx0, ly0, lx1, ly1)
	var cast_diff_right := _max_diff_rect(img_off, img_cast_only, rx0, ry0, rx1, ry1)
	_check(failures, cast_drop_left > 0.08, "Contact shadow casting=1 darkens casting pixels (drop=%.4f)" % cast_drop_left)
	_check(failures, cast_diff_right < 0.005, "Contact shadow non_casting=0 leaves non-casting pixels untouched (diff=%.4f)" % cast_diff_right)

	# 3) Non-casting-only intensity (0.0 casting, 1.0 non-casting): only right panel darkens!
	sun.contact_shadow_casting_intensity = 0.0
	sun.contact_shadow_non_casting_intensity = 1.0
	await _wait_frames(3)
	var img_noncast_only := sub.get_texture().get_image()
	var noncast_diff_left := _max_diff_rect(img_off, img_noncast_only, lx0, ly0, lx1, ly1)
	var noncast_drop_right := off_right - _mean_luma_rect(img_noncast_only, rx0, ry0, rx1, ry1)
	_check(failures, noncast_diff_left < 0.005, "Contact shadow casting=0 leaves casting pixels untouched (diff=%.4f)" % noncast_diff_left)
	_check(failures, noncast_drop_right > 0.08, "Contact shadow non_casting=1 darkens non-casting pixels (drop=%.4f)" % noncast_drop_right)

	# 4) Local light (OmniLight3D) + world-space length contact shadow check.
	sun.visible = false
	var omni := OmniLight3D.new()
	omni.position = Vector3(0.0, 1.1, -1.4)
	omni.omni_range = 10.0
	omni.light_energy = 2.0
	omni.shadow_enabled = false
	omni.contact_shadow_length = 0.0
	omni.contact_shadow_length_world_space = true
	omni.contact_shadow_casting_intensity = 1.0
	omni.contact_shadow_non_casting_intensity = 0.0
	sub.add_child(omni)
	await _wait_frames(3)
	var img_omni_off := sub.get_texture().get_image()
	var omni_off_left := _mean_luma_rect(img_omni_off, lx0, ly0, lx1, ly1)

	omni.contact_shadow_length = 0.35
	await _wait_frames(3)
	var img_omni_on := sub.get_texture().get_image()
	var omni_drop_left := omni_off_left - _mean_luma_rect(img_omni_on, lx0, ly0, lx1, ly1)
	var omni_diff_right := _max_diff_rect(img_omni_off, img_omni_on, rx0, ry0, rx1, ry1)
	_check(failures, omni_drop_left > 0.05, "Local OmniLight3D world-space contact shadow darkens casting pixels (drop=%.4f)" % omni_drop_left)
	_check(failures, omni_diff_right < 0.005, "Local OmniLight3D non_casting=0 leaves non-casting pixels untouched (diff=%.4f)" % omni_diff_right)

	ProjectSettings.set_setting("rendering/lights_and_shadows/contact_shadow/enabled", prev_proj_cs)
	sub.queue_free()
	await _wait_frames(2)

	return {
		"zero_byte_identical": zero_byte_identical,
		"cast_only_drop_left": snappedf(cast_drop_left, 0.0001),
		"cast_only_diff_right": snappedf(cast_diff_right, 0.0001),
		"noncast_only_diff_left": snappedf(noncast_diff_left, 0.0001),
		"noncast_only_drop_right": snappedf(noncast_drop_right, 0.0001),
		"omni_ws_drop_left": snappedf(omni_drop_left, 0.0001),
		"omni_ws_diff_right": snappedf(omni_diff_right, 0.0001),
	}
