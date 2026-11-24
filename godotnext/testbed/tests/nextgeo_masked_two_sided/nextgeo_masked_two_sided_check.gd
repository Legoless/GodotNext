extends Node3D
## NextGeo masked, two-sided, custom-UV, programmable raster distance, and VSM variants check (#153).
##
## Runs with `rendering/nextgeo/debug_programmable_raster = false` (default) to verify every #153
## deliverable end-to-end on real virtualized NextGeo meshes:
## 1. Default routing: alpha scissor, alpha hash, discard, cull_disabled, and cull_front route virtual;
##    alpha blend and alpha-to-coverage stay legacy/alpha.
## 2. Masked coverage (alpha scissor, alpha hash at ALPHA = 0.5 with analytic screen-space derivatives,
##    and discard) matches Forward+ legacy raster in HW (T=0), SW (T=32), and VIS64 buffer modes.
## 3. Two-sided (cull_disabled and cull_front, opaque FIXED and masked programmable): back-facing
##    triangles render in HW and SW, resolve derives FRONT_FACING from screen-space winding, and
##    DO_SIDE_CHECK flips the shading normal to match Forward+.
## 4. Custom UVs from vertex() (both modified UV and custom `varying vec2`) drive masked coverage
##    matching Forward+ in HW and SW.
## 5. GeometryInstance3D.programmable_raster_distance: past distance, 100% of a masked instance's
##    clusters demote to the FIXED raster bin (0 programmable clusters, programmable_distance_fixed_clusters
##    equals total clusters) and render solid with zero black holes.
## 6. VSM variants (HW sw_max=0 and SW sw_max=24): masked alpha scissor, alpha hash (ALPHA = 0.5),
##    two-sided (cull_disabled and cull_front), custom UV, and VSM programmable_raster_distance
##    (distance_fixed_clusters > 0, 0 programmable VSM clusters past distance, 0 overflow, 0 #277 sink).
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const ProbeScript = preload("res://tests/nextgeo_sw_raster/nextgeo_sw_raster_probe.gd")

const SETTLE_FRAMES := 14
const VSM_SETTLE_FRAMES := 10
const VSM_SIZE := 256
const VSM_EXTENT := 4.0

const DISCARD_SHADER := """
shader_type spatial;
void fragment() {
	bool u = fract(UV.x * 8.0) < 0.5;
	bool v = fract(UV.y * 8.0) < 0.5;
	if (u != v) {
		discard;
	}
	ALBEDO = vec3(0.25, 0.45, 0.85);
}
"""

const FRONT_FACING_TWO_SIDED_SHADER := """
shader_type spatial;
render_mode cull_disabled;
void fragment() {
	ALBEDO = FRONT_FACING ? vec3(0.15, 0.85, 0.20) : vec3(0.85, 0.15, 0.20);
	ROUGHNESS = 0.9;
}
"""

const CUSTOM_UV_BUILTIN_SHADER := """
shader_type spatial;
uniform sampler2D stripe_tex : filter_nearest_mipmap;
void vertex() {
	UV = UV * vec2(2.0, 1.0) + vec2(0.125, 0.0);
}
void fragment() {
	vec4 s = texture(stripe_tex, UV);
	ALBEDO = vec3(0.85, 0.65, 0.20);
	ALPHA = s.a;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""

const CUSTOM_UV_VARYING_SHADER := """
shader_type spatial;
uniform sampler2D stripe_tex : filter_nearest_mipmap;
varying vec2 v_mask_uv;
void vertex() {
	v_mask_uv = UV * vec2(1.5, 1.0) + vec2(0.0625, 0.0);
}
void fragment() {
	vec4 s = texture(stripe_tex, v_mask_uv);
	ALBEDO = vec3(0.20, 0.75, 0.80);
	ALPHA = s.a;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""

const VSM_MASKED_STRIPES_SHADER := """
shader_type spatial;
render_mode cull_disabled;
void fragment() {
	ALBEDO = vec3(0.9, 0.9, 0.9);
	ALPHA = step(0.5, fract(UV.x * 4.0));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""

const VSM_ALPHA_HASH_SHADER := """
shader_type spatial;
render_mode cull_disabled;
void fragment() {
	ALBEDO = vec3(0.9, 0.9, 0.9);
	ALPHA = 0.5;
	ALPHA_HASH_SCALE = 1.0;
}
"""

const VSM_CUSTOM_UV_MASKED_SHADER := """
shader_type spatial;
render_mode cull_disabled;
varying vec2 v_uv;
void vertex() {
	v_uv = UV * vec2(3.0, 1.0) + vec2(0.1, 0.0);
}
void fragment() {
	ALBEDO = vec3(0.9, 0.9, 0.9);
	ALPHA = step(0.5, fract(v_uv.x));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""

var _probe: CompositorEffect
var _camera: Camera3D
var _sun: DirectionalLight3D
var _plane_mesh: Mesh
var _sphere_mesh: Mesh
var _box_mesh: Mesh
var _stripe_tex: ImageTexture
var _instances: Dictionary = {} # name -> MeshInstance3D
var _records: Dictionary = {} # name -> record index
var _temp_files: Array[String] = []
var _failures: Array[String] = []
var _report: Dictionary = {}
var _expect_bins := false


func _ready() -> void:
	# Explicitly verify #153 works with debug_programmable_raster OFF.
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", false)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_local", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_directional", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/prefiltered_distant", false)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_hiz", true)

	var rd := RenderingServer.get_rendering_device()
	var image_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_IMAGE_INT64_ATOMICS)
	var buffer_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_INT64_ATOMICS)
	var indirect_count := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT)
	_expect_bins = indirect_count and (image_int64 or buffer_int64)
	_report["device"] = {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"expect_programmable_bins": _expect_bins,
	}

	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 0.5

	_build_scene()
	_run.call_deferred()


