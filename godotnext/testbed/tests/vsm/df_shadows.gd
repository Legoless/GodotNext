extends Node3D

## Distance-field shadows GPU verification (#214; plan §21.3 contract S7, GV-10).
##
## Verifies on Forward+ with VSM active (GODOT_DEV_VSM=1):
## 1. Directional DF far-shadow handoff:
##    - Near caster inside distance_field_shadow_start casts a VSM shadow.
##    - Mid caster spanning the 10% blend band [0.9 * start, start] casts a continuous shadow across
##      the handoff (reports blend-band luminance/gradient diagnostics cleanly).
##    - Far non-Nanite caster beyond distance_field_shadow_start is dropped from directional VSM
##      maps (gpu_df_far_culled > 0, cpu/gpu raster check passes) while still casting a visible DF
##      shadow traced through the PS global SDF.
## 2. Per-light Light3D.SHADOW_METHOD_DISTANCE_FIELD on local lights (OmniLight3D and SpotLight3D):
##    - Allocates 0 VSM maps and runs 0 shadow-atlas passes.
##    - Casts visible shadows with 2 m mesh-SDF detail refinement enabled.
##    - Increasing light_size widens the soft penumbra band monotonically.
## 3. Tier fallback when PS mesh SDFs are disabled (mesh_sdf/generate = false):
##    - virtual_shadow_maps_get_tier_info() reports df_available == false and df_reason == MESH_SDF_DISABLED.
##    - df_shadows.lights_traced drops to 0, VSM continues to full directional shadow distance
##      (gpu_df_far_culled == 0), and the far caster still casts a VSM shadow.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const MESH_SDF_SETTING := "rendering/global_illumination/mesh_sdf/generate"
const HANDOFF_START := 24.0
const HANDOFF_DIST := 120.0
const SDF_WAIT_MAX := 180
const SETTLE_FRAMES := 8

var _saved_mesh_sdf: Variant = false
var _camera: Camera3D
var _sun: DirectionalLight3D
var _omni: OmniLight3D
var _spot: SpotLight3D
var _far_caster: MeshInstance3D
var _near_caster: MeshInstance3D
var _blend_caster: MeshInstance3D
var _local_caster: MeshInstance3D

var _stage := "wait_sdf"
var _stage_frame := 0
var _frame := 0
var _failures: Array[String] = []
var _result: Dictionary = {}
var _raster_armed := false
var _narrow_penumbra_samples: Array[float] = []


