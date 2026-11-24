extends Node3D

## Virtual Shadow Maps in volumetric fog and forward translucency (#215; plan §21.3 contract S6, GV-7).
##
## Verifies all five requirements end-to-end on the GPU:
## 1. Volumetric fog shadows (directional, spot, omni, area): a thick uniform fog volume behind a
##    half-blocking occluder must show clear shadow contrast between the lit and shadowed halves
##    compared to the unshadowed reference, with atlas_passes == 0 (the #222 fog exception is gone)
##    and missing_coarse_pages == 0.
## 2. Forward translucent shadows (directional, spot, omni, area) at both translucent_quality = 0
##    (Coarse, default) and translucent_quality = 1 (Full SMRT): a transparent receiver quad behind an
##    occluder slab must show clear shadow contrast on both settings, with atlas_passes == 0 and
##    missing_coarse_pages == 0.
## 3. Fog Hall (res://scenes/fog_hall/fog_hall.tscn): renders non-black with volumetric fog and
##    transparent glass windows, atlas_passes == 0, lights_sampled > 0, and missing_coarse_pages == 0.
## 4. VSM Flythrough (res://scenes/vsm_flythrough/vsm_flythrough.tscn): runs across frames with
##    moving camera/actor and verifies atlas_passes == 0 and missing_coarse_pages == 0.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.
## User arguments:
##   --shadows=atlas|vsm (default: vsm when GODOT_DEV_VSM is set)
##   --flythrough-frames=N (default: 30)

const SceneArgs = preload("res://scenes/scene_args.gd")

const SIZE := 320
const SETTLE_FRAMES := 8
const LIGHT_TYPES: Array[String] = ["sun", "spot", "omni", "area"]

var _mode := "vsm"
var _flythrough_frames := 30
var _failures := PackedStringArray()
var _result := {}


func _ready() -> void:
	_mode = SceneArgs.value("--shadows", "vsm" if OS.get_environment("GODOT_DEV_VSM") != "" else "atlas")
	_flythrough_frames = maxi(SceneArgs.int_value("--flythrough-frames", 30), 6)
	_result["shadows"] = _mode
	_result["driver"] = RenderingServer.get_current_rendering_driver_name()
	_result["device"] = RenderingServer.get_video_adapter_name()
	_run.call_deferred()


func _frames(p_count: int) -> void:
	for i in p_count:
		await RenderingServer.frame_post_draw


func _region_luma(p_image: Image, p_u0: float, p_v0: float, p_u1: float, p_v1: float) -> float:
	var w := p_image.get_width()
	var h := p_image.get_height()
	var x0 := clampi(int(p_u0 * float(w)), 0, w - 1)
	var x1 := clampi(int(p_u1 * float(w)), x0 + 1, w)
	var y0 := clampi(int(p_v0 * float(h)), 0, h - 1)
	var y1 := clampi(int(p_v1 * float(h)), y0 + 1, h)
	var total := 0.0
	var count := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			var c := p_image.get_pixel(x, y)
			total += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
			count += 1
	return total / float(maxi(count, 1))


func _new_viewport(p_size: Vector2i, p_env: Environment, p_own_world: bool = true) -> SubViewport:
	var vp := SubViewport.new()
	vp.size = p_size
	vp.use_hdr_2d = true
	vp.own_world_3d = p_own_world
	vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	vp.use_taa = false
	add_child(vp)
	if p_env != null:
		var we := WorldEnvironment.new()
		we.environment = p_env
		vp.add_child(we)
	return vp