func _make_stripe_texture() -> ImageTexture:
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			var solid := ((x >> 3) & 1) == 0
			img.set_pixel(x, y, Color(0.95, 0.85, 0.30, 1.0 if solid else 0.0))
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


func _virtualize(p_prim: PrimitiveMesh, p_tag: String) -> Mesh:
	var path := "user://nextgeo_masked_two_sided_%s.ngm" % p_tag
	_temp_files.append(path)
	var imp := ImporterMesh.from_mesh(p_prim)
	var err := imp.build_virtual_geometry(path)
	var mesh := imp.get_mesh()
	if err != OK or mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(%s) failed (err=%d)" % [p_tag, err])
	return mesh


func _shader_mat(p_code: String) -> ShaderMaterial:
	var sh := Shader.new()
	sh.code = p_code
	var mat := ShaderMaterial.new()
	mat.shader = sh
	return mat


func _add_inst(p_name: String, p_mesh: Mesh, p_xform: Transform3D, p_mat: Material) -> MeshInstance3D:
	var inst := MeshInstance3D.new()
	inst.name = p_name
	inst.mesh = p_mesh
	inst.transform = p_xform
	if p_mat != null:
		inst.material_override = p_mat
	add_child(inst)
	_instances[p_name] = inst
	return inst


func _build_scene() -> void:
	_stripe_tex = _make_stripe_texture()

	var box_prim := BoxMesh.new()
	box_prim.size = Vector3(1.0, 1.0, 1.0)
	var bg_mat := StandardMaterial3D.new()
	bg_mat.albedo_color = Color(0.25, 0.25, 0.30)
	bg_mat.roughness = 1.0
	box_prim.material = bg_mat
	_box_mesh = _virtualize(box_prim, "box")

	var plane_prim := PlaneMesh.new()
	plane_prim.size = Vector2(1.4, 1.4)
	plane_prim.subdivide_width = 40
	plane_prim.subdivide_depth = 40
	_plane_mesh = _virtualize(plane_prim, "plane")

	var sphere_prim := SphereMesh.new()
	sphere_prim.radius = 0.65
	sphere_prim.height = 1.3
	sphere_prim.radial_segments = 96
	sphere_prim.rings = 48
	_sphere_mesh = _virtualize(sphere_prim, "sphere")

	_camera = Camera3D.new()
	_camera.fov = 55.0
	_camera.near = 0.05
	_camera.far = 100.0
	_camera.position = Vector3(0.0, 0.0, 6.2)
	_camera.current = true
	add_child(_camera)
	_camera.look_at(Vector3.ZERO)

	_sun = DirectionalLight3D.new()
	_sun.rotation_degrees = Vector3(-18.0, 14.0, 0.0)
	_sun.light_energy = 1.2
	_sun.shadow_enabled = false
	add_child(_sun)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.05, 0.06, 0.08)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.08, 0.08, 0.08)
	env.ambient_light_energy = 1.0
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var compositor := Compositor.new()
	_probe = ProbeScript.new()
	var effects: Array[CompositorEffect] = [_probe]
	compositor.compositor_effects = effects
	var world := WorldEnvironment.new()
	world.environment = env
	world.compositor = compositor
	add_child(world)

	var face_cam := Basis(Vector3.RIGHT, deg_to_rad(90.0))
	var face_away := Basis(Vector3.RIGHT, deg_to_rad(-90.0))

	# Fixed-function opaque background behind all test tiles.
	_add_inst("Background", _box_mesh, Transform3D(Basis.from_scale(Vector3(16.0, 10.0, 0.4)), Vector3(0.0, 0.0, -2.5)), null)

	# Row 1 (y = +1.55): Masked modes.
	var mat_scissor := StandardMaterial3D.new()
	mat_scissor.albedo_texture = _stripe_tex
	mat_scissor.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	mat_scissor.alpha_scissor_threshold = 0.5
	mat_scissor.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST_WITH_MIPMAPS
	_add_inst("AlphaScissor", _plane_mesh, Transform3D(face_cam, Vector3(-3.0, 1.55, 0.0)), mat_scissor)

	var mat_hash := StandardMaterial3D.new()
	mat_hash.albedo_color = Color(0.90, 0.45, 0.25, 0.5)
	mat_hash.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_HASH
	mat_hash.alpha_hash_scale = 1.0
	_add_inst("AlphaHash", _plane_mesh, Transform3D(face_cam, Vector3(-1.0, 1.55, 0.0)), mat_hash)

	_add_inst("DiscardMask", _sphere_mesh, Transform3D(Basis(), Vector3(1.0, 1.55, 0.0)), _shader_mat(DISCARD_SHADER))

	var mat_custom_uv := _shader_mat(CUSTOM_UV_BUILTIN_SHADER)
	mat_custom_uv.set_shader_parameter("stripe_tex", _stripe_tex)
	_add_inst("CustomUVBuiltin", _plane_mesh, Transform3D(face_cam, Vector3(3.0, 1.55, 0.0)), mat_custom_uv)

	# Row 2 (y = -0.10): Two-sided & custom varying UV.
	var mat_custom_var := _shader_mat(CUSTOM_UV_VARYING_SHADER)
	mat_custom_var.set_shader_parameter("stripe_tex", _stripe_tex)
	_add_inst("CustomUVVarying", _plane_mesh, Transform3D(face_cam, Vector3(-3.0, -0.10, 0.0)), mat_custom_var)

	# Opaque CULL_DISABLED back-facing plane (FIXED bin, flipped normal under DO_SIDE_CHECK).
	var mat_2s_opaque := StandardMaterial3D.new()
	mat_2s_opaque.albedo_color = Color(0.85, 0.85, 0.90)
	mat_2s_opaque.roughness = 0.9
	mat_2s_opaque.cull_mode = BaseMaterial3D.CULL_DISABLED
	_add_inst("TwoSidedOpaqueBack", _plane_mesh, Transform3D(face_away, Vector3(-1.0, -0.10, 0.0)), mat_2s_opaque)

	# Masked CULL_FRONT back-facing plane (front culled, back rendered + flipped normal).
	var mat_cull_front_masked := StandardMaterial3D.new()
	mat_cull_front_masked.albedo_texture = _stripe_tex
	mat_cull_front_masked.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	mat_cull_front_masked.alpha_scissor_threshold = 0.5
	mat_cull_front_masked.cull_mode = BaseMaterial3D.CULL_FRONT
	mat_cull_front_masked.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST_WITH_MIPMAPS
	_add_inst("CullFrontMaskedBack", _plane_mesh, Transform3D(face_away, Vector3(1.0, -0.10, 0.0)), mat_cull_front_masked)

	# Two-sided FRONT_FACING test: front-facing and back-facing pair sharing one shader.
	var mat_ff := _shader_mat(FRONT_FACING_TWO_SIDED_SHADER)
	_add_inst("FrontFacingFront", _plane_mesh, Transform3D(face_cam.scaled(Vector3(0.45, 1.0, 0.9)), Vector3(2.65, -0.10, 0.0)), mat_ff)
	_add_inst("FrontFacingBack", _plane_mesh, Transform3D(face_away.scaled(Vector3(0.45, 1.0, 0.9)), Vector3(3.35, -0.10, 0.0)), mat_ff)

	# Row 3 (y = -1.75): Opaque CULL_FRONT + legacy fallbacks (alpha blend & alpha-to-coverage).
	var mat_cull_front_opaque := StandardMaterial3D.new()
	mat_cull_front_opaque.albedo_color = Color(0.35, 0.80, 0.55)
	mat_cull_front_opaque.cull_mode = BaseMaterial3D.CULL_FRONT
	_add_inst("CullFrontOpaqueBack", _plane_mesh, Transform3D(face_away, Vector3(-3.0, -1.75, 0.0)), mat_cull_front_opaque)
	_add_inst("CullFrontOpaqueFront", _plane_mesh, Transform3D(face_cam, Vector3(-1.0, -1.75, 0.0)), mat_cull_front_opaque)

	var mat_alpha_blend := StandardMaterial3D.new()
	mat_alpha_blend.albedo_color = Color(0.2, 0.5, 0.9, 0.5)
	mat_alpha_blend.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_add_inst("AlphaBlendLegacy", _plane_mesh, Transform3D(face_cam, Vector3(1.0, -1.75, 0.0)), mat_alpha_blend)

	var mat_a2c := StandardMaterial3D.new()
	mat_a2c.albedo_texture = _stripe_tex
	mat_a2c.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	mat_a2c.alpha_scissor_threshold = 0.5
	mat_a2c.alpha_antialiasing_mode = BaseMaterial3D.ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE
	_add_inst("AlphaToCoverageLegacy", _plane_mesh, Transform3D(face_cam, Vector3(3.0, -1.75, 0.0)), mat_a2c)


