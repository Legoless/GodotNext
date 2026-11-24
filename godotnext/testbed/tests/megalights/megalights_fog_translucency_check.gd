extends Node3D
## #137: MegaLights fog, D17 translucency direct SH-L1, front-layer translucency, and SSS transmission GPU check
## (plan §21.3 contract M7, GM-6, budget M-8).
## Verifies end-to-end on GPU with Vulkan validation (VVL + SyncVal) enabled:
##  1. off_zero_alloc_identity: rendering/megalights/enabled = false allocates 0 froxel, D17-direct,
##     and front-layer MegaLights bytes (memory_bytes == 0) and produces byte-identical OFF frames.
##  2. volumetric_fog_shared_shadow_ies_lf: MegaLights shared froxel volume lights volumetric fog
##     with visible shadow shafts, IES profile attenuation, and Light Function Atlas modulation,
##     matching OFF reference luminance without double-lighting (volumetric_fog_process.glsl skips
##     LIGHT_FLAG_MEGALIGHTS lights).
##  3. translucency_d17_and_front_layer: transparent surfaces sample the D17 direct SH-L1 channel
##     in both shared_fog_d17 and d17_only modes (front_layer_memory_bytes == 0 when
##     front_layer_translucency = false); enabling front_layer_translucency = true allocates
##     separate front-layer MegaLights history (front_layer_memory_bytes > 0), delivers crisp
##     per-pixel shadows on the frontmost transparent layer while layers behind it fall back to
##     D17 direct SH-L1, and toggling off immediately frees all front-layer and MegaLights buffers.
##  4. sss_backface_transmission: backlit SSS (shading model 1 / transmission) surfaces accumulate
##     thickness-attenuated transmitted radiance (thin >> thick) via hit_distance_texture, while
##     backlit non-SSS opaque control surfaces remain strictly unlit (zero bias).

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
		"test": "megalights_fog_translucency",
		"pass": false,
	}

	var orig_enabled: bool = bool(ProjectSettings.get_setting("rendering/megalights/enabled"))
	var orig_front_layer: bool = bool(ProjectSettings.get_setting("rendering/megalights/front_layer_translucency"))
	var orig_spp: int = int(ProjectSettings.get_setting("rendering/megalights/samples_per_pixel"))
	var orig_ds: int = int(ProjectSettings.get_setting("rendering/megalights/downsample"))
	var orig_dir: bool = bool(ProjectSettings.get_setting("rendering/megalights/directional_lights"))

	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	ProjectSettings.set_setting("rendering/megalights/front_layer_translucency", false)
	ProjectSettings.set_setting("rendering/megalights/samples_per_pixel", 0)
	ProjectSettings.set_setting("rendering/megalights/downsample", 2)
	ProjectSettings.set_setting("rendering/megalights/directional_lights", false)

	report["off_zero_alloc_identity"] = await _check_off_zero_alloc_identity(failures)
	report["volumetric_fog_shared_shadow_ies_lf"] = await _check_volumetric_fog(failures)
	report["translucency_d17_and_front_layer"] = await _check_translucency(failures)
	report["sss_backface_transmission"] = await _check_sss_transmission(failures)

	ProjectSettings.set_setting("rendering/megalights/enabled", orig_enabled)
	ProjectSettings.set_setting("rendering/megalights/front_layer_translucency", orig_front_layer)
	ProjectSettings.set_setting("rendering/megalights/samples_per_pixel", orig_spp)
	ProjectSettings.set_setting("rendering/megalights/downsample", orig_ds)
	ProjectSettings.set_setting("rendering/megalights/directional_lights", orig_dir)

	report["failures"] = failures
	report["pass"] = failures.is_empty()
	print("MEGALIGHTS_FOG_TRANSLUCENCY_RESULT=" + JSON.stringify(report))
	get_tree().quit(0 if failures.is_empty() else 1)


func _frames(count: int) -> void:
	for _i in range(count):
		await RenderingServer.frame_post_draw


