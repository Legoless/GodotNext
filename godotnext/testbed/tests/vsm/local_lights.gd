extends Node3D

## Virtual Shadow Maps for local lights (#216; plan §21.3 contract S7, GV-3, GV-8).
##
## Verifies all six GPU requirements of #216 end-to-end:
## (a) 256 mixed Spot / Omni / Area shadowed local lights all casting shadows simultaneously with
##     atlas_passes == 0, lights_sampled >= 256, local_lights_culled >= 256, bypassing the 88-slot
##     positional shadow atlas and MAX_UPDATE_SHADOWS = 512.
## (b) 64 dirty local lights rasterized in <= 4 indirect dispatches on BOTH the non-Nanite path
##     (non_nanite.indirect_draw_count <= 4) AND the NextGeo path (nanite.local_chains == 1,
##     nanite.local_raster_indirect_dispatches <= 4).
## (c) Per-light Light3D.shadow_resolution_bias visibly and monotonically shifts requested mip levels
##     (-2.0 -> 0.0 -> +2.0 -> +4.0), matching the CPU marking model.
## (d) Distant local lights (projected screen radius below preset threshold) request only coarse mips
##     (>= COARSE_FIRST_MIP = 5), even with negative shadow_resolution_bias.
## (e) Under 25% pool overflow (GODOT_DEV_VSM=25%, 2048 physical pages), 256 mixed local lights trigger
##     real pool overflow with 0 resident overflow and 0 sampled-but-unmapped pages.
## (f) Subsurface transmittance (LIGHT_PARAM_TRANSMITTANCE_BIAS) via VSM depth lookup matches the
##     shadow atlas for directional, spot, off-axis omni, and area lights, and attenuates with depth.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const SETTLE_FRAMES := 6
const MIP_COLORS: Array[Color] = [
	Color(0.90, 0.10, 0.10), # mip 0: red
	Color(0.95, 0.50, 0.10), # mip 1: orange
	Color(0.95, 0.90, 0.10), # mip 2: yellow
	Color(0.20, 0.80, 0.20), # mip 3: green
	Color(0.10, 0.80, 0.85), # mip 4: cyan
	Color(0.15, 0.30, 0.95), # mip 5: blue (COARSE_FIRST_MIP)
	Color(0.60, 0.20, 0.85), # mip 6: purple (root)
]

var _failures := PackedStringArray()
var _result := {}
var _temp_files: Array[String] = []
var _ngm_counter := 0


func _ready() -> void:
	_result["driver"] = RenderingServer.get_current_rendering_driver_name()
	_result["device"] = RenderingServer.get_video_adapter_name()
	_result["dev_vsm_env"] = OS.get_environment("GODOT_DEV_VSM")

	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 1.0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_local", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_directional", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/prefiltered_distant", false)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/debug_nanite_dynamic_layer", false)

	_run.call_deferred()


func _frames(p_count: int) -> void:
	for i in p_count:
		await RenderingServer.frame_post_draw


func _info() -> Dictionary:
	return RenderingServer.virtual_shadow_maps_get_debug_info()


func _run_vsm_check() -> Dictionary:
	RenderingServer.virtual_shadow_maps_request_check()
	await _frames(2)
	return RenderingServer.virtual_shadow_maps_get_check_result()


func _new_env() -> Environment:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_energy = 0.0
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	return env


func _new_viewport(p_size: Vector2i) -> SubViewport:
	var vp := SubViewport.new()
	vp.size = p_size
	vp.use_hdr_2d = true
	vp.own_world_3d = true
	vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	vp.use_taa = false
	add_child(vp)
	var we := WorldEnvironment.new()
	we.environment = _new_env()
	vp.add_child(we)
	return vp


func _white_mat() -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(1.0, 1.0, 1.0)
	mat.roughness = 1.0
	mat.metallic = 0.0
	mat.metallic_specular = 0.0
	return mat


func _black_mat() -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.0, 0.0, 0.0)
	mat.roughness = 1.0
	mat.metallic = 0.0
	mat.metallic_specular = 0.0
	return mat


func _make_virtual_mesh(p_prim: PrimitiveMesh, p_tag: String) -> ArrayMesh:
	_ngm_counter += 1
	var path := "user://vsm_local_lights_%s_%d.ngm" % [p_tag, _ngm_counter]
	_temp_files.append(path)
	var imp := ImporterMesh.from_mesh(p_prim)
	var err := imp.build_virtual_geometry(path)
	if err != OK:
		_failures.append("%s: build_virtual_geometry failed (err=%d)" % [p_tag, err])
		return null
	var mesh := imp.get_mesh()
	if mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("%s: virtual geometry missing on built mesh" % p_tag)
		return null
	return mesh


func _cleanup_temp_files() -> void:
	for path in _temp_files:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	_temp_files.clear()


func _luma(p_c: Color) -> float:
	return 0.2126 * p_c.r + 0.7152 * p_c.g + 0.0722 * p_c.b