func _frames(p_n: int) -> void:
	for _i in p_n:
		await RenderingServer.frame_post_draw


func _capture_probe() -> Dictionary:
	_probe.set("capture", {})
	_probe.set("capture_armed", true)
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var cap: Dictionary = _probe.get("capture")
	_probe.set("capture", {})
	return cap


func _route_of(p_inst: MeshInstance3D) -> Array:
	var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(p_inst.get_instance())
	var route := String(st.get("route", ""))
	var reason := String(st.get("reason", ""))
	var surfaces: Array = st.get("surfaces", [])
	if not surfaces.is_empty():
		var s0: Dictionary = surfaces[0]
		route = String(s0.get("route", ""))
		reason = String(s0.get("reason", ""))
	_records[p_inst.name] = int(st.get("record_index", -1))
	return [route, reason]


func _pixels_of(p_cap: Dictionary, p_record: int) -> PackedByteArray:
	var size: Vector2i = p_cap["size"]
	var words: PackedInt32Array = p_cap["words"]
	var mask := PackedByteArray()
	mask.resize(size.x * size.y)
	for i in size.x * size.y:
		mask[i] = 1 if words[i * ProbeScript.WORDS_PER_PIXEL] == p_record else 0
	return mask


func _bbox_fill(p_cap: Dictionary, p_record: int) -> Array:
	var size: Vector2i = p_cap["size"]
	var mask := _pixels_of(p_cap, p_record)
	var pixels := 0
	var lo := Vector2i(size.x, size.y)
	var hi := Vector2i(-1, -1)
	for y in size.y:
		for x in size.x:
			if mask[y * size.x + x] == 1:
				pixels += 1
				lo = Vector2i(mini(lo.x, x), mini(lo.y, y))
				hi = Vector2i(maxi(hi.x, x), maxi(hi.y, y))
	if pixels == 0:
		return [0, 0, 0.0]
	var area := (hi.x - lo.x + 1) * (hi.y - lo.y + 1)
	return [pixels, area, float(pixels) / float(area)]


