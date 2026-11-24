extends Node3D

# GPU verification for GodotNext #141 (standalone HWRT ray-traced shadows):
# (a) setting off -> byte-identical frame vs baseline, 0 RT VRAM, 0 RT passes
# (b) directional + 64 local RT-shadowed lights (omni/spot/area) with 0 fallbacks and 0 atlas passes
# (c) soft penumbra widening with light_size / light_angular_distance
# (d) translucent caster shadows as opaque (LQ6)
# (e) alpha-tested caster cutout via #145
# (f) per-light override SHADOW_METHOD_RAY_TRACED works when global ray_traced_shadows == false

const SETTING_RT_SHADOWS := "rendering/lights_and_shadows/ray_traced_shadows"
const WARMUP_FRAMES := 8
const PENUMBRA_WARMUP_FRAMES := 14

var _camera: Camera3D
var _sun: DirectionalLight3D
var _floor_mesh: MeshInstance3D
var _blocker_opaque: MeshInstance3D
var _blocker_translucent: MeshInstance3D
var _blocker_cutout: MeshInstance3D
var _local_lights_root: Node3D

var _stage: String = "init"
var _stage_frame: int = 0
var _failures: PackedStringArray = []
var _result: Dictionary = {}

var _baseline_off_bytes: PackedByteArray
var _sharp_penumbra_width: int = 0
var _soft_penumbra_width: int = 0


func _ready() -> void:
	ProjectSettings.set_setting(SETTING_RT_SHADOWS, false)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0, 1.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.nextgi_enabled = false

	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	_camera = Camera3D.new()
	_camera.fov = 55.0
	_camera.near = 0.1
	_camera.far = 100.0
	add_child(_camera)
	_camera.look_at_from_position(Vector3(0.0, 10.5, 9.5), Vector3(0.0, 0.0, 0.0), Vector3.UP)

	# Large matte white receiver floor.
	_floor_mesh = MeshInstance3D.new()
	var floor_box := BoxMesh.new()
	floor_box.size = Vector3(24.0, 0.4, 24.0)
	_floor_mesh.mesh = floor_box
	_floor_mesh.position = Vector3(0.0, -0.2, 0.0)
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.9, 0.9, 0.9, 1.0)
	floor_mat.roughness = 1.0
	floor_mat.metallic = 0.0
	_floor_mesh.material_override = floor_mat
	add_child(_floor_mesh)

	# Directional light angled so casters sit at (-2.0, 2.5, 0.0) and cast shadows centered around (0.0, 0.0, 0.0).
	_sun = DirectionalLight3D.new()
	_sun.light_energy = 1.2
	_sun.light_color = Color.WHITE
	_sun.shadow_enabled = true
	_sun.shadow_method = Light3D.SHADOW_METHOD_PROJECT
	_sun.light_angular_distance = 0.0
	add_child(_sun)
	_sun.look_at_from_position(Vector3(-4.0, 5.0, 0.0), Vector3(0.0, 0.0, 0.0), Vector3.UP)

	# Opaque blocker casting onto floor center (0, 0, 0) along sun direction (-4, 5, 0) -> (0, 0, 0).
	_blocker_opaque = MeshInstance3D.new()
	var opaque_box := BoxMesh.new()
	opaque_box.size = Vector3(1.8, 1.8, 2.6)
	_blocker_opaque.mesh = opaque_box
	_blocker_opaque.position = Vector3(-2.0, 2.5, 0.0)
	var opaque_mat := StandardMaterial3D.new()
	opaque_mat.albedo_color = Color(0.4, 0.4, 0.4, 1.0)
	opaque_mat.roughness = 1.0
	_blocker_opaque.material_override = opaque_mat
	add_child(_blocker_opaque)

	# Translucent (alpha-blended) blocker for LQ6 test (translucent casters shadow as opaque).
	_blocker_translucent = MeshInstance3D.new()
	var trans_box := BoxMesh.new()
	trans_box.size = Vector3(1.8, 1.8, 2.6)
	_blocker_translucent.mesh = trans_box
	_blocker_translucent.position = Vector3(-2.0, 2.5, 0.0)
	var trans_mat := StandardMaterial3D.new()
	trans_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	trans_mat.albedo_color = Color(0.9, 0.2, 0.2, 0.25)
	trans_mat.roughness = 0.8
	_blocker_translucent.material_override = trans_mat
	_blocker_translucent.visible = false
	add_child(_blocker_translucent)

	# Alpha-scissor cutout frame (outer border opaque, inner 50% square transparent) for #145 test.
	_blocker_cutout = MeshInstance3D.new()
	var cutout_quad := QuadMesh.new()
	cutout_quad.size = Vector2(3.6, 3.6)
	_blocker_cutout.mesh = cutout_quad
	var cutout_img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in range(64):
		for x in range(64):
			var in_hole := (x >= 16 and x < 48 and y >= 16 and y < 48)
			cutout_img.set_pixel(x, y, Color(0.6, 0.6, 0.6, 0.0 if in_hole else 1.0))
	var cutout_tex := ImageTexture.create_from_image(cutout_img)
	var cutout_mat := StandardMaterial3D.new()
	cutout_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	cutout_mat.alpha_scissor_threshold = 0.5
	cutout_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	cutout_mat.albedo_texture = cutout_tex
	cutout_mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	_blocker_cutout.material_override = cutout_mat
	_blocker_cutout.visible = false
	add_child(_blocker_cutout)
	_blocker_cutout.look_at_from_position(Vector3(-2.0, 2.5, 0.0), Vector3(0.0, 0.0, 0.0), Vector3.UP)

	_local_lights_root = Node3D.new()
	add_child(_local_lights_root)

	_next_stage("off_baseline")