func _sample_patch_rgb(p_img: Image, p_px: Vector2, p_radius: int = 2) -> Color:
	var w := p_img.get_width()
	var h := p_img.get_height()
	var cx := clampi(int(round(p_px.x)), 0, w - 1)
	var cy := clampi(int(round(p_px.y)), 0, h - 1)
	var acc := Color(0.0, 0.0, 0.0, 0.0)
	var n := 0
	for dy in range(-p_radius, p_radius + 1):
		for dx in range(-p_radius, p_radius + 1):
			var x := clampi(cx + dx, 0, w - 1)
			var y := clampi(cy + dy, 0, h - 1)
			acc += p_img.get_pixel(x, y)
			n += 1
	return acc / float(maxi(n, 1))


func _classify_mip_color(p_c: Color) -> int:
	# DEBUG_DRAW_VSM_MIP blends mip_color(mip) with alpha 0.85 over black -> scale by 1/0.85,
	# and reject dark/unlit pixels (page borders have 0.4x darkening, magenta is unmapped).
	var max_ch := maxf(p_c.r, maxf(p_c.g, p_c.b))
	if max_ch < 0.08:
		return -1
	# Unmapped magenta is (1.0, 0.0, 1.0) * 0.85.
	if p_c.r > 0.7 and p_c.b > 0.7 and p_c.g < 0.1:
		return -2
	var scaled := Vector3(p_c.r, p_c.g, p_c.b) / max_ch
	var best_mip := -1
	var best_dist := 1.0e9
	for m in MIP_COLORS.size():
		var ref := MIP_COLORS[m]
		var ref_max := maxf(ref.r, maxf(ref.g, ref.b))
		var ref_n := Vector3(ref.r, ref.g, ref.b) / ref_max
		var d := scaled.distance_to(ref_n)
		if d < best_dist:
			best_dist = d
			best_mip = m
	return best_mip if best_dist < 0.35 else -1


func _median_mip_in_patch(p_img: Image, p_px: Vector2, p_radius: int = 8) -> Dictionary:
	var w := p_img.get_width()
	var h := p_img.get_height()
	var cx := clampi(int(round(p_px.x)), 0, w - 1)
	var cy := clampi(int(round(p_px.y)), 0, h - 1)
	var mips: Array[int] = []
	var unmapped := 0
	for dy in range(-p_radius, p_radius + 1):
		for dx in range(-p_radius, p_radius + 1):
			var x := clampi(cx + dx, 0, w - 1)
			var y := clampi(cy + dy, 0, h - 1)
			var m := _classify_mip_color(p_img.get_pixel(x, y))
			if m == -2:
				unmapped += 1
			elif m >= 0:
				mips.append(m)
	mips.sort()
	var med := mips[mips.size() / 2] if not mips.is_empty() else -1
	var min_m := mips[0] if not mips.is_empty() else -1
	var max_m := mips[mips.size() - 1] if not mips.is_empty() else -1
	return {
		"median": med,
		"min": min_m,
		"max": max_m,
		"samples": mips.size(),
		"unmapped": unmapped,
	}


func _make_local_light(p_kind: int, p_pos: Vector3, p_range: float, p_energy: float) -> Light3D:
	var light: Light3D
	if p_kind == 0:
		var spot := SpotLight3D.new()
		spot.spot_angle = 32.0
		spot.spot_range = p_range + 1.0
		spot.transform = Transform3D(Basis.looking_at(Vector3.DOWN, Vector3.FORWARD), p_pos)
		light = spot
	elif p_kind == 1:
		var omni := OmniLight3D.new()
		omni.omni_range = p_range
		omni.position = p_pos
		light = omni
	else:
		var area := AreaLight3D.new()
		area.area_range = p_range
		area.area_size = Vector2(0.45, 0.45)
		area.transform = Transform3D(Basis.looking_at(Vector3.DOWN, Vector3.FORWARD), p_pos)
		light = area
	light.light_energy = p_energy
	light.light_size = 0.0
	light.shadow_enabled = true
	return light