func _mean_color_of_record(p_cap: Dictionary, p_img: Image, p_record: int) -> Dictionary:
	var size: Vector2i = p_cap["size"]
	var words: PackedInt32Array = p_cap["words"]
	var img_size := p_img.get_size()
	var sum := Color(0.0, 0.0, 0.0, 0.0)
	var count := 0
	var min_lum := 100.0
	for y in size.y:
		var sy := mini(int((float(y) + 0.5) * float(img_size.y) / float(size.y)), img_size.y - 1)
		for x in size.x:
			var idx := (y * size.x + x) * ProbeScript.WORDS_PER_PIXEL
			if words[idx] != p_record:
				continue
			var sx := mini(int((float(x) + 0.5) * float(img_size.x) / float(size.x)), img_size.x - 1)
			var c := p_img.get_pixel(sx, sy)
			sum += c
			min_lum = minf(min_lum, c.get_luminance())
			count += 1
	if count == 0:
		return {"count": 0, "mean": Color(0, 0, 0), "min_lum": 0.0}
	return {"count": count, "mean": sum / float(count), "min_lum": min_lum}


func _compare_masks(p_ref: PackedByteArray, p_tst: PackedByteArray) -> Dictionary:
	var ref_cnt := 0
	var tst_cnt := 0
	var both := 0
	for i in p_ref.size():
		ref_cnt += p_ref[i]
		tst_cnt += p_tst[i]
		both += 1 if (p_ref[i] == 1 and p_tst[i] == 1) else 0
	return {
		"ref": ref_cnt,
		"tst": tst_cnt,
		"both": both,
		"one_sided": ref_cnt + tst_cnt - 2 * both,
	}