func _ready() -> void:
	_saved_mesh_sdf = ProjectSettings.get_setting(MESH_SDF_SETTING, false)
	ProjectSettings.set_setting(MESH_SDF_SETTING, true)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.05, 0.06, 0.08)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.06, 0.06, 0.06)
	env.ambient_light_energy = 0.2
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.nextgi_enabled = false
	env.sdfgi_enabled = false
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.85, 0.85, 0.85)
	floor_mat.roughness = 0.95
	floor_mat.metallic = 0.0

	var caster_mat := StandardMaterial3D.new()
	caster_mat.albedo_color = Color(0.5, 0.45, 0.4)
	caster_mat.roughness = 0.9

	var floor_mesh := BoxMesh.new()
	floor_mesh.size = Vector3(180.0, 1.0, 180.0)
	var floor_inst := MeshInstance3D.new()
	floor_inst.name = "Ground"
	floor_inst.mesh = floor_mesh
	floor_inst.material_override = floor_mat
	floor_inst.position = Vector3(0.0, -0.5, -45.0)
	add_child(floor_inst)

	# Directional handoff casters. Place NearCaster near x = 0 so its +X shadow is unobstructed from the camera
	# and clear of BlendCaster on -X.
	var near_mesh := BoxMesh.new()
	near_mesh.size = Vector3(2.0, 4.0, 2.0)
	_near_caster = MeshInstance3D.new()
	_near_caster.name = "NearCaster"
	_near_caster.mesh = near_mesh
	_near_caster.material_override = caster_mat
	_near_caster.position = Vector3(0.5, 2.0, 4.0)
	add_child(_near_caster)

	# Long wall spanning across the [21.6 m, 24.0 m] handoff band from camera (0, 14, 18).
	var blend_mesh := BoxMesh.new()
	blend_mesh.size = Vector3(2.0, 4.0, 14.0)
	_blend_caster = MeshInstance3D.new()
	_blend_caster.name = "BlendCaster"
	_blend_caster.mesh = blend_mesh
	_blend_caster.material_override = caster_mat
	_blend_caster.position = Vector3(-5.0, 2.0, -2.0)
	add_child(_blend_caster)

	var far_mesh := BoxMesh.new()
	far_mesh.size = Vector3(4.0, 6.0, 4.0)
	_far_caster = MeshInstance3D.new()
	_far_caster.name = "FarCaster"
	_far_caster.mesh = far_mesh
	_far_caster.material_override = caster_mat
	_far_caster.position = Vector3(-4.0, 3.0, -32.0)
	add_child(_far_caster)

	# Local-light caster in an isolated area (x = 40) so directional casters never interfere.
	var local_mesh := BoxMesh.new()
	local_mesh.size = Vector3(1.6, 3.2, 1.6)
	_local_caster = MeshInstance3D.new()
	_local_caster.name = "LocalCaster"
	_local_caster.mesh = local_mesh
	_local_caster.material_override = caster_mat
	_local_caster.position = Vector3(40.0, 1.6, 0.0)
	add_child(_local_caster)

	_sun = DirectionalLight3D.new()
	_sun.name = "Sun"
	_sun.light_energy = 2.0
	_sun.light_angular_distance = 1.0
	_sun.shadow_enabled = true
	_sun.directional_shadow_max_distance = HANDOFF_DIST
	_sun.distance_field_shadow_start = HANDOFF_START
	_sun.distance_field_shadow_distance = HANDOFF_DIST
	_sun.rotation_degrees = Vector3(-45.0, -55.0, 0.0)
	add_child(_sun)

	_omni = OmniLight3D.new()
	_omni.name = "DFOmni"
	_omni.position = Vector3(35.5, 4.2, 0.0)
	_omni.omni_range = 24.0
	_omni.light_energy = 5.0
	_omni.light_size = 0.05
	_omni.shadow_enabled = true
	_omni.shadow_method = Light3D.SHADOW_METHOD_DISTANCE_FIELD
	_omni.visible = false
	add_child(_omni)

	_spot = SpotLight3D.new()
	_spot.name = "DFSpot"
	_spot.position = Vector3(35.5, 4.2, 0.0)
	_spot.spot_range = 24.0
	_spot.spot_angle = 55.0
	_spot.light_energy = 6.0
	_spot.light_size = 0.25
	_spot.shadow_enabled = true
	_spot.shadow_method = Light3D.SHADOW_METHOD_DISTANCE_FIELD
	_spot.visible = false
	add_child(_spot)
	_spot.look_at(Vector3(41.5, 0.0, 0.0), Vector3.UP)

	_camera = Camera3D.new()
	_camera.fov = 55.0
	_camera.far = 300.0
	_camera.current = true
	add_child(_camera)
	_set_directional_camera()


func _set_directional_camera() -> void:
	_camera.position = Vector3(0.0, 14.0, 18.0)
	_camera.look_at(Vector3(0.0, 0.0, -24.0), Vector3.UP)


func _set_local_camera() -> void:
	_camera.position = Vector3(41.5, 9.0, 10.0)
	_camera.look_at(Vector3(41.5, 0.2, 0.0), Vector3.UP)


func _luma(c: Color) -> float:
	return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b


func _sample_world_luma(img: Image, world_pos: Vector3, radius: int = 2) -> float:
	if _camera.is_position_behind(world_pos):
		return -1.0
	var vp_size := Vector2(img.get_size())
	var screen := _camera.unproject_position(world_pos)
	var cx := int(round(screen.x))
	var cy := int(round(screen.y))
	if cx < radius or cy < radius or cx >= int(vp_size.x) - radius or cy >= int(vp_size.y) - radius:
		return -1.0
	var total := 0.0
	var count := 0
	for dy in range(-radius, radius + 1):
		for dx in range(-radius, radius + 1):
			total += _luma(img.get_pixel(cx + dx, cy + dy))
			count += 1
	return total / float(max(count, 1))


func _sun_shadow_on_floor(world_pt: Vector3) -> Vector3:
	var ray_dir := -_sun.global_basis.z.normalized()
	var t := -world_pt.y / ray_dir.y
	return world_pt + ray_dir * t