func _make_viewport(size: Vector2i, enable_fog: bool) -> Dictionary:
	var sub := SubViewport.new()
	sub.own_world_3d = true
	sub.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	sub.msaa_3d = Viewport.MSAA_DISABLED
	sub.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	sub.positional_shadow_atlas_size = 2048
	sub.size = size
	add_child(sub)

	var world := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_energy = 0.0
	env.nextgi_enabled = false
	env.volumetric_fog_enabled = enable_fog
	env.volumetric_fog_density = 0.25
	env.volumetric_fog_albedo = Color(1.0, 1.0, 1.0)
	env.volumetric_fog_emission = Color(0.0, 0.0, 0.0)
	env.volumetric_fog_emission_energy = 0.0
	env.volumetric_fog_anisotropy = 0.25
	env.volumetric_fog_length = 24.0
	env.volumetric_fog_detail_spread = 2.0
	env.volumetric_fog_ambient_inject = 0.0
	env.volumetric_fog_gi_inject = 0.0
	env.volumetric_fog_sky_affect = 1.0
	env.volumetric_fog_temporal_reprojection_enabled = false
	world.environment = env
	sub.add_child(world)
	return {"sub": sub, "env": env}


func _add_box(parent: Node, pos: Vector3, size: Vector3, mat: Material) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = size
	box.material = mat
	mi.mesh = box
	mi.position = pos
	mi.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	parent.add_child(mi)
	return mi


func _make_opaque_mat(albedo: Color, roughness: float = 0.65) -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = albedo
	mat.roughness = roughness
	mat.metallic = 0.0
	return mat


func _make_transparent_mat(albedo: Color, roughness: float = 0.65) -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.albedo_color = albedo
	mat.roughness = roughness
	mat.metallic = 0.0
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	return mat


func _make_sss_mat(albedo: Color) -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = albedo
	mat.roughness = 0.65
	mat.metallic = 0.0
	mat.subsurf_scatter_enabled = true
	mat.subsurf_scatter_strength = 1.0
	mat.subsurf_scatter_transmittance_enabled = true
	mat.subsurf_scatter_transmittance_color = Color(1.0, 0.45, 0.25)
	mat.subsurf_scatter_transmittance_depth = 0.25
	mat.subsurf_scatter_transmittance_boost = 0.2
	return mat