func _next_stage(p_stage: String) -> void:
	_stage = p_stage
	_stage_frame = 0


func _rt_debug_info() -> Dictionary:
	var dbg := RenderingServer.virtual_shadow_maps_get_debug_info()
	if dbg.has("rt_shadows") and dbg["rt_shadows"] is Dictionary:
		return dbg["rt_shadows"]
	return {}


func _sample_world_lum(p_img: Image, p_world_pos: Vector3, p_half_box: int = 2) -> float:
	if _camera.is_position_behind(p_world_pos):
		return -1.0
	var p := _camera.unproject_position(p_world_pos)
	var vp_size := get_viewport().get_visible_rect().size
	var sx := float(p_img.get_width()) / maxf(vp_size.x, 1.0)
	var sy := float(p_img.get_height()) / maxf(vp_size.y, 1.0)
	var cx := clampi(int(round(p.x * sx)), p_half_box, p_img.get_width() - 1 - p_half_box)
	var cy := clampi(int(round(p.y * sy)), p_half_box, p_img.get_height() - 1 - p_half_box)
	var acc := 0.0
	var count := 0
	for dy in range(-p_half_box, p_half_box + 1):
		for dx in range(-p_half_box, p_half_box + 1):
			var c := p_img.get_pixel(cx + dx, cy + dy)
			acc += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
			count += 1
	return acc / maxf(float(count), 1.0)