func _new_test_light(p_kind: String, p_energy: float, p_fog_energy: float) -> Light3D:
	var light: Light3D
	match p_kind:
		"sun":
			var sun := DirectionalLight3D.new()
			sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
			sun.directional_shadow_max_distance = 50.0
			sun.light_angular_distance = 1.0
			light = sun
		"spot":
			var spot := SpotLight3D.new()
			spot.spot_angle = 55.0
			spot.spot_range = 30.0
			spot.light_size = 0.05
			light = spot
		"omni":
			var omni := OmniLight3D.new()
			omni.omni_range = 30.0
			omni.light_size = 0.05
			light = omni
		_:
			var area := AreaLight3D.new()
			area.area_range = 30.0
			area.area_size = Vector2(0.4, 0.4)
			light = area
	light.light_energy = p_energy
	light.light_volumetric_fog_energy = p_fog_energy
	light.shadow_enabled = true
	return light


func _check_vsm_projection(p_label: String, p_info: Dictionary, p_expect_translucent_quality: int = -1) -> Dictionary:
	var proj: Dictionary = p_info.get("projection", {})
	var rec := {
		"atlas_passes": int(proj.get("atlas_passes", -1)),
		"atlas_passes_skipped": int(proj.get("atlas_passes_skipped", -1)),
		"lights_sampled": int(proj.get("lights_sampled", -1)),
		"missing_coarse_pages": int(proj.get("missing_coarse_pages", -1)),
		"missing_coarse_pages_fog": int(proj.get("missing_coarse_pages_fog", -1)),
		"missing_coarse_pages_translucent": int(proj.get("missing_coarse_pages_translucent", -1)),
		"view_missing_coarse_pages": int(proj.get("view_missing_coarse_pages", -1)),
		"translucent_quality": int(proj.get("translucent_quality", -1)),
	}
	if _mode == "vsm":
		if not bool(p_info.get("active", false)):
			_failures.append("%s: VSM not active" % p_label)
		if rec.lights_sampled <= 0:
			_failures.append("%s: expected lights_sampled > 0, got %d" % [p_label, rec.lights_sampled])
		if rec.atlas_passes != 0:
			_failures.append("%s: expected atlas_passes == 0, got %d" % [p_label, rec.atlas_passes])
		if rec.missing_coarse_pages != 0:
			_failures.append("%s: expected missing_coarse_pages == 0, got %d (fog=%d translucent=%d)" % [p_label, rec.missing_coarse_pages, rec.missing_coarse_pages_fog, rec.missing_coarse_pages_translucent])
		if rec.view_missing_coarse_pages != 0:
			_failures.append("%s: expected view_missing_coarse_pages == 0, got %d" % [p_label, rec.view_missing_coarse_pages])
		if p_expect_translucent_quality >= 0 and rec.translucent_quality != p_expect_translucent_quality:
			_failures.append("%s: expected translucent_quality == %d, got %d" % [p_label, p_expect_translucent_quality, rec.translucent_quality])
	return rec