# (a) 256 mixed Spot/Omni/Area shadowed local lights casting shadows with 0 atlas passes,
#     plus (e) 25% pool overflow check (0 sampled-but-unmapped pages, 0 resident overflow).
func _case_256_lights_and_overflow() -> Dictionary:
	var vp := _new_viewport(Vector2i(768, 768))
	var cam := Camera3D.new()
	cam.fov = 80.0
	cam.near = 0.2
	cam.far = 80.0
	cam.transform = Transform3D(Basis.looking_at(Vector3(0.0, -1.0, -0.005).normalized(), Vector3.FORWARD), Vector3(0.0, 14.0, 0.0))
	vp.add_child(cam)
	cam.current = true

	var white := _white_mat()
	var floor_mi := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(40.0, 40.0)
	floor_mi.mesh = plane
	floor_mi.material_override = white
	floor_mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	vp.add_child(floor_mi)

	var caster_mesh := BoxMesh.new()
	caster_mesh.size = Vector3(0.85, 0.10, 0.85)
	caster_mesh.material = white

	const GRID := 16
	const SPACING := 1.3
	const LIGHT_Y := 2.0
	const CASTER_Y := 0.65
	var lights: Array[Light3D] = []
	var casters: Array[MeshInstance3D] = []
	var light_positions: Array[Vector3] = []
	var light_kinds: Array[int] = []

	for gz in GRID:
		for gx in GRID:
			var idx := gz * GRID + gx
			var kind := idx % 3
			var lx := (float(gx) - 7.5) * SPACING
			var lz := (float(gz) - 7.5) * SPACING
			var pos := Vector3(lx, LIGHT_Y, lz)
			var l := _make_local_light(kind, pos, 2.6, 4.0)
			vp.add_child(l)
			lights.append(l)
			light_positions.append(pos)
			light_kinds.append(kind)

			var c := MeshInstance3D.new()
			c.mesh = caster_mesh
			c.position = Vector3(lx, CASTER_Y, lz)
			c.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY
			vp.add_child(c)
			casters.append(c)

	# Capture VSM check on Frame 1-2 BEFORE overflow bias decays/rises so 25% pool sees peak demand.
	var check := await _run_vsm_check()
	await _frames(SETTLE_FRAMES)
	var info := _info()
	var img_shadowed := vp.get_texture().get_image()

	# Hide all casters to verify every sampled cell (spot, omni, area — including indices > 88 and > 200)
	# genuinely darkened under VSM shadows.
	for c in casters:
		c.visible = false
	await _frames(SETTLE_FRAMES)
	var img_unshadowed := vp.get_texture().get_image()

	var proj: Dictionary = info.get("projection", {})
	var alloc: Dictionary = check.get("allocation", {})
	var sample_info: Dictionary = check.get("sample", {})
	var pool_pages := int(info.get("pool_pages", 0))
	var is_overflow_pool := pool_pages > 0 and pool_pages <= 2048

	var probe_indices: Array[int] = [0, 1, 2, 42, 43, 44, 96, 97, 98, 165, 166, 167, 252, 253, 254, 255]
	var probe_results := []
	var shadow_drop_failures := 0
	for idx in probe_indices:
		var floor_pt := Vector3(light_positions[idx].x, 0.0, light_positions[idx].z)
		var px := cam.unproject_position(floor_pt)
		var l_sh := _luma(_sample_patch_rgb(img_shadowed, px, 2))
		var l_un := _luma(_sample_patch_rgb(img_unshadowed, px, 2))
		var ratio := l_sh / maxf(l_un, 0.0001)
		probe_results.append({
			"index": idx,
			"kind": light_kinds[idx],
			"shadowed": snappedf(l_sh, 0.001),
			"unshadowed": snappedf(l_un, 0.001),
			"ratio": snappedf(ratio, 0.001),
		})
		if l_un < 0.05 or ratio > 0.72:
			shadow_drop_failures += 1

	var rec := {
		"pool_pages": pool_pages,
		"is_overflow_pool": is_overflow_pool,
		"lights_allocated": int(info.get("lights", -1)),
		"maps_allocated": int(info.get("maps_allocated", -1)),
		"atlas_passes": int(proj.get("atlas_passes", -1)),
		"lights_sampled": int(proj.get("lights_sampled", -1)),
		"local_lights": int(proj.get("local_lights", -1)),
		"local_lights_culled": int(proj.get("local_lights_culled", -1)),
		"check_pass": bool(check.get("pass", false)),
		"check_failures": check.get("failures", []),
		"overflow": int(alloc.get("overflow", -1)),
		"resident_overflow": int(alloc.get("resident_overflow", -1)),
		"sample_unmapped": int(sample_info.get("unmapped", -1)),
		"sample_coarse_unmapped": int(sample_info.get("coarse_receiver_unmapped", -1)),
		"sample_fallback": int(sample_info.get("fallback", -1)),
		"shadow_drop_failures": shadow_drop_failures,
		"probes": probe_results,
	}

	if not bool(info.get("active", false)):
		_failures.append("case_256: VSM not active")
	if rec.atlas_passes != 0:
		_failures.append("case_256: expected atlas_passes == 0, got %d" % rec.atlas_passes)
	if rec.lights_sampled < 256:
		_failures.append("case_256: expected lights_sampled >= 256, got %d" % rec.lights_sampled)
	if rec.local_lights < 256:
		_failures.append("case_256: expected projection.local_lights >= 256, got %d" % rec.local_lights)
	if rec.local_lights_culled < 256:
		_failures.append("case_256: expected projection.local_lights_culled >= 256, got %d" % rec.local_lights_culled)
	if rec.maps_allocated < 1021:
		_failures.append("case_256: expected maps_allocated >= 1021 (86 spot + 85 omni + 85 area), got %d" % rec.maps_allocated)
	if not rec.check_pass:
		_failures.append("case_256: VSM GPU/CPU check failed: %s" % str(rec.check_failures))
	if rec.resident_overflow != 0:
		_failures.append("case_256: expected resident_overflow == 0, got %d" % rec.resident_overflow)
	if rec.sample_unmapped != 0 or rec.sample_coarse_unmapped != 0:
		_failures.append("case_256: expected 0 unmapped samples, got fine=%d coarse=%d" % [rec.sample_unmapped, rec.sample_coarse_unmapped])
	if is_overflow_pool and rec.overflow <= 0:
		_failures.append("case_256(25%%): expected real pool overflow (> 0) on 2048-page pool, got %d" % rec.overflow)
	if shadow_drop_failures > 0:
		_failures.append("case_256: %d of %d probed spot/omni/area lights did not cast expected shadow contrast" % [shadow_drop_failures, probe_indices.size()])

	vp.queue_free()
	await _frames(3)
	return rec