func _run() -> void:
	if not _failures.is_empty():
		_finish()
		return

	# 1. Capture Forward+ legacy reference (raster_mode = 1 draws programmable bins on legacy Forward+).
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 1)
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 8.0)
	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", false)
	await _frames(SETTLE_FRAMES)
	var cap_legacy := await _capture_probe()
	var img_legacy := get_viewport().get_texture().get_image()

	# 2. Check default routing (debug_programmable_raster = false, raster_mode = 0).
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 0.0)
	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", false)
	await _frames(SETTLE_FRAMES)
	var routes := {}
	for inst_name: String in _instances:
		routes[inst_name] = _route_of(_instances[inst_name])
	_report["routes"] = routes

	var expected_virtual_masked := ["AlphaScissor", "AlphaHash", "DiscardMask", "CustomUVBuiltin", "CustomUVVarying", "CullFrontMaskedBack"]
	var expected_virtual_fixed := ["Background", "TwoSidedOpaqueBack", "FrontFacingFront", "FrontFacingBack", "CullFrontOpaqueBack", "CullFrontOpaqueFront"]
	var expected_legacy_alpha := ["AlphaBlendLegacy", "AlphaToCoverageLegacy"]

	for n in expected_virtual_fixed:
		var r: Array = routes[n]
		if r[0] != "virtual" or r[1] != "NONE":
			_failures.append("route/%s: got %s/%s expected virtual/NONE" % [n, r[0], r[1]])
	for n in expected_virtual_masked:
		var r: Array = routes[n]
		var exp_route := "virtual" if _expect_bins else "legacy"
		var exp_reason := "NONE" if _expect_bins else "alpha"
		if r[0] != exp_route or r[1] != exp_reason:
			_failures.append("route/%s: got %s/%s expected %s/%s" % [n, r[0], r[1], exp_route, exp_reason])
	for n in expected_legacy_alpha:
		var r: Array = routes[n]
		if r[0] != "legacy" or r[1] != "alpha":
			_failures.append("route/%s: got %s/%s expected legacy/alpha" % [n, r[0], r[1]])

	var cap_hw := await _capture_probe()
	var img_hw := get_viewport().get_texture().get_image()
	var stats_hw: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())

	# 3. SW raster (T = 32) and VIS64 buffer parity.
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 32.0)
	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", false)
	await _frames(SETTLE_FRAMES)
	var cap_sw := await _capture_probe()
	var img_sw := get_viewport().get_texture().get_image()
	var stats_sw: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())

	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", true)
	await _frames(SETTLE_FRAMES)
	var cap_buf := await _capture_probe()
	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", false)

	_report["main_stats"] = {"hw": stats_hw, "sw": stats_sw}

	if _expect_bins:
		# HW & SW stats checks.
		if int(stats_hw.get("programmable_sw_clusters", -1)) != 0 or int(stats_hw.get("programmable_hw_clusters", 0)) <= 0:
			_failures.append("main_hw: expected prog_sw==0 and prog_hw>0, got sw=%s hw=%s" % [str(stats_hw.get("programmable_sw_clusters")), str(stats_hw.get("programmable_hw_clusters"))])
		if int(stats_sw.get("programmable_sw_clusters", 0)) <= 0:
			_failures.append("main_sw: expected prog_sw>0, got %s" % str(stats_sw.get("programmable_sw_clusters")))
		if int(stats_hw.get("programmable_distance_fixed_clusters", -1)) != 0 or int(stats_sw.get("programmable_distance_fixed_clusters", -1)) != 0:
			_failures.append("main: expected 0 distance-demoted clusters at distance=0")

		# Background mask parity vs Forward+ legacy reference (proves scissor, hash, discard, custom UVs, cull_front match!).
		var bg_rec := int(_records["Background"])
		var bg_ref := _pixels_of(cap_legacy, bg_rec)
		for pair in [["hw", cap_hw], ["sw", cap_sw]]:
			var label: String = pair[0]
			var cap: Dictionary = pair[1]
			var cmp := _compare_masks(bg_ref, _pixels_of(cap, bg_rec))
			_report["bg_vs_legacy_" + label] = cmp
			# Alpha hash dither can differ slightly at quad boundaries due to analytic vs 2x2 quad dFdx/dFdy.
			var limit := maxi(64, int(0.03 * float(cmp["ref"])))
			if int(cmp["one_sided"]) > limit:
				_failures.append("bg_vs_legacy_%s: %d pixels differ (limit %d of %d)" % [label, cmp["one_sided"], limit, cmp["ref"]])

		# SW vs VIS64 buffer must be bit-identical.
		var w_sw: PackedInt32Array = cap_sw["words"]
		var w_buf: PackedInt32Array = cap_buf["words"]
		var buf_diff := 0
		for i in w_sw.size() / ProbeScript.WORDS_PER_PIXEL:
			var b := i * ProbeScript.WORDS_PER_PIXEL
			if w_sw[b] != w_buf[b] or (w_sw[b + 1] & 0x7F) != (w_buf[b + 1] & 0x7F) or w_sw[b + 2] != w_buf[b + 2]:
				buf_diff += 1
		if buf_diff != 0:
			_failures.append("vis64_buffer: %d pixels differ from image mode" % buf_diff)

		# Individual feature checks in BOTH HW and SW!
		for phase_pair in [["hw", cap_hw, img_hw], ["sw", cap_sw, img_sw]]:
			var plabel: String = phase_pair[0]
			var cap: Dictionary = phase_pair[1]
			var img: Image = phase_pair[2]

			# Masked coverage fills strictly between 0.25 and 0.75 (proves alpha scissor, alpha hash, discard, custom UVs, cull_front masked).
			for mname in ["AlphaScissor", "AlphaHash", "DiscardMask", "CustomUVBuiltin", "CustomUVVarying", "CullFrontMaskedBack"]:
				var fill := _bbox_fill(cap, int(_records[mname]))
				_report["%s_%s_fill" % [plabel, mname]] = fill
				if int(fill[0]) < 40 or float(fill[2]) < 0.25 or float(fill[2]) > 0.75:
					_failures.append("%s/%s: fill=%.3f (%d/%d px) outside [0.25, 0.75]" % [plabel, mname, fill[2], fill[0], fill[1]])

			# CULL_FRONT opaque: back-facing visible, front-facing culled!
			var cf_back := _bbox_fill(cap, int(_records["CullFrontOpaqueBack"]))
			var cf_front := _bbox_fill(cap, int(_records["CullFrontOpaqueFront"]))
			if int(cf_back[0]) < 80 or float(cf_back[2]) < 0.9:
				_failures.append("%s/CullFrontOpaqueBack: expected solid back face, got %s" % [plabel, str(cf_back)])
			if int(cf_front[0]) != 0:
				_failures.append("%s/CullFrontOpaqueFront: expected 0 pixels (front culled), got %d" % [plabel, cf_front[0]])

			# Two-sided opaque normal flip (DO_SIDE_CHECK): lit back face matches legacy Forward+ brightness!
			var ts_leg := _mean_color_of_record(cap_legacy, img_legacy, int(_records["TwoSidedOpaqueBack"]))
			var ts_cur := _mean_color_of_record(cap, img, int(_records["TwoSidedOpaqueBack"]))
			if int(ts_cur["count"]) < 80:
				_failures.append("%s/TwoSidedOpaqueBack: too few pixels (%d)" % [plabel, ts_cur["count"]])
			else:
				var leg_l: float = (ts_leg["mean"] as Color).get_luminance()
				var cur_l: float = (ts_cur["mean"] as Color).get_luminance()
				if cur_l < 0.30 or absf(cur_l - leg_l) > 0.05:
					_failures.append("%s/TwoSidedOpaqueBack: lit luminance %.3f vs legacy %.3f (normal flip mismatch)" % [plabel, cur_l, leg_l])

			# FRONT_FACING resolve check: FrontFacingFront is green (g > r*2), FrontFacingBack is red (r > g*2)!
			var ff_front := _mean_color_of_record(cap, img, int(_records["FrontFacingFront"]))
			var ff_back := _mean_color_of_record(cap, img, int(_records["FrontFacingBack"]))
			var c_front: Color = ff_front["mean"]
			var c_back: Color = ff_back["mean"]
			if int(ff_front["count"]) < 40 or c_front.g <= c_front.r * 2.0:
				_failures.append("%s/FrontFacingFront: expected green front face, got %s (px=%d)" % [plabel, str(c_front), ff_front["count"]])
			if int(ff_back["count"]) < 40 or c_back.r <= c_back.g * 2.0:
				_failures.append("%s/FrontFacingBack: expected red back face, got %s (px=%d)" % [plabel, str(c_back), ff_back["count"]])

		# 4. Main-view programmable_raster_distance test: isolate a single masked instance!
		await _test_main_programmable_distance()

		# 5. VSM variants: masked scissor, alpha hash, two-sided (cull_disabled & cull_front), custom UV, and programmable_raster_distance!
		await _test_vsm_variants()

	_finish()


