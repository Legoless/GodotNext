extends Node3D
## Streaming texture swap regression check (#305).
##
## Exercises live RenderingServer.texture_replace_compatible() (including a chained double-swap in one
## frame) and RenderingServer.texture_replace() while all MaterialData::texture_cache consumers are active:
## - #155 programmable raster bins (both HW and SW compute paths, StandardMaterial3D alpha-scissor and
##   custom ShaderMaterial masked sampler2D): verifies the alpha mask flips cleanly right after the swap
##   with 0 invalid-RID errors.
## - #158 material shading bins (Compute Bins): verifies albedo color updates on the very next frame with
##   0 tile-quad fallback frames.
## - #130 NextGI material hit groups (when HWRT + RT pipelines are active): verifies an already-tracked
##   material updates its bindless slot and matches fresh card captures after texture_replace_compatible(),
##   a pre-existing material entering #130 after its texture was swapped (and its old RD texture freed)
##   allocates cleanly, and non-compatible texture_replace() while pinned in #130 does not deadlock.
## - #145 shadow policy bindless alpha table: shares the bindless texture table with #130 while an
##   alpha-tested shadow caster's albedo texture is swapped across multiple frames.
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const ProbeScript = preload("res://tests/nextgeo_sw_raster/nextgeo_sw_raster_probe.gd")
const HIT_CHECK_SETTING := "rendering/global_illumination/nextgi/debug/hit_material_check"
const MIN_HIT_MATCH := 0.999
const MAX_HIT_WAIT_FRAMES := 900