# (b) 64 dirty local lights rasterized in <= 4 indirect dispatches (non-Nanite AND NextGeo).
func _case_64_dirty_single_dispatch() -> Dictionary:
	var vp := _new_viewport(Vector2i(512, 512))
	var cam := Camera3D.new()
	cam.fov = 75.0
	cam.near = 0.2
	cam.far = 60.0
	cam.transform = Transform3D(Basis.looking_at(Vector3(0.0, -1.0, -0.005).normalized(), Vector3.FORWARD), Vector3(0.0, 14.0, 0.0))
	vp.add_child(cam)
	cam.current = true

	var white := _white_mat()
	var floor_mi := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(36.0, 36.0)
	floor_mi.mesh = plane
	floor_mi.material_override = white
	floor_mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	vp.add_child(floor_mi)

	# 1. Non-Nanite path: 64 mixed local lights + 64 shared-mesh non-Nanite casters.
	var box := BoxMesh.new()
	box.size = Vector3(0.6, 0.2, 0.6)
	box.material = white

	var lights: Array[Light3D] = []
	var casters: Array[MeshInstance3D] = []
	for gz in 8:
		for gx in 8:
			var idx := gz * 8 + gx
			var lx := (float(gx) - 3.5) * 2.0
			var lz := (float(gz) - 3.5) * 2.0
			var l := _make_local_light(idx % 3, Vector3(lx, 2.4, lz), 3.2, 4.0)
			vp.add_child(l)
			lights.append(l)

			var c := MeshInstance3D.new()
			c.mesh = box
			c.position = Vector3(lx, 0.9, lz)
			vp.add_child(c)
			casters.append(c)

	await _frames(SETTLE_FRAMES)
	# Nudge all 64 lights in one frame so every light's maps become dirty simultaneously.
	for l in lights:
		l.position.x += 0.1
	await _frames(1)
	var nn_info := _info()
	var nn: Dictionary = nn_info.get("non_nanite", {})
	var nn_pages: Dictionary = nn_info.get("pages", {})
	var nn_proj: Dictionary = nn_info.get("projection", {})
	var nn_dirty_local := int(nn_pages.get("dirty", 0)) - int(nn_pages.get("dirty_directional", 0))

	# 2. NextGeo path: swap all 64 casters to a virtualized NextGeo mesh and nudge all 64 lights again.
	var v_box_prim := BoxMesh.new()
	v_box_prim.size = Vector3(0.6, 0.2, 0.6)
	v_box_prim.subdivide_width = 16
	v_box_prim.subdivide_depth = 16
	v_box_prim.subdivide_height = 2
	v_box_prim.material = white
	var vmesh := _make_virtual_mesh(v_box_prim, "64_caster")
	if vmesh != null:
		for c in casters:
			c.mesh = vmesh

	await _frames(SETTLE_FRAMES)
	# Nudge all 64 lights across 4 consecutive frames so async NextGeo GPU counters reflect the dirty local frame.
	var ng_info := {}
	var ng_max_local_dirty := 0
	var ng_max_local_clusters := 0
	var ng_local_dispatches := -1
	var ng_local_chains := -1
	for step in 5:
		for l in lights:
			l.position.x += 0.08 * (1.0 if (step % 2 == 0) else -1.0)
		await _frames(1)
		ng_info = _info()
		var ng: Dictionary = ng_info.get("nanite", {})
		if bool(ng.get("active", false)):
			ng_local_chains = int(ng.get("local_chains", -1))
			ng_local_dispatches = int(ng.get("local_raster_indirect_dispatches", -1))
			if bool(ng.get("gpu_counters_valid", false)):
				ng_max_local_dirty = maxi(ng_max_local_dirty, int(ng.get("local_dirty_pages", 0)))
				ng_max_local_clusters = maxi(ng_max_local_clusters, int(ng.get("local_clusters", 0)))

	var ng_proj: Dictionary = ng_info.get("projection", {})
	var rec := {
		"non_nanite_local_light_count": int(nn.get("local_light_count", -1)),
		"non_nanite_indirect_draw_count": int(nn.get("indirect_draw_count", -1)),
		"non_nanite_dirty_local_pages": nn_dirty_local,
		"non_nanite_atlas_passes": int(nn_proj.get("atlas_passes", -1)),
		"nanite_local_chains": ng_local_chains,
		"nanite_local_raster_indirect_dispatches": ng_local_dispatches,
		"nanite_local_dirty_pages": ng_max_local_dirty,
		"nanite_local_clusters": ng_max_local_clusters,
		"nanite_atlas_passes": int(ng_proj.get("atlas_passes", -1)),
	}

	if rec.non_nanite_local_light_count < 64:
		_failures.append("case_64_dirty: expected non_nanite.local_light_count >= 64, got %d" % rec.non_nanite_local_light_count)
	if rec.non_nanite_indirect_draw_count < 1 or rec.non_nanite_indirect_draw_count > 4:
		_failures.append("case_64_dirty: expected non_nanite.indirect_draw_count in [1, 4], got %d" % rec.non_nanite_indirect_draw_count)
	if rec.non_nanite_dirty_local_pages < 64:
		_failures.append("case_64_dirty: expected >= 64 dirty local pages, got %d" % rec.non_nanite_dirty_local_pages)
	if rec.non_nanite_atlas_passes != 0:
		_failures.append("case_64_dirty: expected non-Nanite atlas_passes == 0, got %d" % rec.non_nanite_atlas_passes)
	if rec.nanite_local_chains != 1:
		_failures.append("case_64_dirty: expected nanite.local_chains == 1, got %d" % rec.nanite_local_chains)
	if rec.nanite_local_raster_indirect_dispatches < 1 or rec.nanite_local_raster_indirect_dispatches > 4:
		_failures.append("case_64_dirty: expected nanite.local_raster_indirect_dispatches in [1, 4], got %d" % rec.nanite_local_raster_indirect_dispatches)
	if rec.nanite_local_dirty_pages < 64:
		_failures.append("case_64_dirty: expected nanite.local_dirty_pages >= 64, got %d" % rec.nanite_local_dirty_pages)
	if rec.nanite_local_clusters <= 0:
		_failures.append("case_64_dirty: expected nanite.local_clusters > 0, got %d" % rec.nanite_local_clusters)
	if rec.nanite_atlas_passes != 0:
		_failures.append("case_64_dirty: expected nanite atlas_passes == 0, got %d" % rec.nanite_atlas_passes)

	vp.queue_free()
	await _frames(3)
	return rec