func _omni_shadow_on_floor(light_pos: Vector3, world_pt: Vector3) -> Vector3:
	var ray_dir := (world_pt - light_pos).normalized()
	var t := -light_pos.y / ray_dir.y
	return light_pos + ray_dir * t


func _enter_stage(name: String) -> void:
	_stage = name
	_stage_frame = 0
	_raster_armed = false


func _process(_delta: float) -> void:
	_frame += 1
	_stage_frame += 1

	match _stage:
		"wait_sdf":
			var info := RenderingServer.virtual_shadow_maps_get_debug_info()
			var df: Dictionary = info.get("df_shadows", {})
			var sdf_instances := int(df.get("sdf_instances", 0))
			if sdf_instances >= 5 and _stage_frame >= SETTLE_FRAMES:
				_enter_stage("dir_handoff")
			elif _stage_frame >= SDF_WAIT_MAX:
				_failures.append("timed out waiting for runtime mesh SDFs (sdf_instances=%d, df=%s)" % [sdf_instances, str(df)])
				_finish()

		"dir_handoff":
			# Clear VSM map cache for one frame so all directional pages are dirty when raster check captures.
			if _stage_frame == 1:
				_sun.shadow_enabled = false
			elif _stage_frame == 2:
				_sun.shadow_enabled = true
				if not _raster_armed:
					RenderingServer.virtual_shadow_maps_request_raster_check()
					_raster_armed = true
			if _stage_frame < SETTLE_FRAMES:
				return
			_check_directional_handoff()
			# Move to Stage 2: local DF OmniLight3D (narrow penumbra); keep sun visible with shadow_enabled=false
			# and energy=0 so update_light() immediately frees its VSM maps cleanly.
			_sun.shadow_enabled = false
			_sun.light_energy = 0.0
			_omni.visible = true
			_omni.light_size = 0.05
			_spot.visible = false
			_set_local_camera()
			_enter_stage("local_omni_narrow")

		"local_omni_narrow":
			if _stage_frame < SETTLE_FRAMES:
				return
			_check_local_omni_narrow()
			_omni.light_size = 1.2
			_enter_stage("local_omni_wide")

		"local_omni_wide":
			if _stage_frame < SETTLE_FRAMES:
				return
			_check_local_omni_wide()
			_omni.visible = false
			_spot.visible = true
			_enter_stage("local_spot")

		"local_spot":
			if _stage_frame < SETTLE_FRAMES:
				return
			_check_local_spot()
			# Move to Stage 3: disable PS mesh SDFs and verify tier fallback.
			_spot.visible = false
			_sun.visible = true
			_sun.light_energy = 2.0
			_sun.shadow_enabled = false
			_set_directional_camera()
			ProjectSettings.set_setting(MESH_SDF_SETTING, false)
			_enter_stage("tier_fallback")

		"tier_fallback":
			if _stage_frame == 2:
				_sun.shadow_enabled = true
				if not _raster_armed:
					RenderingServer.virtual_shadow_maps_request_raster_check()
					_raster_armed = true
			if _stage_frame < SETTLE_FRAMES:
				return
			_check_tier_fallback()
			_finish()