func _run_volumetric_fog_cases() -> Array:
	var records := []
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_energy = 0.0
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.volumetric_fog_enabled = true
	env.volumetric_fog_density = 0.25
	env.volumetric_fog_albedo = Color(1.0, 1.0, 1.0)
	env.volumetric_fog_length = 24.0
	env.volumetric_fog_temporal_reprojection_enabled = false

	var vp := _new_viewport(Vector2i(SIZE, SIZE), env)
	var cam := Camera3D.new()
	cam.fov = 60.0
	cam.near = 0.1
	cam.far = 40.0
	cam.transform = Transform3D(Basis.IDENTITY, Vector3(0.0, 0.0, 8.0))
	vp.add_child(cam)
	cam.current = true

	# Dark unlit backdrop behind the fog volume so pixels come purely from volumetric fog scattering.
	var dark_mat := StandardMaterial3D.new()
	dark_mat.albedo_color = Color(0.0, 0.0, 0.0)
	dark_mat.metallic_specular = 0.0
	var wall := MeshInstance3D.new()
	var wall_mesh := BoxMesh.new()
	wall_mesh.size = Vector3(40.0, 40.0, 0.5)
	wall.mesh = wall_mesh
	wall.material_override = dark_mat
	wall.position = Vector3(0.0, 0.0, -14.0)
	wall.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	vp.add_child(wall)

	# Large occluder placed above the left half (x < 0) of the view, outside the camera frustum so
	# only its shadow falls across the left half of the fog volume while the right half (x > 0) stays lit.
	var occluder := MeshInstance3D.new()
	var occ_mesh := BoxMesh.new()
	occ_mesh.size = Vector3(12.0, 0.5, 22.0)
	occluder.mesh = occ_mesh
	occluder.material_override = dark_mat
	occluder.position = Vector3(-6.0, 5.5, -2.0)
	vp.add_child(occluder)

	for kind: String in LIGHT_TYPES:
		var energy := 4.0 if kind == "sun" else (60.0 if kind == "area" else 40.0)
		var light := _new_test_light(kind, energy, 2.0)
		vp.add_child(light)
		if kind == "sun":
			light.transform = Transform3D(Basis.looking_at(Vector3(0.0, -1.0, 0.0), Vector3(0.0, 0.0, -1.0)), Vector3(0.0, 12.0, 0.0))
		else:
			light.look_at_from_position(Vector3(0.0, 10.0, -1.0), Vector3(0.0, 0.0, -1.0), Vector3(0.0, 0.0, -1.0))

		# Unshadowed reference: occluder casts no shadow.
		occluder.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		await _frames(SETTLE_FRAMES)
		var ref_img := vp.get_texture().get_image()
		var ref_left := _region_luma(ref_img, 0.12, 0.35, 0.38, 0.65)
		var ref_right := _region_luma(ref_img, 0.62, 0.35, 0.88, 0.65)

		# Shadowed fog: occluder blocks the left half of the fog volume.
		occluder.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		await _frames(SETTLE_FRAMES)
		var sh_img := vp.get_texture().get_image()
		var sh_left := _region_luma(sh_img, 0.12, 0.35, 0.38, 0.65)
		var sh_right := _region_luma(sh_img, 0.62, 0.35, 0.88, 0.65)
		var info := RenderingServer.virtual_shadow_maps_get_debug_info()
		var vsm_rec := _check_vsm_projection("fog_" + kind, info)

		var ratio: float = sh_left / maxf(sh_right, 1e-6)
		var ref_ratio: float = ref_left / maxf(ref_right, 1e-6)
		if ref_right < 0.02:
			_failures.append("fog_%s: fog reference too dark (ref_right=%.4f)" % [kind, ref_right])
		if sh_right < 0.02:
			_failures.append("fog_%s: lit fog side too dark (sh_right=%.4f)" % [kind, sh_right])
		var max_ratio: float = 0.15 if _mode == "vsm" else 0.65
		if ratio > max_ratio or ratio >= ref_ratio * 0.7:
			_failures.append("fog_%s: expected shadow contrast in fog (sh_left=%.4f sh_right=%.4f ratio=%.3f ref_ratio=%.3f)" % [kind, sh_left, sh_right, ratio, ref_ratio])
		records.append({
			"light": kind,
			"ref_left": ref_left,
			"ref_right": ref_right,
			"shadowed_left": sh_left,
			"shadowed_right": sh_right,
			"ratio": ratio,
			"vsm": vsm_rec,
		})
		vp.remove_child(light)
		light.queue_free()

	remove_child(vp)
	vp.queue_free()
	await _frames(2)
	return records