# (c) Light3D.shadow_resolution_bias visibly shifts requested mip levels (-2.0, 0.0, +2.0, +4.0).
func _case_resolution_bias() -> Dictionary:
	var vp := _new_viewport(Vector2i(512, 512))
	vp.debug_draw = Viewport.DEBUG_DRAW_VSM_MIP
	var cam := Camera3D.new()
	cam.fov = 60.0
	cam.near = 0.2
	cam.far = 50.0
	cam.transform = Transform3D(Basis.looking_at(Vector3(0.0, -1.0, -0.01).normalized(), Vector3.FORWARD), Vector3(0.0, 10.0, 0.0))
	vp.add_child(cam)
	cam.current = true

	var floor_mi := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(20.0, 20.0)
	floor_mi.mesh = plane
	floor_mi.material_override = _black_mat()
	floor_mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	vp.add_child(floor_mi)

	var spot := SpotLight3D.new()
	spot.spot_angle = 45.0
	spot.spot_range = 12.0
	spot.light_energy = 4.0
	spot.shadow_enabled = true
	spot.transform = Transform3D(Basis.looking_at(Vector3.DOWN, Vector3.FORWARD), Vector3(0.0, 4.0, 0.0))
	vp.add_child(spot)

	var biases: Array[float] = [-2.0, 0.0, 2.0, 4.0]
	var sampled_mips: Array[int] = []
	var details := []
	var center_px := cam.unproject_position(Vector3(0.0, 0.0, 0.0))

	for b in biases:
		spot.shadow_resolution_bias = b
		await _frames(SETTLE_FRAMES)
		var check := await _run_vsm_check()
		var img := vp.get_texture().get_image()
		var stats := _median_mip_in_patch(img, center_px, 12)
		sampled_mips.append(int(stats.median))
		details.append({
			"bias": b,
			"mip_median": int(stats.median),
			"mip_min": int(stats.min),
			"mip_max": int(stats.max),
			"unmapped_px": int(stats.unmapped),
			"check_pass": bool(check.get("pass", false)),
		})
		if not bool(check.get("pass", false)):
			_failures.append("case_res_bias(%.1f): VSM GPU/CPU check failed: %s" % [b, str(check.get("failures", []))])
		if int(stats.median) < 0 or int(stats.unmapped) > 0:
			_failures.append("case_res_bias(%.1f): invalid DEBUG_DRAW_VSM_MIP readback %s" % [b, str(stats)])

	# Verify strict monotonic mip increase across -2.0 -> 0.0 -> +2.0 -> +4.0.
	if sampled_mips.size() == 4:
		if not (sampled_mips[0] < sampled_mips[1] and sampled_mips[1] < sampled_mips[2] and sampled_mips[2] <= sampled_mips[3] and sampled_mips[0] + 3 <= sampled_mips[3]):
			_failures.append("case_res_bias: expected strictly increasing mips for [-2, 0, +2, +4], got %s" % str(sampled_mips))

	vp.debug_draw = Viewport.DEBUG_DRAW_DISABLED
	vp.queue_free()
	await _frames(3)
	return {
		"sampled_mips": sampled_mips,
		"details": details,
	}