func _check_directional_handoff() -> void:
	var tier := RenderingServer.virtual_shadow_maps_get_tier_info()
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	var df: Dictionary = info.get("df_shadows", {})
	var raster := RenderingServer.virtual_shadow_maps_get_raster_check_result()
	var img := get_viewport().get_texture().get_image()

	var near_shadow_pt := _sun_shadow_on_floor(Vector3(_near_caster.position.x, 3.0, _near_caster.position.z))
	var near_lit_pt := near_shadow_pt + Vector3(3.5, 0.0, 0.0)
	var far_shadow_pt := _sun_shadow_on_floor(_far_caster.position)
	var far_lit_pt := far_shadow_pt + Vector3(0.0, 0.0, 8.0)

	var near_shadow := _sample_world_luma(img, near_shadow_pt)
	var near_lit := _sample_world_luma(img, near_lit_pt)
	var far_shadow := _sample_world_luma(img, far_shadow_pt)
	var far_lit := _sample_world_luma(img, far_lit_pt)

	# Sample continuously along the blend wall's shadow across [18 m .. 29 m] camera distance,
	# bracketing the 10% blend band [21.6 m, 24.0 m].
	var band_samples: Array[float] = []
	var band_max_jump := 0.0
	var band_max_luma := 0.0
	var prev := -1.0
	for i in range(13):
		var z := 3.0 - float(i) * 0.85
		var pt := _sun_shadow_on_floor(Vector3(_blend_caster.position.x, 2.0, z))
		var lum := _sample_world_luma(img, pt, 1)
		if lum >= 0.0:
			band_samples.append(lum)
			band_max_luma = maxf(band_max_luma, lum)
			if prev >= 0.0:
				band_max_jump = maxf(band_max_jump, absf(lum - prev))
			prev = lum

	var gpu_far_culled := int(raster.get("gpu_df_far_culled", -1))
	var cpu_far_culled := int(raster.get("cpu_df_far_culled", -1))
	var raster_pass := bool(raster.get("pass", false))

	_result["directional_handoff"] = {
		"tier_df_available": bool(tier.get("df_available", false)),
		"tier_df_reason": String(tier.get("df_reason", "")),
		"lights_traced": int(df.get("lights_traced", 0)),
		"views": int(df.get("views", 0)),
		"sdf_instances": int(df.get("sdf_instances", 0)),
		"global_sdf_levels": int(df.get("global_sdf_levels", 0)),
		"detail_refinement": bool(df.get("detail_refinement", false)),
		"gpu_df_far_culled": gpu_far_culled,
		"cpu_df_far_culled": cpu_far_culled,
		"raster_check_pass": raster_pass,
		"near_shadow_luma": near_shadow,
		"near_lit_luma": near_lit,
		"far_shadow_luma": far_shadow,
		"far_lit_luma": far_lit,
		"blend_band_samples": band_samples.size(),
		"blend_band_max_luma": band_max_luma,
		"blend_band_max_jump": band_max_jump,
	}

	if not bool(tier.get("df_available", false)):
		_failures.append("dir_handoff: expected df_available=true, got reason=%s" % String(tier.get("df_reason", "")))
	if int(df.get("lights_traced", 0)) < 1:
		_failures.append("dir_handoff: expected df_shadows.lights_traced >= 1, got %d" % int(df.get("lights_traced", 0)))
	if not raster_pass:
		_failures.append("dir_handoff: VSM raster check failed: %s" % str(raster.get("failures", [])))
	if gpu_far_culled < 1 or cpu_far_culled < 1:
		_failures.append("dir_handoff: expected far non-Nanite caster dropped from VSM (gpu_df_far_culled=%d, cpu_df_far_culled=%d)" % [gpu_far_culled, cpu_far_culled])
	if near_shadow < 0.0 or near_lit <= 0.05 or near_shadow >= near_lit * 0.55:
		_failures.append("dir_handoff: near VSM shadow missing or too weak (shadow=%.4f, lit=%.4f)" % [near_shadow, near_lit])
	if far_shadow < 0.0 or far_lit <= 0.05 or far_shadow >= far_lit * 0.55:
		_failures.append("dir_handoff: far DF shadow missing or too weak (shadow=%.4f, lit=%.4f)" % [far_shadow, far_lit])
	if band_samples.size() < 6:
		_failures.append("dir_handoff: insufficient blend-band samples (%d)" % band_samples.size())
	elif band_max_luma >= near_lit * 0.6:
		_failures.append("dir_handoff: bright seam detected across blend band (max_luma=%.4f vs lit=%.4f)" % [band_max_luma, near_lit])


func _sample_local_edge_profile(img: Image) -> Array[float]:
	var shadow_center := _omni_shadow_on_floor(_omni.position, _local_caster.position)
	# Scan transversely along +Z from shadow umbra (z = 0) into lit floor (z = 3.2).
	var profile: Array[float] = []
	for i in range(17):
		var z := float(i) * 0.2
		var lum := _sample_world_luma(img, Vector3(shadow_center.x, 0.0, z), 1)
		profile.append(lum)
	return profile