func _measure_penumbra_width(p_img: Image, p_lit_lum: float) -> int:
	# Scan across the right shadow edge on the floor from shadow center (0, 0, 0) to lit floor (4.5, 0, 0).
	var p0 := _camera.unproject_position(Vector3(0.0, 0.0, 0.0))
	var p1 := _camera.unproject_position(Vector3(4.5, 0.0, 0.0))
	var vp_size := get_viewport().get_visible_rect().size
	var sx := float(p_img.get_width()) / maxf(vp_size.x, 1.0)
	var sy := float(p_img.get_height()) / maxf(vp_size.y, 1.0)
	var x0 := clampi(int(round(p0.x * sx)), 0, p_img.get_width() - 1)
	var x1 := clampi(int(round(p1.x * sx)), 0, p_img.get_width() - 1)
	var y := clampi(int(round(p0.y * sy)), 2, p_img.get_height() - 3)
	if x1 < x0:
		var tmp := x0
		x0 = x1
		x1 = tmp
	var denom := maxf(p_lit_lum, 0.001)
	var transition_px := 0
	for x in range(x0, x1 + 1):
		var acc := 0.0
		for dy in range(-2, 3):
			var c := p_img.get_pixel(x, y + dy)
			acc += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
		var ratio := (acc / 5.0) / denom
		if ratio > 0.15 and ratio < 0.85:
			transition_px += 1
	return transition_px


func _spawn_64_local_lights() -> void:
	_clear_local_lights()
	var idx := 0
	for gy in range(8):
		for gx in range(8):
			var pos := Vector3(-7.0 + float(gx) * 2.0, 1.8, -7.0 + float(gy) * 2.0)
			var light: Light3D
			if idx < 22:
				var omni := OmniLight3D.new()
				omni.omni_range = 3.2
				omni.light_size = 0.12
				light = omni
			elif idx < 44:
				var spot := SpotLight3D.new()
				spot.spot_range = 3.6
				spot.spot_angle = 42.0
				spot.light_size = 0.12
				light = spot
			else:
				var area := AreaLight3D.new()
				area.area_range = 3.4
				area.area_size = Vector2(0.5, 0.5)
				area.light_size = 0.12
				light = area
			light.light_energy = 0.35
			light.light_color = Color.from_hsv(float(idx) / 64.0, 0.35, 1.0)
			light.shadow_enabled = true
			light.shadow_method = Light3D.SHADOW_METHOD_PROJECT
			_local_lights_root.add_child(light)
			if light is OmniLight3D:
				light.position = pos
			else:
				light.look_at_from_position(pos, Vector3(pos.x, 0.0, pos.z + 0.01), Vector3.UP)
			idx += 1


func _clear_local_lights() -> void:
	for child in _local_lights_root.get_children():
		_local_lights_root.remove_child(child)
		child.queue_free()