func _test_main_programmable_distance() -> void:
	# Hide all other instances so the viewport contains ONLY one curved masked sphere at z = 0 (distance ~5.5 m from camera).
	for inst_name: String in _instances:
		(_instances[inst_name] as MeshInstance3D).visible = (inst_name == "DiscardMask")
	var inst: MeshInstance3D = _instances["DiscardMask"]
	var saved_xform := inst.transform
	inst.transform = Transform3D(Basis.from_scale(Vector3.ONE * 1.4), Vector3(0.0, 0.0, 0.0))

	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 24.0)

	# 4a. programmable_raster_distance = 12.0 (> 5.5 m): stays programmable (0 distance-demoted, both SW/HW clusters).
	inst.programmable_raster_distance = 12.0
	await _frames(SETTLE_FRAMES)
	var st_near: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var cap_near := await _capture_probe()
	var fill_near := _bbox_fill(cap_near, int(_records["DiscardMask"]))

	if int(st_near.get("programmable_clusters", 0)) <= 0 or int(st_near.get("programmable_sw_clusters", 0)) <= 0:
		_failures.append("dist_near: expected programmable_clusters > 0 and prog_sw > 0, got total=%s sw=%s" % [str(st_near.get("programmable_clusters")), str(st_near.get("programmable_sw_clusters"))])
	if int(st_near.get("programmable_distance_fixed_clusters", -1)) != 0:
		_failures.append("dist_near: expected programmable_distance_fixed_clusters == 0, got %s" % str(st_near.get("programmable_distance_fixed_clusters")))
	if float(fill_near[2]) > 0.70:
		_failures.append("dist_near: expected masked holes (fill <= 0.70), got %.3f" % fill_near[2])

	# 4b. programmable_raster_distance = 3.0 (< 5.5 m): 100% FIXED bin, 0 programmable, solid color (no black holes)!
	inst.programmable_raster_distance = 3.0
	await _frames(SETTLE_FRAMES)
	var st_far: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var cap_far := await _capture_probe()
	var img_far := get_viewport().get_texture().get_image()
	var fill_far := _bbox_fill(cap_far, int(_records["DiscardMask"]))
	var col_far := _mean_color_of_record(cap_far, img_far, int(_records["DiscardMask"]))

	var far_prog := int(st_far.get("programmable_clusters", -1))
	var far_prog_sw := int(st_far.get("programmable_sw_clusters", -1))
	var far_prog_hw := int(st_far.get("programmable_hw_clusters", -1))
	var far_fixed := int(st_far.get("fixed_clusters", 0))
	var far_demoted := int(st_far.get("programmable_distance_fixed_clusters", 0))
	if far_prog != 0 or far_prog_sw != 0 or far_prog_hw != 0:
		_failures.append("dist_far: expected 100%% FIXED bin (0 programmable clusters), got prog=%d sw=%d hw=%d" % [far_prog, far_prog_sw, far_prog_hw])
	if far_fixed <= 1 or far_demoted != far_fixed:
		_failures.append("dist_far: expected programmable_distance_fixed_clusters == fixed_clusters > 1, got demoted=%d fixed=%d" % [far_demoted, far_fixed])
	# A solid sphere covers ~pi/4 (~0.785) of its screen bounding box; masked checker covers ~0.40.
	if int(fill_far[0]) < 100 or float(fill_far[2]) < 0.72:
		_failures.append("dist_far: expected solid FIXED sphere coverage (fill >= 0.72), got %.3f (%d/%d px)" % [fill_far[2], fill_far[0], fill_far[1]])
	if float(col_far["min_lum"]) < 0.04 or (col_far["mean"] as Color).b < 0.15:
		_failures.append("dist_far: resolve discarded demoted pixels (min_lum=%.3f mean=%s, black hole!)" % [float(col_far["min_lum"]), str(col_far["mean"])])

	_report["main_programmable_distance"] = {
		"near": {
			"programmable_clusters": int(st_near.get("programmable_clusters", 0)),
			"programmable_sw_clusters": int(st_near.get("programmable_sw_clusters", 0)),
			"fixed_clusters": int(st_near.get("fixed_clusters", 0)),
			"demoted": int(st_near.get("programmable_distance_fixed_clusters", 0)),
			"fill": fill_near,
		},
		"far": {
			"programmable_clusters": far_prog,
			"fixed_clusters": far_fixed,
			"demoted": far_demoted,
			"fill": fill_far,
			"min_lum": float(col_far["min_lum"]),
		},
	}

	# Restore.
	inst.programmable_raster_distance = 0.0
	inst.transform = saved_xform
	for inst_name: String in _instances:
		(_instances[inst_name] as MeshInstance3D).visible = true