func _check_local_omni_narrow() -> void:
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	var df: Dictionary = info.get("df_shadows", {})
	var proj: Dictionary = info.get("projection", {})
	var img := get_viewport().get_texture().get_image()

	var shadow_pt := _omni_shadow_on_floor(_omni.position, _local_caster.position)
	var lit_pt := Vector3(shadow_pt.x, 0.0, 4.0)
	var shadow_luma := _sample_world_luma(img, shadow_pt)
	var lit_luma := _sample_world_luma(img, lit_pt)
	_narrow_penumbra_samples = _sample_local_edge_profile(img)

	_result["local_omni_narrow"] = {
		"vsm_lights": int(info.get("lights", -1)),
		"atlas_passes": int(proj.get("atlas_passes", -1)),
		"lights_traced": int(df.get("lights_traced", 0)),
		"detail_refinement": bool(df.get("detail_refinement", false)),
		"shadow_luma": shadow_luma,
		"lit_luma": lit_luma,
		"edge_profile": _narrow_penumbra_samples,
	}

	if int(info.get("lights", -1)) != 0:
		_failures.append("local_omni: expected 0 VSM lights for DISTANCE_FIELD OmniLight3D, got %d" % int(info.get("lights", -1)))
	if int(proj.get("atlas_passes", -1)) > 0:
		_failures.append("local_omni: expected 0 shadow-atlas passes for DISTANCE_FIELD OmniLight3D, got %d" % int(proj.get("atlas_passes", -1)))
	if int(df.get("lights_traced", 0)) != 1:
		_failures.append("local_omni: expected df_shadows.lights_traced == 1, got %d" % int(df.get("lights_traced", 0)))
	if not bool(df.get("detail_refinement", false)):
		_failures.append("local_omni: expected 2 m mesh-SDF detail_refinement=true")
	if shadow_luma < 0.0 or lit_luma <= 0.05 or shadow_luma >= lit_luma * 0.45:
		_failures.append("local_omni: DF shadow missing or too weak (shadow=%.4f, lit=%.4f)" % [shadow_luma, lit_luma])


func _penumbra_intermediate_count(profile: Array[float], lo: float, hi: float) -> int:
	var n := 0
	for v in profile:
		if v > lo and v < hi:
			n += 1
	return n


func _penumbra_max_gradient(profile: Array[float]) -> float:
	var g := 0.0
	for i in range(1, profile.size()):
		if profile[i] >= 0.0 and profile[i - 1] >= 0.0:
			g = maxf(g, absf(profile[i] - profile[i - 1]))
	return g


func _check_local_omni_wide() -> void:
	var img := get_viewport().get_texture().get_image()
	var wide_profile := _sample_local_edge_profile(img)
	var shadow_pt := _omni_shadow_on_floor(_omni.position, _local_caster.position)
	var lit_pt := Vector3(shadow_pt.x, 0.0, 4.0)
	var shadow_luma := _sample_world_luma(img, shadow_pt)
	var lit_luma := _sample_world_luma(img, lit_pt)

	var lo := shadow_luma + (lit_luma - shadow_luma) * 0.15
	var hi := shadow_luma + (lit_luma - shadow_luma) * 0.85
	var narrow_mid := _penumbra_intermediate_count(_narrow_penumbra_samples, lo, hi)
	var wide_mid := _penumbra_intermediate_count(wide_profile, lo, hi)
	var narrow_grad := _penumbra_max_gradient(_narrow_penumbra_samples)
	var wide_grad := _penumbra_max_gradient(wide_profile)

	_result["local_omni_wide"] = {
		"shadow_luma": shadow_luma,
		"lit_luma": lit_luma,
		"narrow_mid_samples": narrow_mid,
		"wide_mid_samples": wide_mid,
		"narrow_max_grad": narrow_grad,
		"wide_max_grad": wide_grad,
		"edge_profile": wide_profile,
	}

	if shadow_luma >= lit_luma * 0.55:
		_failures.append("local_omni_wide: umbra lost at light_size=1.2 (shadow=%.4f, lit=%.4f)" % [shadow_luma, lit_luma])
	if not (wide_mid > narrow_mid or wide_grad < narrow_grad * 0.92):
		_failures.append("local_omni_wide: light_size=1.2 did not soften penumbra (narrow_mid=%d wide_mid=%d narrow_grad=%.4f wide_grad=%.4f)" % [narrow_mid, wide_mid, narrow_grad, wide_grad])