func _run_translucent_cases() -> Array:
	var records := []
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_energy = 0.0
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR

	var vp := _new_viewport(Vector2i(SIZE, SIZE), env)
	var cam := Camera3D.new()
	cam.projection = Camera3D.PROJECTION_ORTHOGONAL
	cam.size = 6.0
	cam.near = 0.1
	cam.far = 30.0
	cam.transform = Transform3D(Basis.looking_at(Vector3.DOWN, Vector3.FORWARD), Vector3(0.0, 6.0, 0.0))
	vp.add_child(cam)
	cam.current = true

	# Forward translucent receiver quad (in RENDER_LIST_ALPHA, no opaque surface behind it).
	var trans_mat := StandardMaterial3D.new()
	trans_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	trans_mat.albedo_color = Color(1.0, 1.0, 1.0, 0.8)
	trans_mat.roughness = 1.0
	trans_mat.metallic = 0.0
	trans_mat.metallic_specular = 0.0
	var receiver := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(10.0, 10.0)
	receiver.mesh = plane
	receiver.material_override = trans_mat
	receiver.position = Vector3(0.0, 0.0, 0.0)
	receiver.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	vp.add_child(receiver)

	# Opaque occluder slab covering the left half (x < 0) of the transparent receiver, positioned
	# off to the side along the light ray so the camera sees the transparent receiver directly.
	var occ_mat := StandardMaterial3D.new()
	occ_mat.albedo_color = Color(0.0, 0.0, 0.0)
	occ_mat.metallic_specular = 0.0
	var occluder := MeshInstance3D.new()
	var occ_mesh := BoxMesh.new()
	occ_mesh.size = Vector3(3.0, 0.1, 8.0)
	occluder.mesh = occ_mesh
	occluder.material_override = occ_mat
	occluder.position = Vector3(-4.5, 2.0, 0.0)
	vp.add_child(occluder)

	var prev_tq := int(ProjectSettings.get_setting("rendering/lights_and_shadows/virtual_shadows/translucent_quality", 0))
	for tq in [0, 1]:
		ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/translucent_quality", tq)
		for kind: String in LIGHT_TYPES:
			var energy := 2.0 if kind == "sun" else (35.0 if kind == "area" else 25.0)
			var light := _new_test_light(kind, energy, 0.0)
			vp.add_child(light)
			# Light comes from (-x, +y) at 45 degrees so the slab at x=-4.5, y=2 shadows x in [-4.0, -1.0]
			# on the transparent receiver (left side of viewport), while x in [1.0, 2.5] (right side) is lit.
			if kind == "sun":
				light.transform = Transform3D(Basis.looking_at(Vector3(1.0, -0.7, 0.0).normalized(), Vector3.UP), Vector3(-6.0, 6.0, 0.0))
			else:
				light.look_at_from_position(Vector3(-8.5, 4.8, 0.0), Vector3(0.0, 0.0, 0.0), Vector3.UP)

			occluder.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			await _frames(SETTLE_FRAMES)
			var ref_img := vp.get_texture().get_image()
			var ref_left := _region_luma(ref_img, 0.15, 0.3, 0.35, 0.7)
			var ref_right := _region_luma(ref_img, 0.65, 0.3, 0.85, 0.7)

			occluder.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
			await _frames(SETTLE_FRAMES)
			var sh_img := vp.get_texture().get_image()
			var sh_left := _region_luma(sh_img, 0.15, 0.3, 0.35, 0.7)
			var sh_right := _region_luma(sh_img, 0.65, 0.3, 0.85, 0.7)
			var info := RenderingServer.virtual_shadow_maps_get_debug_info()
			var vsm_rec := _check_vsm_projection("translucent_q%d_%s" % [tq, kind], info, tq)

			var drop_left: float = sh_left / maxf(ref_left, 1e-6)
			var keep_right: float = sh_right / maxf(ref_right, 1e-6)
			if ref_left < 0.02 or ref_right < 0.02:
				_failures.append("translucent_q%d_%s: reference too dark (left=%.4f right=%.4f)" % [tq, kind, ref_left, ref_right])
			if drop_left > 0.35:
				_failures.append("translucent_q%d_%s: expected shadowed left side on transparent receiver (drop_left=%.3f sh=%.4f ref=%.4f)" % [tq, kind, drop_left, sh_left, ref_left])
			if keep_right < 0.80:
				_failures.append("translucent_q%d_%s: lit right side unexpectedly darkened (keep_right=%.3f sh=%.4f ref=%.4f)" % [tq, kind, keep_right, sh_right, ref_right])
			records.append({
				"translucent_quality": tq,
				"light": kind,
				"ref_left": ref_left,
				"ref_right": ref_right,
				"shadowed_left": sh_left,
				"shadowed_right": sh_right,
				"drop_left": drop_left,
				"keep_right": keep_right,
				"vsm": vsm_rec,
			})
			vp.remove_child(light)
			light.queue_free()

	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/translucent_quality", prev_tq)
	remove_child(vp)
	vp.queue_free()
	await _frames(2)
	return records