func _test_vsm_variants() -> void:
	# Hide main-view scene nodes and run an isolated top-down orthographic VSM test viewport.
	for inst_name: String in _instances:
		(_instances[inst_name] as MeshInstance3D).visible = false
	_sun.visible = false
	_camera.current = false

	ProjectSettings.set_setting("rendering/lights_and_shadows/shadow_method", 1) # Virtual Shadow Maps
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", false)

	var sv := SubViewport.new()
	sv.size = Vector2i(VSM_SIZE, VSM_SIZE)
	sv.use_hdr_2d = true
	sv.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	sv.msaa_3d = Viewport.MSAA_DISABLED
	sv.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	sv.use_taa = false
	add_child(sv)

	var vcam := Camera3D.new()
	vcam.projection = Camera3D.PROJECTION_ORTHOGONAL
	vcam.size = VSM_EXTENT
	vcam.near = 0.05
	vcam.far = 20.0
	vcam.transform = Transform3D(Basis.looking_at(Vector3.DOWN, Vector3.FORWARD), Vector3(0.0, 5.0, 0.0))
	sv.add_child(vcam)
	vcam.current = true

	var white := StandardMaterial3D.new()
	white.albedo_color = Color(1.0, 1.0, 1.0)
	white.roughness = 1.0
	white.metallic = 0.0
	white.metallic_specular = 0.0

	var floor_inst := MeshInstance3D.new()
	var fp := PlaneMesh.new()
	fp.size = Vector2(12.0, 12.0)
	floor_inst.mesh = fp
	floor_inst.material_override = white
	floor_inst.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(floor_inst)

	var caster_plane := PlaneMesh.new()
	caster_plane.size = Vector2(1.0, 1.0)
	caster_plane.subdivide_width = 48
	caster_plane.subdivide_depth = 48
	var caster_vmesh := _virtualize(caster_plane, "vsm_caster")

	var caster := MeshInstance3D.new()
	caster.mesh = caster_vmesh
	caster.position = Vector3(-1.0, 1.0, 0.0)
	add_child(caster)

	# Reference lit floor with shadow casting disabled.
	var sun_ref := _make_vsm_sun()
	caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	await _frames(VSM_SETTLE_FRAMES)
	var ref_lum := _capture_subviewport_lum(sv)
	sun_ref.queue_free()
	await _frames(2)

	var vsm_out := {}

	# Helper materials for VSM cases.
	var mat_vsm_scissor := _shader_mat(VSM_MASKED_STRIPES_SHADER)
	var mat_vsm_hash := _shader_mat(VSM_ALPHA_HASH_SHADER)
	var mat_vsm_custom_uv := _shader_mat(VSM_CUSTOM_UV_MASKED_SHADER)
	var mat_vsm_cull_disabled := StandardMaterial3D.new()
	mat_vsm_cull_disabled.albedo_color = Color(1.0, 1.0, 1.0)
	mat_vsm_cull_disabled.cull_mode = BaseMaterial3D.CULL_DISABLED
	var mat_vsm_cull_front := StandardMaterial3D.new()
	mat_vsm_cull_front.albedo_color = Color(1.0, 1.0, 1.0)
	mat_vsm_cull_front.cull_mode = BaseMaterial3D.CULL_FRONT

	var vsm_cases := [
		{"name": "vsm_scissor_hw", "mat": mat_vsm_scissor, "rot_x": 0.0, "sw_max": 0.0, "dist": 0.0, "min_f": 0.25, "max_f": 0.75, "expect_prog": "hw"},
		{"name": "vsm_scissor_sw", "mat": mat_vsm_scissor, "rot_x": 0.0, "sw_max": 24.0, "dist": 0.0, "min_f": 0.25, "max_f": 0.75, "expect_prog": "sw"},
		{"name": "vsm_hash_hw", "mat": mat_vsm_hash, "rot_x": 0.0, "sw_max": 0.0, "dist": 0.0, "min_f": 0.20, "max_f": 0.80, "expect_prog": "hw"},
		{"name": "vsm_hash_sw", "mat": mat_vsm_hash, "rot_x": 0.0, "sw_max": 24.0, "dist": 0.0, "min_f": 0.20, "max_f": 0.80, "expect_prog": "sw"},
		{"name": "vsm_custom_uv_sw", "mat": mat_vsm_custom_uv, "rot_x": 0.0, "sw_max": 24.0, "dist": 0.0, "min_f": 0.25, "max_f": 0.75, "expect_prog": "sw"},
		# Flipped upside down (180 deg around X -> normal faces -Y, back-facing to top-down sun).
		{"name": "vsm_two_sided_disabled_sw", "mat": mat_vsm_cull_disabled, "rot_x": 180.0, "sw_max": 24.0, "dist": 0.0, "min_f": 0.95, "max_f": 1.01, "expect_prog": "fixed"},
		{"name": "vsm_two_sided_front_sw", "mat": mat_vsm_cull_front, "rot_x": 180.0, "sw_max": 24.0, "dist": 0.0, "min_f": 0.95, "max_f": 1.01, "expect_prog": "fixed"},
		# Programmable distance cutoff in VSM: camera at y=5, caster at (-1,1,0) -> distance ~4.1 m > 2.0 m -> 100% FIXED solid shadow!
		{"name": "vsm_distance_fixed_hw", "mat": mat_vsm_scissor, "rot_x": 0.0, "sw_max": 0.0, "dist": 2.0, "min_f": 0.95, "max_f": 1.01, "expect_prog": "demoted"},
		{"name": "vsm_distance_fixed_sw", "mat": mat_vsm_scissor, "rot_x": 0.0, "sw_max": 24.0, "dist": 2.0, "min_f": 0.95, "max_f": 1.01, "expect_prog": "demoted"},
	]

	caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	for c in vsm_cases:
		var cname: String = c["name"]
		ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", float(c["sw_max"]))
		caster.material_override = c["mat"]
		caster.rotation_degrees = Vector3(float(c["rot_x"]), 0.0, 0.0)
		caster.programmable_raster_distance = float(c["dist"])
		var sun := _make_vsm_sun()
		var peak := await _settle_vsm_peak(VSM_SETTLE_FRAMES)
		var lit_lum := _capture_subviewport_lum(sv)
		sun.queue_free()
		await _frames(2)

		var frac := _vsm_shadow_fraction(ref_lum, lit_lum, Vector3(-1.0, 1.0, 0.0), 0.5)
		if frac < float(c["min_f"]) or frac > float(c["max_f"]):
			_failures.append("%s: shadow fraction %.4f outside [%.2f, %.2f]" % [cname, frac, float(c["min_f"]), float(c["max_f"])])
		if not bool(peak.get("vsm_active", false)) or not bool(peak.get("nanite_active", false)):
			_failures.append("%s: VSM or Nanite VSM inactive (peak=%s)" % [cname, str(peak)])
		if int(peak.get("overflow", 0)) != 0 or int(peak.get("sink", 0)) != 0:
			_failures.append("%s: VSM overflow=%d sink=%d" % [cname, int(peak.get("overflow", 0)), int(peak.get("sink", 0))])
		if bool(peak.get("gpu_counters_valid", false)):
			var expect_prog: String = c["expect_prog"]
			if expect_prog == "hw" and int(peak.get("prog_hw_clusters", 0)) <= 0:
				_failures.append("%s: expected prog_hw_clusters > 0, got %s" % [cname, str(peak)])
			elif expect_prog == "sw" and int(peak.get("prog_sw_clusters", 0)) <= 0:
				_failures.append("%s: expected prog_sw_clusters > 0, got %s" % [cname, str(peak)])
			elif expect_prog == "fixed":
				if int(peak.get("prog_hw_clusters", -1)) != 0 or int(peak.get("prog_sw_clusters", -1)) != 0:
					_failures.append("%s: opaque two-sided should stay in FIXED VSM bin, got %s" % [cname, str(peak)])
				if int(peak.get("hw_clusters", 0)) + int(peak.get("sw_clusters", 0)) <= 0:
					_failures.append("%s: expected FIXED VSM clusters > 0, got %s" % [cname, str(peak)])
			elif expect_prog == "demoted":
				if int(peak.get("prog_hw_clusters", -1)) != 0 or int(peak.get("prog_sw_clusters", -1)) != 0:
					_failures.append("%s: expected 0 programmable VSM clusters past distance, got %s" % [cname, str(peak)])
				if int(peak.get("distance_fixed_clusters", 0)) <= 0:
					_failures.append("%s: expected distance_fixed_clusters > 0, got %s" % [cname, str(peak)])
		else:
			_failures.append("%s: VSM GPU counters invalid" % cname)
		vsm_out[cname] = {
			"shadow_fraction": snappedf(frac, 1e-4),
			"prog_hw": int(peak.get("prog_hw_clusters", 0)),
			"prog_sw": int(peak.get("prog_sw_clusters", 0)),
			"hw": int(peak.get("hw_clusters", 0)),
			"sw": int(peak.get("sw_clusters", 0)),
			"distance_fixed": int(peak.get("distance_fixed_clusters", 0)),
		}

	_report["vsm"] = vsm_out
	caster.queue_free()
	floor_inst.queue_free()
	sv.queue_free()
	await _frames(2)