func _check_local_spot() -> void:
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	var df: Dictionary = info.get("df_shadows", {})
	var proj: Dictionary = info.get("projection", {})
	var img := get_viewport().get_texture().get_image()

	var shadow_pt := _omni_shadow_on_floor(_spot.position, _local_caster.position)
	var lit_pt := Vector3(shadow_pt.x, 0.0, 3.0)
	var shadow_luma := _sample_world_luma(img, shadow_pt)
	var lit_luma := _sample_world_luma(img, lit_pt)

	_result["local_spot"] = {
		"vsm_lights": int(info.get("lights", -1)),
		"atlas_passes": int(proj.get("atlas_passes", -1)),
		"lights_traced": int(df.get("lights_traced", 0)),
		"shadow_luma": shadow_luma,
		"lit_luma": lit_luma,
	}

	if int(info.get("lights", -1)) != 0:
		_failures.append("local_spot: expected 0 VSM lights for DISTANCE_FIELD SpotLight3D, got %d" % int(info.get("lights", -1)))
	if int(proj.get("atlas_passes", -1)) > 0:
		_failures.append("local_spot: expected 0 shadow-atlas passes for DISTANCE_FIELD SpotLight3D, got %d" % int(proj.get("atlas_passes", -1)))
	if int(df.get("lights_traced", 0)) != 1:
		_failures.append("local_spot: expected df_shadows.lights_traced == 1, got %d" % int(df.get("lights_traced", 0)))
	if shadow_luma < 0.0 or lit_luma <= 0.05 or shadow_luma >= lit_luma * 0.55:
		_failures.append("local_spot: DF shadow missing or too weak (shadow=%.4f, lit=%.4f)" % [shadow_luma, lit_luma])


func _check_tier_fallback() -> void:
	var tier := RenderingServer.virtual_shadow_maps_get_tier_info()
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	var df: Dictionary = info.get("df_shadows", {})
	var raster := RenderingServer.virtual_shadow_maps_get_raster_check_result()
	var img := get_viewport().get_texture().get_image()

	var far_shadow_pt := _sun_shadow_on_floor(_far_caster.position)
	var far_lit_pt := far_shadow_pt + Vector3(0.0, 0.0, 8.0)
	var far_shadow := _sample_world_luma(img, far_shadow_pt)
	var far_lit := _sample_world_luma(img, far_lit_pt)
	var gpu_far_culled := int(raster.get("gpu_df_far_culled", -1))

	_result["tier_fallback"] = {
		"df_available": bool(tier.get("df_available", true)),
		"df_reason": String(tier.get("df_reason", "")),
		"lights_traced": int(df.get("lights_traced", -1)),
		"gpu_df_far_culled": gpu_far_culled,
		"raster_check_pass": bool(raster.get("pass", false)),
		"far_shadow_luma": far_shadow,
		"far_lit_luma": far_lit,
	}

	if bool(tier.get("df_available", true)):
		_failures.append("tier_fallback: expected df_available=false when mesh_sdf/generate=false")
	if String(tier.get("df_reason", "")) != "MESH_SDF_DISABLED":
		_failures.append("tier_fallback: expected df_reason=MESH_SDF_DISABLED, got %s" % String(tier.get("df_reason", "")))
	if int(df.get("lights_traced", -1)) != 0:
		_failures.append("tier_fallback: expected df_shadows.lights_traced == 0 when disabled, got %d" % int(df.get("lights_traced", -1)))
	if gpu_far_culled != 0:
		_failures.append("tier_fallback: expected gpu_df_far_culled == 0 when DF unavailable, got %d" % gpu_far_culled)
	if not bool(raster.get("pass", false)):
		_failures.append("tier_fallback: VSM raster check failed during fallback: %s" % str(raster.get("failures", [])))
	if far_shadow < 0.0 or far_lit <= 0.05 or far_shadow >= far_lit * 0.55:
		_failures.append("tier_fallback: VSM did not continue to full distance when DF disabled (shadow=%.4f, lit=%.4f)" % [far_shadow, far_lit])


func _finish() -> void:
	ProjectSettings.set_setting(MESH_SDF_SETTING, _saved_mesh_sdf)
	_result["pass"] = _failures.is_empty()
	_result["failures"] = _failures
	print("GODOTNEXT_RESULT %s" % JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