func _run_scene_check(p_label: String, p_scene_path: String, p_frames: int) -> Dictionary:
	var packed := load(p_scene_path) as PackedScene
	if packed == null:
		_failures.append("%s: failed to load %s" % [p_label, p_scene_path])
		return {"loaded": false}
	var vp := _new_viewport(Vector2i(640, 360), null, true)
	var instance := packed.instantiate()
	if instance.has_method("set_meta"):
		instance.set_meta("skip_runtime", true)
	vp.add_child(instance)
	var cam := instance.find_child("Camera*", true, false) as Camera3D
	var cam_path := instance.find_child("CameraPath*", true, false) as Path3D
	var cam_follow: PathFollow3D = null
	if cam == null:
		cam = Camera3D.new()
		cam.near = 0.1
		cam.far = 300.0
		if cam_path != null:
			cam_follow = PathFollow3D.new()
			cam_path.add_child(cam_follow)
			cam_follow.add_child(cam)
		else:
			cam.transform = Transform3D(Basis.IDENTITY, Vector3(0.0, 3.0, 8.0))
			vp.add_child(cam)
	cam.current = true

	var max_missing := 0
	var max_atlas := 0
	var last_info := {}
	for f in p_frames:
		if cam_follow != null:
			cam_follow.progress_ratio = float(f) / float(maxi(p_frames - 1, 1))
		await RenderingServer.frame_post_draw
		if f >= 4:
			last_info = RenderingServer.virtual_shadow_maps_get_debug_info()
			var proj: Dictionary = last_info.get("projection", {})
			max_missing = maxi(max_missing, int(proj.get("missing_coarse_pages", 0)))
			max_atlas = maxi(max_atlas, int(proj.get("atlas_passes", 0)))

	var img := vp.get_texture().get_image()
	var mean_luma := _region_luma(img, 0.05, 0.05, 0.95, 0.95)
	if mean_luma < 0.002:
		_failures.append("%s: rendered frame is black (mean_luma=%.5f)" % [p_label, mean_luma])
	var vsm_rec := _check_vsm_projection(p_label, last_info)
	if _mode == "vsm" and max_missing != 0:
		_failures.append("%s: missing_coarse_pages peaked at %d" % [p_label, max_missing])
	if _mode == "vsm" and max_atlas != 0:
		_failures.append("%s: atlas_passes peaked at %d after warm-up" % [p_label, max_atlas])

	remove_child(vp)
	vp.queue_free()
	await _frames(2)
	return {
		"loaded": true,
		"frames": p_frames,
		"mean_luma": mean_luma,
		"max_missing_coarse_pages": max_missing,
		"max_atlas_passes": max_atlas,
		"vsm": vsm_rec,
	}


func _run() -> void:
	await _frames(3)
	_result["volumetric_fog"] = await _run_volumetric_fog_cases()
	_result["translucent"] = await _run_translucent_cases()
	_result["fog_hall"] = await _run_scene_check("fog_hall", "res://scenes/fog_hall/fog_hall.tscn", 16)
	_result["vsm_flythrough"] = await _run_scene_check("vsm_flythrough", "res://scenes/vsm_flythrough/vsm_flythrough.tscn", _flythrough_frames)
	_result["passed"] = _failures.is_empty()
	_result["failures"] = Array(_failures)
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