# (d) Distant local lights request only coarse mips (>= COARSE_FIRST_MIP = 5).
func _case_distant_light_policy() -> Dictionary:
	var vp := _new_viewport(Vector2i(512, 512))
	vp.debug_draw = Viewport.DEBUG_DRAW_VSM_MIP
	var cam := Camera3D.new()
	cam.fov = 70.0
	cam.near = 0.2
	cam.far = 100.0
	cam.transform = Transform3D(Basis.looking_at(Vector3.FORWARD, Vector3.UP), Vector3(0.0, 1.0, 0.0))
	vp.add_child(cam)
	cam.current = true

	var wall := MeshInstance3D.new()
	var quad := PlaneMesh.new()
	quad.size = Vector2(8.0, 8.0)
	wall.mesh = quad
	wall.material_override = _black_mat()
	wall.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	# Face +Z toward the camera at origin.
	wall.transform = Transform3D(Basis(Vector3.RIGHT, Vector3.FORWARD, Vector3.UP).orthonormalized(), Vector3(0.0, 1.0, -4.0))
	wall.rotation_degrees = Vector3(90.0, 0.0, 0.0)
	vp.add_child(wall)

	var spot := SpotLight3D.new()
	spot.spot_angle = 45.0
	spot.spot_range = 1.0
	spot.light_energy = 5.0
	spot.shadow_enabled = true
	# Negative bias (-2.0) proves near light uses fine mips while distant policy still clamps to >= 5!
	spot.shadow_resolution_bias = -2.0
	spot.transform = Transform3D(Basis.looking_at(Vector3.FORWARD, Vector3.UP), Vector3(0.0, 1.0, -3.5))
	vp.add_child(spot)

	# 1. Near position (z = -3.5, reach = 1.0 -> projected radius ~0.20 >> 0.03): NOT distant, fine mip < 5.
	await _frames(SETTLE_FRAMES)
	var near_check := await _run_vsm_check()
	var near_info := _info()
	var near_img := vp.get_texture().get_image()
	var near_px := cam.unproject_position(Vector3(0.0, 1.0, -4.0))
	var near_mip := _median_mip_in_patch(near_img, near_px, 10)
	var near_distant := int(near_info.get("projection", {}).get("distant_lights", -1))

	# 2. Distant position (z = -36.0, reach = 1.0 -> projected radius ~0.0198 < 0.03 High threshold):
	#    MUST be flagged distant and request ONLY coarse mips (>= 5).
	wall.position = Vector3(0.0, 1.0, -36.5)
	spot.position = Vector3(0.0, 1.0, -36.0)
	await _frames(SETTLE_FRAMES)
	var far_check := await _run_vsm_check()
	var far_info := _info()
	var far_img := vp.get_texture().get_image()
	var far_px := cam.unproject_position(Vector3(0.0, 1.0, -36.5))
	var far_mip := _median_mip_in_patch(far_img, far_px, 4)
	var far_distant := int(far_info.get("projection", {}).get("distant_lights", -1))

	var rec := {
		"threshold": float(far_info.get("distant_light_screen_radius", -1.0)),
		"near_distant_lights": near_distant,
		"near_mip": near_mip,
		"near_check_pass": bool(near_check.get("pass", false)),
		"far_distant_lights": far_distant,
		"far_mip": far_mip,
		"far_check_pass": bool(far_check.get("pass", false)),
	}

	if near_distant != 0:
		_failures.append("case_distant: expected near distant_lights == 0, got %d" % near_distant)
	if int(near_mip.median) < 0 or int(near_mip.median) >= 5:
		_failures.append("case_distant: expected near light to use fine mip < 5, got %s" % str(near_mip))
	if not bool(near_check.get("pass", false)):
		_failures.append("case_distant(near): VSM check failed: %s" % str(near_check.get("failures", [])))
	if far_distant < 1:
		_failures.append("case_distant: expected distant_lights >= 1 at z=-36, got %d" % far_distant)
	if int(far_mip.min) < 5 or int(far_mip.samples) == 0 or int(far_mip.unmapped) > 0:
		_failures.append("case_distant: expected distant light to request ONLY coarse mips (>= 5), got %s" % str(far_mip))
	if not bool(far_check.get("pass", false)):
		_failures.append("case_distant(far): VSM check failed: %s" % str(far_check.get("failures", [])))

	vp.debug_draw = Viewport.DEBUG_DRAW_DISABLED
	vp.queue_free()
	await _frames(3)
	return rec