func _make_vsm_sun() -> DirectionalLight3D:
	var sun := DirectionalLight3D.new()
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_ORTHOGONAL
	sun.directional_shadow_max_distance = 12.0
	sun.shadow_enabled = true
	sun.shadow_method = Light3D.SHADOW_METHOD_VIRTUAL
	sun.transform = Transform3D(Basis.looking_at(Vector3(1.0, -1.0, 0.0).normalized(), Vector3.UP), Vector3(0.0, 5.0, 0.0))
	add_child(sun)
	return sun


func _settle_vsm_peak(p_frames: int) -> Dictionary:
	var peak := {
		"vsm_active": false,
		"nanite_active": false,
		"gpu_counters_valid": false,
		"sw_clusters": 0,
		"hw_clusters": 0,
		"prog_sw_clusters": 0,
		"prog_hw_clusters": 0,
		"distance_fixed_clusters": 0,
		"overflow": 0,
		"sink": 0,
	}
	for _i in p_frames:
		await RenderingServer.frame_post_draw
		var info := RenderingServer.virtual_shadow_maps_get_debug_info()
		if bool(info.get("active", false)):
			peak["vsm_active"] = true
		var n: Dictionary = info.get("nanite", {})
		if bool(n.get("active", false)):
			peak["nanite_active"] = true
		if bool(n.get("gpu_counters_valid", false)):
			peak["gpu_counters_valid"] = true
			for k in ["sw_clusters", "hw_clusters", "prog_sw_clusters", "prog_hw_clusters", "distance_fixed_clusters", "overflow", "sink"]:
				peak[k] = maxi(int(peak[k]), int(n.get(k, 0)))
	return peak


func _capture_subviewport_lum(p_sv: SubViewport) -> PackedFloat32Array:
	var img := p_sv.get_texture().get_image()
	img.convert(Image.FORMAT_RF)
	return img.get_data().to_float32_array()


func _vsm_shadow_fraction(p_ref: PackedFloat32Array, p_lit: PackedFloat32Array, p_center: Vector3, p_half: float) -> float:
	# Directional sun (1, -1, 0) projects center (cx, cy, cz) onto y=0 at (cx + cy, cz).
	var x_min := p_center.x - p_half + p_center.y + 0.06
	var x_max := p_center.x + p_half + p_center.y - 0.06
	var z_min := p_center.z - p_half + 0.06
	var z_max := p_center.z + p_half - 0.06
	var inside := 0
	var dark := 0
	for py in VSM_SIZE:
		var wz := ((float(py) + 0.5) / float(VSM_SIZE) - 0.5) * VSM_EXTENT
		if wz < z_min or wz > z_max:
			continue
		var row := py * VSM_SIZE
		for px in VSM_SIZE:
			var wx := ((float(px) + 0.5) / float(VSM_SIZE) - 0.5) * VSM_EXTENT
			if wx < x_min or wx > x_max:
				continue
			var r := p_ref[row + px]
			if r < 0.02:
				continue
			inside += 1
			if p_lit[row + px] / r < 0.5:
				dark += 1
	return float(dark) / float(maxi(inside, 1))


func _cleanup() -> void:
	for p in _temp_files:
		if FileAccess.file_exists(p):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(p))
	_temp_files.clear()


func _finish() -> void:
	_cleanup()
	_report["failures"] = _failures
	_report["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_report))
	get_tree().quit(0 if _failures.is_empty() else 1)