func _build_ies_profile(nadir_cd: float, side_cd: float) -> IESProfile:
	var text := "\n".join([
		"IESNA:LM-63-2002",
		"[TEST] MegaLights froxel IES profile (#137)",
		"TILT=NONE",
		"1 1000.0 1.0 4 1 1 2 0.0 0.0 0.0",
		"1.0 0.0 10.0",
		"0.0 50.0 90.0 180.0",
		"0.0",
		"%.2f %.2f %.2f %.2f" % [nadir_cd, nadir_cd, side_cd, side_cd],
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


func _roi_rgb_luma(img: Image, u0: float, v0: float, u1: float, v1: float) -> Dictionary:
	var w := img.get_width()
	var h := img.get_height()
	var x0 := clampi(int(u0 * float(w)), 0, w - 1)
	var x1 := clampi(int(u1 * float(w)), x0 + 1, w)
	var y0 := clampi(int(v0 * float(h)), 0, h - 1)
	var y1 := clampi(int(v1 * float(h)), y0 + 1, h)
	var sr := 0.0
	var sg := 0.0
	var sb := 0.0
	var sl := 0.0
	var slin := 0.0
	var n := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			var c := img.get_pixel(x, y)
			var clin := c.srgb_to_linear()
			sr += c.r
			sg += c.g
			sb += c.b
			sl += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
			slin += 0.2126 * clin.r + 0.7152 * clin.g + 0.0722 * clin.b
			n += 1
	var inv := 1.0 / float(maxi(n, 1))
	return {"r": sr * inv, "g": sg * inv, "b": sb * inv, "luma": sl * inv, "linear_luma": slin * inv}


func _check_off_zero_alloc_identity(failures: PackedStringArray) -> Dictionary:
	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	ProjectSettings.set_setting("rendering/megalights/front_layer_translucency", false)

	var vp := _make_viewport(VIEW_SIZE, true)
	var sub: SubViewport = vp["sub"]
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 4.5)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.0, -2.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	_add_box(sub, Vector3(0.0, 0.0, -4.0), Vector3(10.0, 10.0, 0.4), _make_opaque_mat(Color(0.5, 0.5, 0.5)))
	_add_box(sub, Vector3(-1.0, 0.0, 0.0), Vector3(2.2, 2.2, 0.06), _make_transparent_mat(Color(0.85, 0.85, 0.85, 0.45)))
	_add_box(sub, Vector3(1.5, 0.0, 0.0), Vector3(1.4, 2.0, 0.08), _make_sss_mat(Color(0.9, 0.9, 0.9)))

	var omni := OmniLight3D.new()
	omni.position = Vector3(0.0, 1.5, 1.5)
	omni.omni_range = 14.0
	omni.light_energy = 6.0
	omni.shadow_enabled = true
	sub.add_child(omni)

	await _frames(8)
	var info: Dictionary = RenderingServer.megalights_get_tier_info()
	var img_a: Image = sub.get_texture().get_image()
	await _frames(2)
	var img_b: Image = sub.get_texture().get_image()
	var identical: bool = img_a.get_data() == img_b.get_data()

	var mem_total: int = int(info.get("memory_bytes", -1))
	var mem_froxel: int = int(info.get("froxel_memory_bytes", -1))
	var mem_d17: int = int(info.get("d17_direct_memory_bytes", -1))
	var mem_fl: int = int(info.get("front_layer_memory_bytes", -1))
	var dispatch_mode: String = String(info.get("froxel_dispatch_mode", ""))

	_check(failures, mem_total == 0, "off_zero_alloc: memory_bytes=%d expected 0" % mem_total)
	_check(failures, mem_froxel == 0, "off_zero_alloc: froxel_memory_bytes=%d expected 0" % mem_froxel)
	_check(failures, mem_d17 == 0, "off_zero_alloc: d17_direct_memory_bytes=%d expected 0" % mem_d17)
	_check(failures, mem_fl == 0, "off_zero_alloc: front_layer_memory_bytes=%d expected 0" % mem_fl)
	_check(failures, dispatch_mode == "off", "off_zero_alloc: froxel_dispatch_mode='%s' expected 'off'" % dispatch_mode)
	_check(failures, identical, "off_zero_alloc: consecutive OFF frames not byte-identical")

	sub.queue_free()
	await _frames(2)
	return {
		"memory_bytes": mem_total,
		"froxel_memory_bytes": mem_froxel,
		"d17_direct_memory_bytes": mem_d17,
		"front_layer_memory_bytes": mem_fl,
		"froxel_dispatch_mode": dispatch_mode,
		"consecutive_identical": identical,
	}


func _check_volumetric_fog(failures: PackedStringArray) -> Dictionary:
	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	ProjectSettings.set_setting("rendering/megalights/front_layer_translucency", false)

	var vp := _make_viewport(VIEW_SIZE, true)
	var sub: SubViewport = vp["sub"]
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 4.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.0, -4.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	# Dark unlit backdrop behind the fog volume so pixels come purely from volumetric fog scattering.
	var dark_mat := _make_opaque_mat(Color(0.0, 0.0, 0.0), 1.0)
	dark_mat.metallic_specular = 0.0
	var wall := _add_box(sub, Vector3(0.0, 0.0, -14.0), Vector3(40.0, 40.0, 0.5), dark_mat)
	wall.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF

	# Black shadow-casting shelf blocking the left half of the fog volume below y = 1.8.
	_add_box(sub, Vector3(-2.6, 1.8, -2.0), Vector3(5.0, 0.3, 10.0), dark_mat)

	var omni := OmniLight3D.new()
	omni.position = Vector3(0.0, 3.2, -2.0)
	omni.rotation_degrees = Vector3(-90.0, 0.0, 0.0) # Local -Z points straight down (-Y) for IES nadir test.
	omni.omni_range = 16.0
	omni.light_energy = 3.0
	omni.light_volumetric_fog_energy = 1.5
	omni.shadow_enabled = true
	sub.add_child(omni)

	# 1. Reference OFF volumetric fog.
	await _frames(10)
	var ref_img: Image = sub.get_texture().get_image()
	var ref_lit: float = float(_roi_rgb_luma(ref_img, 0.62, 0.46, 0.88, 0.78)["linear_luma"])
	var ref_shadow: float = float(_roi_rgb_luma(ref_img, 0.12, 0.46, 0.38, 0.78)["linear_luma"])

	# 2. MegaLights ON shared froxel fog.
	ProjectSettings.set_setting("rendering/megalights/enabled", true)
	await _frames(16)
	var info: Dictionary = RenderingServer.megalights_get_tier_info()
	var ml_img: Image = sub.get_texture().get_image()
	var ml_lit: float = float(_roi_rgb_luma(ml_img, 0.62, 0.46, 0.88, 0.78)["linear_luma"])
	var ml_shadow: float = float(_roi_rgb_luma(ml_img, 0.12, 0.46, 0.38, 0.78)["linear_luma"])

	var mem_froxel: int = int(info.get("froxel_memory_bytes", 0))
	var cells: int = int(info.get("froxel_dispatched_cells", 0))
	var mode: String = String(info.get("froxel_dispatch_mode", ""))

	# 3. IES profile attenuation on froxel volume (dark nadir pointing downward into fog).
	omni.ies_profile = _build_ies_profile(50.0, 1000.0)
	# Briefly toggle enabled off->on to reset temporal froxel history cleanly for exact modulation check.
	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	await _frames(2)
	ProjectSettings.set_setting("rendering/megalights/enabled", true)
	await _frames(12)
	var ies_img: Image = sub.get_texture().get_image()
	var ml_ies_lit: float = float(_roi_rgb_luma(ies_img, 0.62, 0.46, 0.88, 0.78)["linear_luma"])

	# 4. Light Function Atlas modulation (0.25x) on froxel volume.
	omni.ies_profile = null
	omni.light_function = _make_light_function_material(0.25)
	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	await _frames(2)
	ProjectSettings.set_setting("rendering/megalights/enabled", true)
	await _frames(12)
	var lf_img: Image = sub.get_texture().get_image()
	var ml_lf_lit: float = float(_roi_rgb_luma(lf_img, 0.62, 0.46, 0.88, 0.78)["linear_luma"])

	var lf_ratio: float = ml_lf_lit / maxf(ml_lit, 1e-6)
	var ref_ratio: float = ml_lit / maxf(ref_lit, 1e-6)
	var shadow_contrast: float = ml_lit / maxf(ml_shadow, 1e-5)

	_check(failures, mem_froxel > 0, "fog: froxel_memory_bytes=%d expected > 0" % mem_froxel)
	_check(failures, cells > 0, "fog: froxel_dispatched_cells=%d expected > 0" % cells)
	_check(failures, mode == "fog_only" or mode == "shared_fog_d17",
			"fog: unexpected froxel_dispatch_mode='%s'" % mode)
	_check(failures, ref_lit > 0.02, "fog: OFF reference fog too dark (%.4f)" % ref_lit)
	_check(failures, ml_lit > 0.02, "fog: MegaLights fog in-scattering too dark (%.4f)" % ml_lit)
	_check(failures, shadow_contrast > 1.4,
			"fog: insufficient shadow shaft contrast (lit=%.4f shadow=%.4f ratio=%.2f)" % [ml_lit, ml_shadow, shadow_contrast])
	_check(failures, ref_ratio > 0.45 and ref_ratio < 1.85,
			"fog: MegaLights/OFF luminance ratio=%.3f outside [0.45, 1.85] (ml=%.4f ref=%.4f)" % [ref_ratio, ml_lit, ref_lit])
	_check(failures, ml_ies_lit < ml_lit * 0.65,
			"fog: IES profile did not attenuate froxel fog (base=%.4f ies=%.4f)" % [ml_lit, ml_ies_lit])
	_check(failures, lf_ratio > 0.12 and lf_ratio < 0.42,
			"fog: Light Function Atlas ratio=%.3f expected ~0.25 (base=%.4f lf=%.4f)" % [lf_ratio, ml_lit, ml_lf_lit])

	# Clean OFF restore.
	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	await _frames(3)
	var off_info: Dictionary = RenderingServer.megalights_get_tier_info()
	_check(failures, int(off_info.get("froxel_memory_bytes", -1)) == 0,
			"fog: froxel_memory_bytes did not free on OFF (%d)" % int(off_info.get("froxel_memory_bytes", -1)))

	sub.queue_free()
	await _frames(2)
	return {
		"froxel_memory_bytes": mem_froxel,
		"froxel_dispatched_cells": cells,
		"froxel_dispatch_mode": mode,
		"ref_lit": ref_lit,
		"ref_shadow": ref_shadow,
		"ml_lit": ml_lit,
		"ml_shadow": ml_shadow,
		"shadow_contrast": shadow_contrast,
		"ref_ratio": ref_ratio,
		"ml_ies_lit": ml_ies_lit,
		"ml_lf_lit": ml_lf_lit,
		"lf_ratio": lf_ratio,
	}


func _check_translucency(failures: PackedStringArray) -> Dictionary:
	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	ProjectSettings.set_setting("rendering/megalights/front_layer_translucency", false)

	var vp := _make_viewport(VIEW_SIZE, true)
	var sub: SubViewport = vp["sub"]
	var env: Environment = vp["env"]
	env.volumetric_fog_density = 0.02 # Subtle fog so shared_fog_d17 runs without washing out transparent quads.

	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 5.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.0, 0.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	# Front transparent quad (red-tinted frosted glass at z = 0.0) + Back transparent quad (blue-tinted at z = -2.2).
	_add_box(sub, Vector3(0.0, 0.0, 0.0), Vector3(7.0, 5.5, 0.05), _make_transparent_mat(Color(0.95, 0.06, 0.06, 0.45), 0.65))
	_add_box(sub, Vector3(0.0, 0.0, -2.2), Vector3(8.5, 6.5, 0.05), _make_transparent_mat(Color(0.06, 0.12, 0.95, 0.90), 0.65))

	# Opaque black shadow caster above camera middle ROI blocking left half (x < 0) of the front quad.
	_add_box(sub, Vector3(-1.7, 1.6, 0.5), Vector3(3.3, 0.18, 1.8), _make_opaque_mat(Color(0.0, 0.0, 0.0), 1.0))

	var omni := OmniLight3D.new()
	omni.position = Vector3(0.0, 4.5, 1.6)
	omni.omni_range = 18.0
	omni.light_energy = 14.0
	omni.shadow_enabled = true
	sub.add_child(omni)

	# Baseline OFF capture.
	await _frames(6)
	var off_before: Image = sub.get_texture().get_image()

	# 1. MegaLights ON + volumetric fog ON + front_layer_translucency = false -> shared_fog_d17 mode.
	ProjectSettings.set_setting("rendering/megalights/enabled", true)
	ProjectSettings.set_setting("rendering/megalights/front_layer_translucency", false)
	await _frames(14)
	var info_shared: Dictionary = RenderingServer.megalights_get_tier_info()
	var img_shared: Image = sub.get_texture().get_image()
	var shared_right: Dictionary = _roi_rgb_luma(img_shared, 0.62, 0.45, 0.88, 0.78)
	var shared_left: Dictionary = _roi_rgb_luma(img_shared, 0.12, 0.45, 0.38, 0.78)

	# 2. Volumetric fog OFF + front_layer_translucency = false -> d17_only mode.
	env.volumetric_fog_enabled = false
	await _frames(14)
	var info_d17_only: Dictionary = RenderingServer.megalights_get_tier_info()
	var img_d17_only: Image = sub.get_texture().get_image()
	var d17_right: Dictionary = _roi_rgb_luma(img_d17_only, 0.62, 0.45, 0.88, 0.78)
	var d17_left: Dictionary = _roi_rgb_luma(img_d17_only, 0.12, 0.45, 0.38, 0.78)

	# 3. front_layer_translucency = true -> separate per-pixel front-layer MegaLights + D17 fallback on back layer.
	ProjectSettings.set_setting("rendering/megalights/front_layer_translucency", true)
	await _frames(16)
	var info_fl: Dictionary = RenderingServer.megalights_get_tier_info()
	var img_fl: Image = sub.get_texture().get_image()
	var fl_right: Dictionary = _roi_rgb_luma(img_fl, 0.62, 0.45, 0.88, 0.78)
	var fl_left: Dictionary = _roi_rgb_luma(img_fl, 0.12, 0.45, 0.38, 0.78)

	# 4. Toggle front_layer_translucency = false -> immediate 0-byte free of front-layer buffers.
	ProjectSettings.set_setting("rendering/megalights/front_layer_translucency", false)
	await _frames(4)
	var info_fl_off: Dictionary = RenderingServer.megalights_get_tier_info()

	# 5. Toggle enabled = false + restore fog -> 0 bytes everywhere and byte-identical OFF restore.
	env.volumetric_fog_enabled = true
	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	await _frames(6)
	var info_off: Dictionary = RenderingServer.megalights_get_tier_info()
	var off_after: Image = sub.get_texture().get_image()
	var off_restored_identical: bool = off_before.get_data() == off_after.get_data()

	var shared_mode: String = String(info_shared.get("froxel_dispatch_mode", ""))
	var d17_only_mode: String = String(info_d17_only.get("froxel_dispatch_mode", ""))
	var d17_mem_shared: int = int(info_shared.get("d17_direct_memory_bytes", 0))
	var fl_mem_when_off: int = int(info_shared.get("front_layer_memory_bytes", -1))
	var fl_mem_when_on: int = int(info_fl.get("front_layer_memory_bytes", 0))
	var fl_mem_after_toggle: int = int(info_fl_off.get("front_layer_memory_bytes", -1))
	var d17_red_contrast: float = float(d17_right["r"]) / maxf(float(d17_left["r"]), 1e-5)
	var fl_red_contrast: float = float(fl_right["r"]) / maxf(float(fl_left["r"]), 1e-5)

	_check(failures, shared_mode == "shared_fog_d17",
			"translucency: expected froxel_dispatch_mode='shared_fog_d17', got '%s'" % shared_mode)
	_check(failures, d17_only_mode == "d17_only",
			"translucency: expected froxel_dispatch_mode='d17_only', got '%s'" % d17_only_mode)
	_check(failures, d17_mem_shared > 0, "translucency: d17_direct_memory_bytes=%d expected > 0" % d17_mem_shared)
	_check(failures, fl_mem_when_off == 0,
			"translucency: front_layer_memory_bytes=%d expected 0 when front_layer_translucency=false" % fl_mem_when_off)
	_check(failures, float(shared_right["luma"]) > 0.025,
			"translucency: shared_fog_d17 lit transparent quad too dark (%.4f)" % float(shared_right["luma"]))
	_check(failures, float(d17_right["luma"]) > 0.025,
			"translucency: d17_only lit transparent quad too dark (%.4f)" % float(d17_right["luma"]))
	_check(failures, float(d17_right["r"]) > float(d17_left["r"]) * 1.15,
			"translucency: D17 direct SH-L1 missing shadow contrast (lit=%.4f shadow=%.4f)" % [float(d17_right["r"]), float(d17_left["r"])])
	_check(failures, fl_mem_when_on > 0,
			"translucency: front_layer_memory_bytes=%d expected > 0 when front_layer_translucency=true" % fl_mem_when_on)
	_check(failures, float(fl_right["r"]) > 0.03,
			"translucency: front-layer red quad too dark (%.4f)" % float(fl_right["r"]))
	_check(failures, fl_red_contrast > 1.5,
			"translucency: front-layer shadow contrast too weak (lit=%.4f shadow=%.4f ratio=%.2f)" % [float(fl_right["r"]), float(fl_left["r"]), fl_red_contrast])
	_check(failures, float(fl_right["b"]) > 0.015,
			"translucency: background blue layer behind front layer not lit via D17 fallback (b=%.4f)" % float(fl_right["b"]))
	_check(failures, fl_mem_after_toggle == 0,
			"translucency: front_layer_memory_bytes=%d did not free immediately on toggle off" % fl_mem_after_toggle)
	_check(failures, int(info_off.get("memory_bytes", -1)) == 0,
			"translucency: memory_bytes=%d not 0 after OFF" % int(info_off.get("memory_bytes", -1)))
	_check(failures, int(info_off.get("d17_direct_memory_bytes", -1)) == 0,
			"translucency: d17_direct_memory_bytes=%d not 0 after OFF" % int(info_off.get("d17_direct_memory_bytes", -1)))
	_check(failures, off_restored_identical,
			"translucency: OFF frame after toggle cycle not byte-identical to initial OFF baseline")

	sub.queue_free()
	await _frames(2)
	return {
		"shared_mode": shared_mode,
		"d17_only_mode": d17_only_mode,
		"d17_mem_shared": d17_mem_shared,
		"fl_mem_when_off": fl_mem_when_off,
		"fl_mem_when_on": fl_mem_when_on,
		"fl_mem_after_toggle": fl_mem_after_toggle,
		"shared_right_luma": float(shared_right["luma"]),
		"shared_left_luma": float(shared_left["luma"]),
		"d17_right_r": float(d17_right["r"]),
		"d17_left_r": float(d17_left["r"]),
		"d17_red_contrast": d17_red_contrast,
		"fl_right_r": float(fl_right["r"]),
		"fl_left_r": float(fl_left["r"]),
		"fl_right_b": float(fl_right["b"]),
		"fl_red_contrast": fl_red_contrast,
		"off_restored_identical": off_restored_identical,
	}


func _check_sss_transmission(failures: PackedStringArray) -> Dictionary:
	ProjectSettings.set_setting("rendering/megalights/enabled", true)
	ProjectSettings.set_setting("rendering/megalights/front_layer_translucency", false)

	var vp := _make_viewport(VIEW_SIZE, false)
	var sub: SubViewport = vp["sub"]
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 4.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.0, 0.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	# Three side-by-side slabs whose front faces all lie at z = +0.02 facing the camera (+Z),
	# strictly backlit by a shadowed MegaLights light behind them at z = -3.2.
	# Left (x = -1.75): thin SSS slab (thickness 0.04m -> high Beer-Lambert transmission).
	_add_box(sub, Vector3(-1.75, 0.0, 0.0), Vector3(1.3, 2.8, 0.04), _make_sss_mat(Color(0.9, 0.9, 0.9)))
	# Center (x = 0.0): non-SSS opaque control slab (thickness 0.04m -> strictly 0 transmission).
	_add_box(sub, Vector3(0.0, 0.0, 0.0), Vector3(1.3, 2.8, 0.04), _make_opaque_mat(Color(0.9, 0.9, 0.9), 0.65))
	# Right (x = +1.75): thick SSS slab (thickness 0.80m, center z = -0.38 so front face is also z = +0.02).
	_add_box(sub, Vector3(1.75, 0.0, -0.38), Vector3(1.3, 2.8, 0.80), _make_sss_mat(Color(0.9, 0.9, 0.9)))

	var omni := OmniLight3D.new()
	omni.position = Vector3(0.0, 0.0, -3.2)
	omni.omni_range = 16.0
	omni.light_energy = 4.0
	omni.shadow_enabled = true
	sub.add_child(omni)

	await _frames(16)
	var img: Image = sub.get_texture().get_image()

	var thin_stats: Dictionary = _roi_rgb_luma(img, 0.14, 0.32, 0.30, 0.68)
	var ctrl_stats: Dictionary = _roi_rgb_luma(img, 0.42, 0.32, 0.58, 0.68)
	var thick_stats: Dictionary = _roi_rgb_luma(img, 0.70, 0.32, 0.86, 0.68)

	var thin_luma: float = float(thin_stats["luma"])
	var ctrl_luma: float = float(ctrl_stats["luma"])
	var thick_luma: float = float(thick_stats["luma"])
	var thickness_ratio: float = thin_luma / maxf(thick_luma, 1e-5)

	_check(failures, thin_luma > 0.02,
			"sss_transmission: backlit thin SSS slab too dark (%.4f)" % thin_luma)
	_check(failures, thin_luma > thick_luma * 2.0,
			"sss_transmission: thin SSS (%.4f) did not transmit >= 2x more than thick SSS (%.4f)" % [thin_luma, thick_luma])
	_check(failures, ctrl_luma < 0.005,
			"sss_transmission: backlit non-SSS opaque control slab leaked light (%.4f)" % ctrl_luma)

	ProjectSettings.set_setting("rendering/megalights/enabled", false)
	await _frames(3)
	sub.queue_free()
	await _frames(2)
	return {
		"thin_luma": thin_luma,
		"thick_luma": thick_luma,
		"control_luma": ctrl_luma,
		"thickness_ratio": thickness_ratio,
	}