func _process(_delta: float) -> void:
	_stage_frame += 1

	match _stage:
		"off_baseline":
			if _stage_frame < WARMUP_FRAMES:
				return
			var tier := RenderingServer.virtual_shadow_maps_get_tier_info()
			var rt := _rt_debug_info()
			_result["tier"] = tier
			_result["off_baseline_rt"] = rt
			if not bool(tier.get("rt_available", false)):
				_failures.append("RT shadows tier is not available (rt_reason=%s)" % str(tier.get("rt_reason", "")))
				_finish()
				return
			if bool(rt.get("active", false)):
				_failures.append("setting off: rt_shadows.active should be false")
			if int(rt.get("lights_traced", -1)) != 0:
				_failures.append("setting off: expected 0 lights_traced, got %s" % str(rt.get("lights_traced", -1)))
			if int(rt.get("atlas_vram_bytes", -1)) != 0:
				_failures.append("setting off: expected 0 atlas_vram_bytes, got %s" % str(rt.get("atlas_vram_bytes", -1)))
			var img := get_viewport().get_texture().get_image()
			_baseline_off_bytes = img.get_data()
			# Next: (f) per-light override SHADOW_METHOD_RAY_TRACED with global setting still false.
			_sun.shadow_method = Light3D.SHADOW_METHOD_RAY_TRACED
			_sun.light_angular_distance = 0.0
			_next_stage("per_light_override")

		"per_light_override":
			if _stage_frame < WARMUP_FRAMES:
				return
			var rt := _rt_debug_info()
			_result["per_light_override_rt"] = rt
			if not bool(rt.get("active", false)):
				_failures.append("per-light override: rt_shadows.active should be true when global setting is false")
			if int(rt.get("lights_traced", 0)) != 1 or int(rt.get("directional_lights", 0)) != 1:
				_failures.append("per-light override: expected 1 directional RT light, got %s" % str(rt))
			if int(rt.get("tiles_allocated", 0)) <= 0:
				_failures.append("per-light override: expected tiles_allocated > 0, got %s" % str(rt.get("tiles_allocated", 0)))
			var img := get_viewport().get_texture().get_image()
			var lit := _sample_world_lum(img, Vector3(4.5, 0.0, 0.0))
			var sh := _sample_world_lum(img, Vector3(0.0, 0.0, 0.0))
			_result["per_light_override_lum"] = { "lit": lit, "shadow": sh }
			if lit < 0.2:
				_failures.append("per-light override: lit floor too dark (%.4f)" % lit)
			if sh >= lit * 0.2:
				_failures.append("per-light override: opaque box did not cast RT shadow (lit=%.4f shadow=%.4f)" % [lit, sh])
			_sharp_penumbra_width = _measure_penumbra_width(img, lit)
			_result["penumbra_sharp_px"] = _sharp_penumbra_width
			# Next: (c) soft penumbra widening with light_angular_distance.
			_sun.light_angular_distance = 12.0
			_next_stage("penumbra_soft")

		"penumbra_soft":
			if _stage_frame < PENUMBRA_WARMUP_FRAMES:
				return
			var img := get_viewport().get_texture().get_image()
			var lit := _sample_world_lum(img, Vector3(4.5, 0.0, 0.0))
			_soft_penumbra_width = _measure_penumbra_width(img, lit)
			_result["penumbra_soft_px"] = _soft_penumbra_width
			if _soft_penumbra_width <= _sharp_penumbra_width:
				_failures.append("soft penumbra did not widen (sharp=%d px, soft=%d px)" % [_sharp_penumbra_width, _soft_penumbra_width])
			# Next: (d) translucent caster shadows as opaque (LQ6).
			_sun.light_angular_distance = 0.0
			_blocker_opaque.visible = false
			_blocker_translucent.visible = true
			_next_stage("translucent_caster")

		"translucent_caster":
			if _stage_frame < WARMUP_FRAMES:
				return
			var img := get_viewport().get_texture().get_image()
			var lit := _sample_world_lum(img, Vector3(4.5, 0.0, 0.0))
			var sh := _sample_world_lum(img, Vector3(0.0, 0.0, 0.0))
			_result["translucent_caster_lum"] = { "lit": lit, "shadow": sh }
			if lit < 0.2 or sh >= lit * 0.25:
				_failures.append("translucent caster did not shadow as opaque (LQ6): lit=%.4f shadow=%.4f" % [lit, sh])
			# Next: (e) alpha-tested cutout caster via #145.
			_blocker_translucent.visible = false
			_blocker_cutout.visible = true
			_next_stage("alpha_cutout")

		"alpha_cutout":
			if _stage_frame < WARMUP_FRAMES:
				return
			var img := get_viewport().get_texture().get_image()
			var lit := _sample_world_lum(img, Vector3(5.0, 0.0, 0.0))
			var hole := _sample_world_lum(img, Vector3(0.0, 0.0, 0.0))
			var border := _sample_world_lum(img, Vector3(0.0, 0.0, 1.35))
			_result["alpha_cutout_lum"] = { "lit": lit, "hole": hole, "border": border }
			if lit < 0.2:
				_failures.append("alpha cutout: lit floor too dark (%.4f)" % lit)
			if hole < lit * 0.6:
				_failures.append("alpha cutout: transparent center did not transmit light (lit=%.4f hole=%.4f)" % [lit, hole])
			if border >= lit * 0.35:
				_failures.append("alpha cutout: opaque border did not cast shadow (lit=%.4f border=%.4f)" % [lit, border])
			# Next: (b) directional + 64 local RT-shadowed lights (omni/spot/area) with 0 fallbacks and 0 atlas passes.
			_blocker_cutout.visible = false
			_blocker_opaque.visible = true
			ProjectSettings.set_setting(SETTING_RT_SHADOWS, true)
			_sun.shadow_method = Light3D.SHADOW_METHOD_PROJECT
			_sun.light_energy = 0.2
			_spawn_64_local_lights()
			_next_stage("many_lights_65")

		"many_lights_65":
			if _stage_frame < WARMUP_FRAMES:
				return
			var rt := _rt_debug_info()
			var shadow_draws := RenderingServer.viewport_get_render_info(
				get_viewport().get_viewport_rid(),
				RenderingServer.VIEWPORT_RENDER_INFO_TYPE_SHADOW,
				RenderingServer.VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME
			)
			_result["many_lights_65_rt"] = rt
			_result["many_lights_65_shadow_atlas_draws"] = shadow_draws
			if not bool(rt.get("active", false)):
				_failures.append("65 lights: rt_shadows.active should be true")
			if int(rt.get("lights_traced", 0)) != 65:
				_failures.append("65 lights: expected lights_traced == 65, got %s" % str(rt.get("lights_traced", 0)))
			if int(rt.get("directional_lights", 0)) != 1 or int(rt.get("local_lights", 0)) != 64:
				_failures.append("65 lights: expected 1 directional + 64 local, got %s" % str(rt))
			if int(rt.get("overflow_lights", -1)) != 0 or int(rt.get("fallback_lights", -1)) != 0:
				_failures.append("65 lights: expected 0 overflow/fallback lights, got overflow=%s fallback=%s" % [str(rt.get("overflow_lights", -1)), str(rt.get("fallback_lights", -1))])
			if int(rt.get("tiles_allocated", 0)) <= 0 or int(rt.get("tiles_allocated", 0)) > int(rt.get("tile_budget", 0)):
				_failures.append("65 lights: tiles_allocated out of budget (%s / %s)" % [str(rt.get("tiles_allocated", 0)), str(rt.get("tile_budget", 0))])
			if shadow_draws != 0:
				_failures.append("65 lights: expected 0 shadow-atlas draw calls, got %d" % shadow_draws)
			# Next: (a) verify setting off produces byte-identical frame vs baseline!
			_clear_local_lights()
			ProjectSettings.set_setting(SETTING_RT_SHADOWS, false)
			_sun.shadow_method = Light3D.SHADOW_METHOD_PROJECT
			_sun.light_energy = 1.2
			_sun.light_angular_distance = 0.0
			_next_stage("off_byte_identical")

		"off_byte_identical":
			if _stage_frame < WARMUP_FRAMES:
				return
			var rt := _rt_debug_info()
			_result["off_after_rt"] = rt
			var img := get_viewport().get_texture().get_image()
			var after_bytes := img.get_data()
			var byte_identical := (after_bytes == _baseline_off_bytes)
			_result["off_byte_identical"] = byte_identical
			if bool(rt.get("active", false)) or int(rt.get("lights_traced", -1)) != 0:
				_failures.append("off_after: expected inactive with 0 lights_traced, got %s" % str(rt))
			if not byte_identical:
				var diff_bytes := 0
				var n := mini(after_bytes.size(), _baseline_off_bytes.size())
				for i in range(n):
					if after_bytes[i] != _baseline_off_bytes[i]:
						diff_bytes += 1
				_failures.append("setting off frame is not byte-identical to baseline (%d differing bytes, sizes %d vs %d)" % [diff_bytes, after_bytes.size(), _baseline_off_bytes.size()])
			_finish()


func _finish() -> void:
	ProjectSettings.set_setting(SETTING_RT_SHADOWS, false)
	_result["pass"] = _failures.is_empty()
	_result["failures"] = Array(_failures)
	print("RT_SHADOWS_CHECK_RESULT=" + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