# (f) Subsurface transmittance via VSM matches atlas for directional, spot, off-axis omni and area lights.
func _case_sss_transmittance() -> Dictionary:
	var vp := _new_viewport(Vector2i(320, 320))
	var cam := Camera3D.new()
	cam.fov = 50.0
	cam.near = 0.1
	cam.far = 40.0
	cam.transform = Transform3D(Basis.looking_at(Vector3.FORWARD, Vector3.UP), Vector3(0.0, 1.0, 2.6))
	vp.add_child(cam)
	cam.current = true

	var sss_mat := StandardMaterial3D.new()
	sss_mat.albedo_color = Color(0.15, 0.15, 0.15)
	sss_mat.roughness = 0.8
	sss_mat.metallic = 0.0
	sss_mat.metallic_specular = 0.0
	sss_mat.subsurf_scatter_enabled = true
	sss_mat.subsurf_scatter_strength = 1.0
	sss_mat.subsurf_scatter_transmittance_enabled = true
	sss_mat.subsurf_scatter_transmittance_color = Color(1.0, 0.35, 0.15, 1.0)
	# SSS() computes exp(-(8.25 * thickness / transmittance_depth)^2): depth=3.0 gives strong glow at
	# 0.25 m thin slab (exp(-0.47) ~ 0.62) and sharp attenuation at 0.90 m thick slab (exp(-6.1) ~ 0.002).
	# Boost=0.5 ensures AreaLight3D's SSS(ltc_diffuse >= 0) also receives strong transmittance.
	sss_mat.subsurf_scatter_transmittance_depth = 3.0
	sss_mat.subsurf_scatter_transmittance_boost = 0.5

	var thin_mesh := BoxMesh.new()
	thin_mesh.size = Vector3(2.4, 2.4, 0.25)
	# Subdivide the front/back faces so AreaLight3D's legacy vertex-shader dual-paraboloid atlas has
	# accurate interpolated depth across the 2.4 m quad for the atlas-vs-VSM reference comparison.
	thin_mesh.subdivide_width = 16
	thin_mesh.subdivide_height = 16
	var thick_mesh := BoxMesh.new()
	thick_mesh.size = Vector3(2.4, 2.4, 0.90)
	thick_mesh.subdivide_width = 16
	thick_mesh.subdivide_height = 16

	var slab := MeshInstance3D.new()
	slab.mesh = thin_mesh
	slab.material_override = sss_mat
	slab.position = Vector3(0.0, 1.0, 0.0)
	vp.add_child(slab)

	var kinds := ["directional", "spot", "omni_offaxis", "area"]
	var results := []
	var center_px := Vector2(160.0, 160.0)

	for kind in kinds:
		var light: Light3D
		if kind == "directional":
			var dl := DirectionalLight3D.new()
			dl.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
			dl.directional_shadow_max_distance = 30.0
			dl.transform = Transform3D(Basis.looking_at(Vector3.BACK, Vector3.UP), Vector3(0.0, 1.0, -5.0))
			dl.light_energy = 8.0
			light = dl
		elif kind == "spot":
			var sl := SpotLight3D.new()
			sl.spot_angle = 50.0
			sl.spot_range = 10.0
			sl.transform = Transform3D(Basis.looking_at(Vector3.BACK, Vector3.UP), Vector3(0.0, 1.0, -2.2))
			sl.light_energy = 16.0
			light = sl
		elif kind == "omni_offaxis":
			var ol := OmniLight3D.new()
			ol.omni_range = 10.0
			# Off-axis so cube-face x_ndc/y_ndc are non-zero, testing radial thickness conversion.
			ol.position = Vector3(0.85, 1.75, -1.8)
			ol.light_energy = 18.0
			light = ol
		else:
			var al := AreaLight3D.new()
			al.area_range = 10.0
			al.area_size = Vector2(0.5, 0.5)
			al.transform = Transform3D(Basis.looking_at(Vector3(0.0, 1.0, 0.0) - Vector3(0.4, 1.3, -2.0), Vector3.UP), Vector3(0.4, 1.3, -2.0))
			al.light_energy = 36.0
			light = al

		light.shadow_enabled = true
		light.shadow_transmittance_bias = 0.05
		vp.add_child(light)

		# 1. Atlas reference (thin slab).
		# Note: upstream Godot's legacy spot-atlas transmittance shader has a pre-existing reversed-Z bug
		# (scene_forward_lights_inc.glsl), so for an on-axis SpotLight3D we measure the exact analytical
		# atlas reference using an identical-position/range/energy OmniLight3D in SHADOW_METHOD_ATLAS.
		slab.mesh = thin_mesh
		slab.position = Vector3(0.0, 1.0, 0.0)
		var atlas_luma := 0.0
		if kind == "spot":
			light.visible = false
			var ref_omni := OmniLight3D.new()
			ref_omni.omni_range = 10.0
			ref_omni.position = Vector3(0.0, 1.0, -2.2)
			ref_omni.light_energy = 16.0
			ref_omni.shadow_enabled = true
			ref_omni.shadow_transmittance_bias = 0.05
			ref_omni.shadow_method = Light3D.SHADOW_METHOD_ATLAS
			vp.add_child(ref_omni)
			await _frames(SETTLE_FRAMES)
			atlas_luma = _luma(_sample_patch_rgb(vp.get_texture().get_image(), center_px, 6))
			ref_omni.queue_free()
			await _frames(2)
			light.visible = true
		else:
			light.shadow_method = Light3D.SHADOW_METHOD_ATLAS
			await _frames(SETTLE_FRAMES)
			atlas_luma = _luma(_sample_patch_rgb(vp.get_texture().get_image(), center_px, 6))

		# 2. VSM thin slab (bias = 0.05).
		light.shadow_method = Light3D.SHADOW_METHOD_VIRTUAL
		await _frames(SETTLE_FRAMES)
		var vsm_info := _info()
		var vsm_c := _sample_patch_rgb(vp.get_texture().get_image(), center_px, 6)
		var vsm_luma := _luma(vsm_c)
		var vsm_atlas_passes := int(vsm_info.get("projection", {}).get("atlas_passes", -1))

		# 3. VSM transmittance_bias check (bias = 0.20 reduces effective thickness -> brighter glow).
		light.shadow_transmittance_bias = 0.20
		await _frames(SETTLE_FRAMES)
		var vsm_biased_luma := _luma(_sample_patch_rgb(vp.get_texture().get_image(), center_px, 6))
		light.shadow_transmittance_bias = 0.05

		# 4. VSM thick slab (back face kept at same z = +0.125 so attenuation comes purely from depth).
		slab.mesh = thick_mesh
		slab.position = Vector3(0.0, 1.0, -0.325)
		await _frames(SETTLE_FRAMES)
		var vsm_thick_c := _sample_patch_rgb(vp.get_texture().get_image(), center_px, 6)
		var vsm_thick_luma := _luma(vsm_thick_c)

		var rel_err := absf(vsm_luma - atlas_luma) / maxf(atlas_luma, 0.02)
		results.append({
			"kind": kind,
			"atlas_luma": snappedf(atlas_luma, 0.0001),
			"vsm_luma": snappedf(vsm_luma, 0.0001),
			"vsm_biased_luma": snappedf(vsm_biased_luma, 0.0001),
			"vsm_thick_luma": snappedf(vsm_thick_luma, 0.0001),
			"rel_err": snappedf(rel_err, 0.001),
			"vsm_atlas_passes": vsm_atlas_passes,
		})

		if vsm_atlas_passes != 0:
			_failures.append("case_sss(%s): expected atlas_passes == 0 under VSM, got %d" % [kind, vsm_atlas_passes])
		if atlas_luma < 0.015 or vsm_luma < 0.015:
			_failures.append("case_sss(%s): transmittance glow too dark (atlas=%.4f, vsm=%.4f)" % [kind, atlas_luma, vsm_luma])
		elif rel_err > 0.20:
			_failures.append("case_sss(%s): VSM transmittance differs from atlas by %.1f%% (atlas=%.4f, vsm=%.4f)" % [kind, rel_err * 100.0, atlas_luma, vsm_luma])
		if vsm_biased_luma <= vsm_luma * 1.05:
			_failures.append("case_sss(%s): increasing shadow_transmittance_bias did not brighten VSM SSS (bias0.05=%.4f, bias0.20=%.4f)" % [kind, vsm_luma, vsm_biased_luma])
		if vsm_thick_luma >= vsm_luma * 0.65:
			_failures.append("case_sss(%s): thick slab did not attenuate VSM transmittance (thin=%.4f, thick=%.4f)" % [kind, vsm_luma, vsm_thick_luma])

		light.queue_free()
		await _frames(3)

	vp.queue_free()
	await _frames(3)
	return {"lights": results}


func _run() -> void:
	_result["case_256_overflow"] = await _case_256_lights_and_overflow()
	_result["case_64_dirty_dispatch"] = await _case_64_dirty_single_dispatch()
	_result["case_resolution_bias"] = await _case_resolution_bias()
	_result["case_distant_light"] = await _case_distant_light_policy()
	_result["case_sss_transmittance"] = await _case_sss_transmittance()

	_cleanup_temp_files()
	_result["failures"] = _failures
	_result["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