const MASKED_TEX_SHADER := """
shader_type spatial;
uniform sampler2D mask_tex : source_color, filter_nearest;
void fragment() {
	vec4 c = texture(mask_tex, UV);
	ALBEDO = c.rgb;
	ALPHA = c.a;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""

const HIT_TEX_SHADER := """
shader_type spatial;
uniform sampler2D albedo_tex : source_color, filter_linear_mipmap;
void fragment() {
	ALBEDO = texture(albedo_tex, UV).rgb;
	ROUGHNESS = 0.7;
	METALLIC = 0.0;
}
"""

var _probe: CompositorEffect
var _expect_bins := false
var _expect_hit_groups := false
var _failures: Array[String] = []
var _report: Dictionary = {}

# Scene instances and materials.
var _background: MeshInstance3D
var _raster_std_inst: MeshInstance3D
var _raster_shader_inst: MeshInstance3D
var _shading_box_inst: MeshInstance3D
var _hit_active_inst: MeshInstance3D
var _hit_shader_inst: MeshInstance3D
var _hit_late_inst: MeshInstance3D
var _hit_replace_inst: MeshInstance3D

var _panel_mesh: ArrayMesh
var _hit_active_mat: StandardMaterial3D
var _hit_shader_mat: ShaderMaterial
var _hit_late_mat: StandardMaterial3D
var _hit_replace_mat: StandardMaterial3D

# Textures kept alive for their RIDs.
var _tex_raster_std: ImageTexture
var _tex_raster_shader: ImageTexture
var _tex_shade_albedo: ImageTexture
var _tex_shade_normal: ImageTexture
var _tex_hit_active: ImageTexture
var _tex_hit_normal: ImageTexture
var _tex_hit_shader: ImageTexture
var _tex_hit_late: ImageTexture
var _tex_hit_replace: ImageTexture


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", true)
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 0.0)
	ProjectSettings.set_setting("rendering/nextgeo/shading_path", 1) # Compute Bins.
	ProjectSettings.set_setting(HIT_CHECK_SETTING, false)

	var rd := RenderingServer.get_rendering_device()
	var int64 := rd != null and (rd.has_feature(RenderingDevice.SUPPORTS_IMAGE_INT64_ATOMICS) or rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_INT64_ATOMICS))
	var indirect_count := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT)
	_expect_bins = int64 and indirect_count

	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 0.5

	_build_scene()
	_run()


func _check(p_ok: bool, p_msg: String) -> void:
	if not p_ok:
		_failures.append(p_msg)


func _virtualize(p_mesh: PrimitiveMesh, p_ngm_path: String) -> Mesh:
	var importer := ImporterMesh.from_mesh(p_mesh)
	var err := importer.build_virtual_geometry(p_ngm_path)
	var mesh := importer.get_mesh()
	if err != OK or mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(%s) failed (err=%d)" % [p_ngm_path, err])
	return mesh


func _solid_image(p_color: Color, p_size := 64) -> Image:
	var img := Image.create(p_size, p_size, false, Image.FORMAT_RGBA8)
	img.fill(p_color)
	img.generate_mipmaps()
	return img


func _half_mask_image(p_horizontal: bool, p_second_half: bool, p_color: Color, p_size := 64) -> Image:
	var img := Image.create(p_size, p_size, false, Image.FORMAT_RGBA8)
	var half := p_size / 2
	for y in p_size:
		for x in p_size:
			var coord := x if p_horizontal else y
			var opaque := (coord >= half) if p_second_half else (coord < half)
			img.set_pixel(x, y, Color(p_color.r, p_color.g, p_color.b, 1.0 if opaque else 0.0))
	img.generate_mipmaps()
	return img


func _normal_image(p_tilt_x: float, p_size := 64) -> Image:
	var img := Image.create(p_size, p_size, false, Image.FORMAT_RGBA8)
	var n := Vector3(p_tilt_x, 0.0, 1.0).normalized()
	img.fill(Color(n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, n.z * 0.5 + 0.5, 1.0))
	img.generate_mipmaps()
	return img


func _make_panel_mesh() -> ArrayMesh:
	var plane := PlaneMesh.new()
	plane.size = Vector2(1.0, 1.0)
	plane.subdivide_width = 2
	plane.subdivide_depth = 2
	plane.orientation = PlaneMesh.FACE_Z
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, plane.get_mesh_arrays())
	return mesh


func _build_scene() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.08, 0.1, 0.14)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.35, 0.35, 0.35)
	env.ambient_light_energy = 1.0
	env.nextgi_enabled = true

	var compositor := Compositor.new()
	_probe = ProbeScript.new()
	compositor.compositor_effects = [_probe]

	var world := WorldEnvironment.new()
	world.environment = env
	world.compositor = compositor
	add_child(world)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-25.0, 15.0, 0.0)
	sun.light_energy = 1.5
	sun.shadow_enabled = true
	add_child(sun)

	var cam := Camera3D.new()
	cam.fov = 55.0
	cam.near = 0.1
	cam.far = 100.0
	cam.position = Vector3(0.0, 0.0, 5.2)
	cam.current = true
	add_child(cam)
	cam.look_at(Vector3.ZERO)

	# Virtualized meshes for #155 and #158.
	var bg_box := BoxMesh.new()
	bg_box.size = Vector3(1.0, 1.0, 1.0)
	var bg_mat := StandardMaterial3D.new()
	bg_mat.albedo_color = Color(0.25, 0.25, 0.28)
	bg_box.material = bg_mat
	var bg_mesh := _virtualize(bg_box, "user://p305_bg_box.ngm")
	_background = MeshInstance3D.new()
	_background.name = "Background"
	_background.mesh = bg_mesh
	_background.transform = Transform3D(Basis.from_scale(Vector3(14.0, 9.0, 0.4)), Vector3(0.0, 0.0, -2.5))
	add_child(_background)

	var vplane := PlaneMesh.new()
	vplane.size = Vector2(1.4, 1.4)
	vplane.subdivide_width = 24
	vplane.subdivide_depth = 24
	var vplane_mesh := _virtualize(vplane, "user://p305_plane.ngm")

	# #155 + #145 + #130: StandardMaterial3D alpha-scissor plane with albedo_texture (left half opaque).
	_tex_raster_std = ImageTexture.create_from_image(_half_mask_image(true, false, Color(0.9, 0.8, 0.2)))
	var raster_std_mat := StandardMaterial3D.new()
	raster_std_mat.albedo_texture = _tex_raster_std
	raster_std_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	raster_std_mat.alpha_scissor_threshold = 0.5
	raster_std_mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST_WITH_MIPMAPS
	_raster_std_inst = MeshInstance3D.new()
	_raster_std_inst.name = "RasterStdPlane"
	_raster_std_inst.mesh = vplane_mesh
	_raster_std_inst.material_override = raster_std_mat
	_raster_std_inst.transform = Transform3D(Basis(Vector3.RIGHT, deg_to_rad(90.0)), Vector3(-1.8, 1.0, 0.0))
	add_child(_raster_std_inst)

	# #155: Custom ShaderMaterial alpha-scissor plane with sampler2D (top half opaque).
	_tex_raster_shader = ImageTexture.create_from_image(_half_mask_image(false, false, Color(0.2, 0.85, 0.9)))
	var raster_shader := Shader.new()
	raster_shader.code = MASKED_TEX_SHADER
	var raster_shader_mat := ShaderMaterial.new()
	raster_shader_mat.shader = raster_shader
	raster_shader_mat.set_shader_parameter("mask_tex", _tex_raster_shader)
	_raster_shader_inst = MeshInstance3D.new()
	_raster_shader_inst.name = "RasterShaderPlane"
	_raster_shader_inst.mesh = vplane_mesh
	_raster_shader_inst.material_override = raster_shader_mat
	_raster_shader_inst.transform = Transform3D(Basis(Vector3.RIGHT, deg_to_rad(90.0)), Vector3(0.0, 1.0, 0.0))
	add_child(_raster_shader_inst)

	# #158: Textured + normal-mapped virtualized box for compute shading bins (initially RED).
	var sbox := BoxMesh.new()
	sbox.size = Vector3(1.2, 1.2, 1.2)
	sbox.subdivide_width = 8
	sbox.subdivide_height = 8
	sbox.subdivide_depth = 8
	_tex_shade_albedo = ImageTexture.create_from_image(_solid_image(Color(0.9, 0.12, 0.1)))
	_tex_shade_normal = ImageTexture.create_from_image(_normal_image(0.15))
	var shade_mat := StandardMaterial3D.new()
	shade_mat.albedo_texture = _tex_shade_albedo
	shade_mat.normal_enabled = true
	shade_mat.normal_texture = _tex_shade_normal
	shade_mat.roughness = 0.7
	sbox.material = shade_mat
	var sbox_mesh := _virtualize(sbox, "user://p305_shade_box.ngm")
	_shading_box_inst = MeshInstance3D.new()
	_shading_box_inst.name = "ShadingBox"
	_shading_box_inst.mesh = sbox_mesh
	_shading_box_inst.transform = Transform3D(Basis(), Vector3(1.8, 1.0, 0.0))
	add_child(_shading_box_inst)

	# #130 panels (non-virtual ArrayMesh so attribute streams + hit groups shade them directly).
	_panel_mesh = _make_panel_mesh()

	_tex_hit_active = ImageTexture.create_from_image(_solid_image(Color(0.9, 0.12, 0.85)))
	_tex_hit_normal = ImageTexture.create_from_image(_normal_image(-0.15))
	_hit_active_mat = StandardMaterial3D.new()
	_hit_active_mat.albedo_texture = _tex_hit_active
	_hit_active_mat.normal_enabled = true
	_hit_active_mat.normal_texture = _tex_hit_normal
	_hit_active_mat.roughness = 0.65
	_hit_active_inst = _spawn_panel("HitActivePanel", Vector3(-1.8, -1.0, 0.0), _hit_active_mat)

	_tex_hit_shader = ImageTexture.create_from_image(_solid_image(Color(0.95, 0.55, 0.1)))
	var hit_sh := Shader.new()
	hit_sh.code = HIT_TEX_SHADER
	_hit_shader_mat = ShaderMaterial.new()
	_hit_shader_mat.shader = hit_sh
	_hit_shader_mat.set_shader_parameter("albedo_tex", _tex_hit_shader)
	_hit_shader_inst = _spawn_panel("HitShaderPanel", Vector3(-0.6, -1.0, 0.0), _hit_shader_mat)

	_tex_hit_late = ImageTexture.create_from_image(_solid_image(Color(0.3, 0.3, 0.95)))
	_hit_late_mat = StandardMaterial3D.new()
	_hit_late_mat.albedo_texture = _tex_hit_late
	_hit_late_mat.roughness = 0.65
	_hit_late_inst = _spawn_panel("HitLatePanel", Vector3(0.6, -1.0, 0.0), _hit_late_mat)

	_tex_hit_replace = ImageTexture.create_from_image(_solid_image(Color(0.15, 0.3, 0.92)))
	_hit_replace_mat = StandardMaterial3D.new()
	_hit_replace_mat.albedo_texture = _tex_hit_replace
	_hit_replace_mat.roughness = 0.65
	_hit_replace_inst = _spawn_panel("HitReplacePanel", Vector3(1.8, -1.0, 0.0), _hit_replace_mat)


func _spawn_panel(p_name: String, p_pos: Vector3, p_mat: Material, p_mesh: ArrayMesh = null) -> MeshInstance3D:
	var inst := MeshInstance3D.new()
	inst.name = p_name
	inst.mesh = p_mesh if p_mesh != null else _panel_mesh
	inst.material_override = p_mat
	inst.position = p_pos
	add_child(inst)
	return inst


func _wait_frames(p_count: int) -> void:
	for i in p_count:
		await RenderingServer.frame_post_draw


func _capture_probe() -> Dictionary:
	_probe.set("capture", {})
	_probe.set("capture_armed", true)
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	return _probe.get("capture")


func _record_index(p_instance: MeshInstance3D) -> int:
	var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(p_instance.get_instance())
	return int(st.get("record_index", -1))


func _surface_route(p_instance: MeshInstance3D) -> String:
	var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(p_instance.get_instance())
	var surfaces: Array = st.get("surfaces", [])
	if not surfaces.is_empty():
		return String(surfaces[0].get("route", ""))
	return String(st.get("route", ""))


# Counts pixels belonging to p_record_id in the first vs second half of the instance's projected screen box,
# skipping a 2 px band on the midpoint seam so integer projection rounding never straddles the cut.
func _split_coverage(p_capture: Dictionary, p_record_id: int, p_rect: Rect2i, p_horizontal: bool) -> Vector2i:
	var size: Vector2i = p_capture.get("size", Vector2i.ZERO)
	var words: PackedInt32Array = p_capture.get("words", PackedInt32Array())
	if size.x <= 0 or size.y <= 0 or words.is_empty() or p_record_id < 0:
		return Vector2i.ZERO
	var first := 0
	var second := 0
	var mid := (p_rect.position.x + p_rect.size.x / 2) if p_horizontal else (p_rect.position.y + p_rect.size.y / 2)
	var x0 := clampi(p_rect.position.x, 0, size.x)
	var y0 := clampi(p_rect.position.y, 0, size.y)
	var x1 := clampi(p_rect.end.x, 0, size.x)
	var y1 := clampi(p_rect.end.y, 0, size.y)
	for y in range(y0, y1):
		for x in range(x0, x1):
			var coord := x if p_horizontal else y
			if absi(coord - mid) < 2:
				continue
			var base := (y * size.x + x) * 5
			if words[base] == p_record_id:
				if coord > mid:
					second += 1
				else:
					first += 1
	return Vector2i(first, second)


# Projects a 3D box around p_center of half-extents p_half into pixel coordinates of p_size.
func _project_rect(p_center: Vector3, p_half: Vector3, p_size: Vector2i) -> Rect2i:
	var cam := get_viewport().get_camera_3d()
	var vp_size := Vector2(get_viewport().size)
	var scale := Vector2(float(p_size.x) / max(vp_size.x, 1.0), float(p_size.y) / max(vp_size.y, 1.0))
	var min_p := Vector2(1e9, 1e9)
	var max_p := Vector2(-1e9, -1e9)
	for sx in [-1.0, 1.0]:
		for sy in [-1.0, 1.0]:
			for sz in [-1.0, 1.0]:
				var corner := p_center + Vector3(sx * p_half.x, sy * p_half.y, sz * p_half.z)
				var sp := cam.unproject_position(corner) * scale
				min_p = min_p.min(sp)
				max_p = max_p.max(sp)
	return Rect2i(Vector2i(floori(min_p.x), floori(min_p.y)), Vector2i(ceili(max_p.x - min_p.x), ceili(max_p.y - min_p.y)))


func _mean_color_in_rect(p_image: Image, p_rect: Rect2i) -> Color:
	var w := p_image.get_width()
	var h := p_image.get_height()
	var x0 := clampi(p_rect.position.x + p_rect.size.x / 4, 0, w - 1)
	var x1 := clampi(p_rect.end.x - p_rect.size.x / 4, x0 + 1, w)
	var y0 := clampi(p_rect.position.y + p_rect.size.y / 4, 0, h - 1)
	var y1 := clampi(p_rect.end.y - p_rect.size.y / 4, y0 + 1, h)
	var sum := Color(0, 0, 0, 0)
	var n := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			sum += p_image.get_pixel(x, y)
			n += 1
	return sum / float(max(n, 1))


func _wait_hit_check(p_target_runs: int) -> Dictionary:
	var counters := {}
	for i in MAX_HIT_WAIT_FRAMES:
		await RenderingServer.frame_post_draw
		counters = RenderingServer.nextgi_get_counters()
		if int(counters.get("hit_check_runs", 0)) >= p_target_runs and int(counters.get("hit_groups_pending", 0)) == 0:
			break
	return counters


func _check_hit_counters(p_phase: String, p_counters: Dictionary, p_min_runs: int) -> Dictionary:
	var samples := int(p_counters.get("hit_check_samples", 0))
	var hits := int(p_counters.get("hit_check_hits", 0))
	var fallback := int(p_counters.get("hit_check_fallback", 0))
	var shaded := hits - fallback
	var matched_all := int(p_counters.get("hit_check_all", 0))
	var share_all := float(matched_all) / float(max(shaded, 1))
	var table_full := int(p_counters.get("hit_ineligible_table_full", 0))
	var runs := int(p_counters.get("hit_check_runs", 0))
	_check(runs >= p_min_runs, "%s: hit check did not advance (%d < %d)" % [p_phase, runs, p_min_runs])
	_check(shaded > 0, "%s: no shaded hit-group samples (%d hits, %d fallback)" % [p_phase, hits, fallback])
	_check(share_all >= MIN_HIT_MATCH, "%s: hit-group payload mismatch (share_all=%.5f, %d/%d)" % [p_phase, share_all, matched_all, shaded])
	_check(table_full == 0, "%s: bindless table reported TABLE_FULL (%d)" % [p_phase, table_full])
	return {
		"runs": runs,
		"samples": samples,
		"hits": hits,
		"fallback": fallback,
		"shaded": shaded,
		"share_all": share_all,
		"table_full": table_full,
	}


func _swap_compatible(p_tex: ImageTexture, p_new_image: Image) -> void:
	var new_rid := RenderingServer.texture_2d_create(p_new_image)
	RenderingServer.texture_replace_compatible(p_tex.get_rid(), new_rid)


func _swap_replace(p_tex: ImageTexture, p_new_image: Image) -> void:
	var new_rid := RenderingServer.texture_2d_create(p_new_image)
	RenderingServer.texture_replace(p_tex.get_rid(), new_rid)


func _run() -> void:
	# Warm up with HitLatePanel visible so its MaterialData::texture_cache is populated with old_rd_late
	# BEFORE hit groups turn on.
	await _wait_frames(12)
	_hit_late_inst.visible = false
	await _wait_frames(6)

	var tier: Dictionary = RenderingServer.nextgi_get_tier_info()
	var rd := RenderingServer.get_rendering_device()
	var rt_pipelines := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE) and rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE_LIBRARY) and rd.has_feature(RenderingDevice.SUPPORTS_BINDLESS)
	_expect_hit_groups = String(tier.get("tier", "")) == "HWRT" and String(tier.get("hardware_ray_tracing_mode", "")) == "FULL" and rt_pipelines

	_report["device"] = {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"tier": String(tier.get("tier", "")),
		"expect_bins": _expect_bins,
		"expect_hit_groups": _expect_hit_groups,
	}

	if _expect_hit_groups:
		ProjectSettings.set_setting(HIT_CHECK_SETTING, true)
		var pre_hit := await _wait_hit_check(1)
		_report["pre_swap_hit"] = _check_hit_counters("pre_swap_hit", pre_hit, 1)
	else:
		await _wait_frames(10)

	# Phase 1: Pre-swap baseline (#155 HW bins + #158 compute bins).
	var pre_cap := await _capture_probe()
	var pre_img := get_viewport().get_texture().get_image()
	var pre_vp_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var std_rec := _record_index(_raster_std_inst)
	var sh_rec := _record_index(_raster_shader_inst)
	var probe_size: Vector2i = pre_cap.get("size", Vector2i.ZERO)
	var std_rect := _project_rect(_raster_std_inst.position, Vector3(0.7, 0.7, 0.05), probe_size)
	var sh_rect := _project_rect(_raster_shader_inst.position, Vector3(0.7, 0.7, 0.05), probe_size)
	var shade_rect := _project_rect(_shading_box_inst.position, Vector3(0.6, 0.6, 0.6), pre_img.get_size())

	var pre_std_cov := _split_coverage(pre_cap, std_rec, std_rect, true)
	var pre_sh_cov := _split_coverage(pre_cap, sh_rec, sh_rect, false)
	var pre_shade_col := _mean_color_in_rect(pre_img, shade_rect)
	var pre_compute_b := int(pre_vp_stats.get("shading_stage_b_compute", 0))
	var pre_bins := int(pre_vp_stats.get("programmable_bins", 0))
	_report["pre_swap"] = {
		"std_route": _surface_route(_raster_std_inst),
		"shader_route": _surface_route(_raster_shader_inst),
		"programmable_bins": pre_bins,
		"std_cov": [pre_std_cov.x, pre_std_cov.y],
		"shader_cov": [pre_sh_cov.x, pre_sh_cov.y],
		"shading_stage_b_compute": pre_compute_b,
		"shade_color": [pre_shade_col.r, pre_shade_col.g, pre_shade_col.b],
	}
	if _expect_bins:
		_check(_surface_route(_raster_std_inst) == "virtual", "pre_swap: RasterStdPlane not virtual")
		_check(_surface_route(_raster_shader_inst) == "virtual", "pre_swap: RasterShaderPlane not virtual")
		_check(pre_bins >= 2, "pre_swap: expected >= 2 programmable bins, got %d" % pre_bins)
		_check(pre_std_cov.x > 50 and pre_std_cov.y == 0, "pre_swap: RasterStdPlane expected left-only coverage, got %s" % str(pre_std_cov))
		_check(pre_sh_cov.x > 50 and pre_sh_cov.y == 0, "pre_swap: RasterShaderPlane expected top-only coverage, got %s" % str(pre_sh_cov))
	_check(pre_compute_b >= 1, "pre_swap: expected compute shading bins active, got %d" % pre_compute_b)
	_check(pre_shade_col.r > pre_shade_col.b + 0.2, "pre_swap: ShadingBox expected red-dominant, got %s" % str(pre_shade_col))

	# Phase 2: Live streaming swaps (texture_replace_compatible) across ALL consumers!
	# - RasterStdPlane: flip left -> right half opaque (also updates #145 shadow caster + #130).
	_swap_compatible(_tex_raster_std, _half_mask_image(true, true, Color(0.9, 0.8, 0.2)))
	# - RasterShaderPlane: chained DOUBLE swap in the same frame (intermediate -> bottom half opaque).
	_swap_compatible(_tex_raster_shader, _half_mask_image(false, false, Color(0.6, 0.6, 0.6)))
	_swap_compatible(_tex_raster_shader, _half_mask_image(false, true, Color(0.2, 0.85, 0.9)))
	# - ShadingBox: RED -> BLUE albedo + normal map swap.
	_swap_compatible(_tex_shade_albedo, _solid_image(Color(0.1, 0.2, 0.92)))
	_swap_compatible(_tex_shade_normal, _normal_image(-0.15))
	# - HitActivePanel & HitShaderPanel (already tracked in #130): MAGENTA -> LIME, ORANGE -> CYAN.
	_swap_compatible(_tex_hit_active, _solid_image(Color(0.15, 0.9, 0.2)))
	_swap_compatible(_tex_hit_normal, _normal_image(0.2))
	_swap_compatible(_tex_hit_shader, _solid_image(Color(0.1, 0.85, 0.95)))
	# - HitLatePanel (currently hidden, NOT in #130 yet -> old_rd_late is immediately freed!).
	_swap_compatible(_tex_hit_late, _solid_image(Color(0.95, 0.9, 0.15)))

	# Check IMMEDIATELY on the very first rendered frame after swap: #158 compute bins must NOT drop
	# to tile quads, and #155 HW bins must immediately render the flipped masks.
	await RenderingServer.frame_post_draw
	var immediate_vp_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var immediate_compute_b := int(immediate_vp_stats.get("shading_stage_b_compute", 0))
	var post_hw_cap := await _capture_probe()
	var post_hw_img := get_viewport().get_texture().get_image()
	var post_hw_std_cov := _split_coverage(post_hw_cap, std_rec, std_rect, true)
	var post_hw_sh_cov := _split_coverage(post_hw_cap, sh_rec, sh_rect, false)
	var post_shade_col := _mean_color_in_rect(post_hw_img, shade_rect)
	_report["post_swap_hw"] = {
		"immediate_shading_stage_b_compute": immediate_compute_b,
		"std_cov": [post_hw_std_cov.x, post_hw_std_cov.y],
		"shader_cov": [post_hw_sh_cov.x, post_hw_sh_cov.y],
		"shade_color": [post_shade_col.r, post_shade_col.g, post_shade_col.b],
	}
	_check(immediate_compute_b >= pre_compute_b, "post_swap_hw: compute shading bins dropped on first frame after swap (%d < %d)" % [immediate_compute_b, pre_compute_b])
	_check(post_shade_col.b > post_shade_col.r + 0.2, "post_swap_hw: ShadingBox did not switch to blue after swap (%s)" % str(post_shade_col))
	if _expect_bins:
		_check(post_hw_std_cov.x == 0 and post_hw_std_cov.y > 50, "post_swap_hw: RasterStdPlane expected right-only coverage after swap, got %s" % str(post_hw_std_cov))
		_check(post_hw_sh_cov.x == 0 and post_hw_sh_cov.y > 50, "post_swap_hw: RasterShaderPlane expected bottom-only coverage after double-swap, got %s" % str(post_hw_sh_cov))

	# Phase 3: Switch to SW compute raster bins (T = 32), verify flipped masks, then swap LIVE in SW mode!
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 32.0)
	await _wait_frames(4)
	var post_sw_cap := await _capture_probe()
	var post_sw_std_cov := _split_coverage(post_sw_cap, std_rec, std_rect, true)
	var post_sw_sh_cov := _split_coverage(post_sw_cap, sh_rec, sh_rect, false)
	# Live swap while SW compute raster bins are active: flip RasterStdPlane right -> left.
	_swap_compatible(_tex_raster_std, _half_mask_image(true, false, Color(0.9, 0.8, 0.2)))
	await _wait_frames(2)
	var post_sw2_cap := await _capture_probe()
	var post_sw2_std_cov := _split_coverage(post_sw2_cap, std_rec, std_rect, true)
	_report["post_swap_sw"] = {
		"std_cov_before": [post_sw_std_cov.x, post_sw_std_cov.y],
		"shader_cov": [post_sw_sh_cov.x, post_sw_sh_cov.y],
		"std_cov_after": [post_sw2_std_cov.x, post_sw2_std_cov.y],
	}
	if _expect_bins:
		_check(post_sw_std_cov.x == 0 and post_sw_std_cov.y > 50, "post_swap_sw: RasterStdPlane expected right-only coverage on SW path, got %s" % str(post_sw_std_cov))
		_check(post_sw_sh_cov.x == 0 and post_sw_sh_cov.y > 50, "post_swap_sw: RasterShaderPlane expected bottom-only coverage on SW path, got %s" % str(post_sw_sh_cov))
		_check(post_sw2_std_cov.x > 50 and post_sw2_std_cov.y == 0, "post_swap_sw: RasterStdPlane did not flip back to left-only after live SW swap, got %s" % str(post_sw2_std_cov))

	# Phase 4: Verify #130 hit groups on BOTH already-active swapped materials AND newly-entering swapped
	# material (HitLatePanel whose old_rd_late was already freed). Give each fresh panel a fresh ArrayMesh
	# with the exact same Material RID so NextGISRC (which keys card sets by (mesh, material)) captures fresh
	# cards against the new textures while #130 reuses its tracked Material entries.
	remove_child(_hit_active_inst)
	_hit_active_inst.free()
	remove_child(_hit_shader_inst)
	_hit_shader_inst.free()
	remove_child(_hit_late_inst)
	_hit_late_inst.free()
	_hit_active_inst = _spawn_panel("HitActiveFresh", Vector3(-1.8, -1.0, 0.0), _hit_active_mat, _make_panel_mesh())
	_hit_shader_inst = _spawn_panel("HitShaderFresh", Vector3(-0.6, -1.0, 0.0), _hit_shader_mat, _make_panel_mesh())
	_hit_late_inst = _spawn_panel("HitLateFresh", Vector3(0.6, -1.0, 0.0), _hit_late_mat, _make_panel_mesh())
	await _wait_frames(10) # Let frame ring wrap so deferred bindless frees complete too.
	if _expect_hit_groups:
		var runs_now := int(RenderingServer.nextgi_get_counters().get("hit_check_runs", 0))
		var post_hit := await _wait_hit_check(runs_now + 2)
		_report["post_swap_hit"] = _check_hit_counters("post_swap_hit", post_hit, runs_now + 2)

	# Phase 5: Non-compatible RenderingServer.texture_replace() while pinned in #130 bindless table.
	_swap_replace(_tex_hit_replace, _solid_image(Color(0.2, 0.92, 0.35)))
	remove_child(_hit_replace_inst)
	_hit_replace_inst.free()
	_hit_replace_inst = _spawn_panel("HitReplaceFresh", Vector3(1.8, -1.0, 0.0), _hit_replace_mat, _make_panel_mesh())
	await _wait_frames(10)
	if _expect_hit_groups:
		var runs_now := int(RenderingServer.nextgi_get_counters().get("hit_check_runs", 0))
		var replace_hit := await _wait_hit_check(runs_now + 2)
		_report["post_replace_hit"] = _check_hit_counters("post_replace_hit", replace_hit, runs_now + 2)

	ProjectSettings.set_setting(HIT_CHECK_SETTING, false)
	await _wait_frames(4)

	# Phase 6: Proxy texture replacement/retargeting, render-target resize + 0x0 shrink/restore, and
	# unlinked-proxy texture_replace_compatible() (#305 review follow-ups).
	var proxy_base_a := RenderingServer.texture_2d_create(_solid_image(Color(0.92, 0.12, 0.12)))
	var proxy_base_b := RenderingServer.texture_2d_create(_solid_image(Color(0.12, 0.18, 0.92)))
	var proxy_rid := RenderingServer.texture_proxy_create(proxy_base_a)
	var proxy_shader := Shader.new()
	proxy_shader.code = HIT_TEX_SHADER
	var proxy_mat := ShaderMaterial.new()
	proxy_mat.shader = proxy_shader
	RenderingServer.material_set_param(proxy_mat.get_rid(), "albedo_tex", proxy_rid)
	_shading_box_inst.material_override = proxy_mat
	await _wait_frames(4)
	var proxy_pre_col := _mean_color_in_rect(get_viewport().get_texture().get_image(), shade_rect)

	# Non-compatible texture_replace() on the proxy's base frees the old proxy shared views before
	# texture_proxy_update() runs; MaterialData::texture_cache must still invalidate and refresh.
	RenderingServer.texture_replace(proxy_base_a, RenderingServer.texture_2d_create(_solid_image(Color(0.12, 0.9, 0.15))))
	await _wait_frames(4)
	var proxy_rep_col := _mean_color_in_rect(get_viewport().get_texture().get_image(), shade_rect)

	# Retarget the proxy directly to proxy_base_b.
	RenderingServer.texture_proxy_update(proxy_rid, proxy_base_b)
	await _wait_frames(4)
	var proxy_retarget_col := _mean_color_in_rect(get_viewport().get_texture().get_image(), shade_rect)

	_report["proxy_updates"] = {
		"initial": [proxy_pre_col.r, proxy_pre_col.g, proxy_pre_col.b],
		"after_base_replace": [proxy_rep_col.r, proxy_rep_col.g, proxy_rep_col.b],
		"after_retarget": [proxy_retarget_col.r, proxy_retarget_col.g, proxy_retarget_col.b],
	}
	_check(proxy_pre_col.r > proxy_pre_col.b + 0.15, "proxy_updates: expected red before base replace, got %s" % str(proxy_pre_col))
	_check(proxy_rep_col.g > proxy_rep_col.r + 0.15, "proxy_updates: expected green after base texture_replace, got %s" % str(proxy_rep_col))
	_check(proxy_retarget_col.b > proxy_retarget_col.g + 0.15, "proxy_updates: expected blue after texture_proxy_update, got %s" % str(proxy_retarget_col))

	# RenderTarget resize (64x64 -> 128x128 -> 0x0 -> 64x64) with both a direct RT texture and a proxy
	# bound to active materials.
	var sub_vp := SubViewport.new()
	sub_vp.size = Vector2i(64, 64)
	sub_vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	var sub_rect := ColorRect.new()
	sub_rect.size = Vector2(256, 256)
	sub_rect.color = Color(0.15, 0.2, 0.9, 1.0)
	sub_vp.add_child(sub_rect)
	add_child(sub_vp)

	var rt_tex_rid := RenderingServer.viewport_get_texture(sub_vp.get_viewport_rid())
	var rt_proxy_rid := RenderingServer.texture_proxy_create(rt_tex_rid)
	var rt_proxy_mat := ShaderMaterial.new()
	rt_proxy_mat.shader = proxy_shader
	RenderingServer.material_set_param(rt_proxy_mat.get_rid(), "albedo_tex", rt_proxy_rid)
	var rt_direct_mat := ShaderMaterial.new()
	rt_direct_mat.shader = proxy_shader
	RenderingServer.material_set_param(rt_direct_mat.get_rid(), "albedo_tex", rt_tex_rid)
	_shading_box_inst.material_override = rt_proxy_mat
	_hit_active_inst.material_override = rt_direct_mat
	await _wait_frames(4)

	sub_rect.color = Color(0.95, 0.85, 0.1, 1.0)
	RenderingServer.viewport_set_size(sub_vp.get_viewport_rid(), 128, 128)
	await _wait_frames(4)
	var rt_resized_col := _mean_color_in_rect(get_viewport().get_texture().get_image(), shade_rect)
	RenderingServer.viewport_set_size(sub_vp.get_viewport_rid(), 0, 0)
	await _wait_frames(4)
	RenderingServer.viewport_set_size(sub_vp.get_viewport_rid(), 64, 64)
	await _wait_frames(4)
	_report["render_target_resize"] = {
		"resized_color": [rt_resized_col.r, rt_resized_col.g, rt_resized_col.b],
	}
	_check(rt_resized_col.r > rt_resized_col.b + 0.15 and rt_resized_col.g > rt_resized_col.b + 0.15, "render_target_resize: expected yellow after 64->128 resize, got %s" % str(rt_resized_col))

	# Unlinked proxy passed to texture_replace_compatible() must not call RD::texture_replace_rid(RID(), ...).
	var unlinked_base := RenderingServer.texture_2d_create(_solid_image(Color(0.5, 0.5, 0.5)))
	var unlinked_proxy := RenderingServer.texture_proxy_create(unlinked_base)
	RenderingServer.free_rid(unlinked_base)
	RenderingServer.texture_replace_compatible(unlinked_proxy, RenderingServer.texture_2d_create(_solid_image(Color(0.3, 0.7, 0.3))))
	RenderingServer.free_rid(unlinked_proxy)

	_shading_box_inst.material_override = null
	_hit_active_inst.material_override = _hit_active_mat
	await _wait_frames(4)
	RenderingServer.free_rid(rt_proxy_rid)
	remove_child(sub_vp)
	sub_vp.free()
	RenderingServer.free_rid(proxy_rid)
	RenderingServer.free_rid(proxy_base_a)
	RenderingServer.free_rid(proxy_base_b)
	await _wait_frames(4)

	_report["pass"] = _failures.is_empty()
	_report["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(_report))
	get_tree().quit(0 if _failures.is_empty() else 1)
