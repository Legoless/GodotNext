extends Node3D
## Pixel depth offset (DEPTH_OFFSET) GPU verification (#149, plan §21.4 contract N6, GN-3, GN-12).
##
## Verifies end-to-end on Vulkan with `rendering/nextgeo/debug_programmable_raster = false` (default):
## 1. Forward+ byte-identity: `DEPTH_OFFSET = 0.0` (and negative clamped `-0.25`) produces 0 differing
##    RGBA8 color bytes and 0 differing depth bits against the identical shader without `DEPTH_OFFSET`.
## 2. NextGeo HW VS (`path=1, T=0`), SW (`T=32`), VIS64 buffer, and HW Mesh (`path=2, T=0` when
##    supported) depth parity against Forward+ legacy raster within `<= 1e-5` normalized depth error
##    (GN-3) across constant PDO, spatially varying/clamped PDO, texture-driven PDO (`textureGrad`),
##    and combined PDO + alpha scissor + two-sided; plus resolve shading parity (`vertex` stays at
##    the triangle position, while exported depth buffer carries the offset depth).
## 3. Occlusion ordering: intersecting PDO plane in front of a plain plane pushes part of the PDO
##    surface behind the plain plane; per-pixel occlusion winner matches Forward+ across HW VS, SW,
##    VIS64 buffer, and HW Mesh.
## 4. VSM PDO shadows (`GODOT_DEV_VSM=1`, `shadow_method=1`): directional and spot light shadows
##    from PDO casters match Forward+ shadow pass (PDO pushes caster past receiver so pushed half
##    stops casting shadow while un-offset half casts full shadow) across non-Nanite Forward+ VSM,
##    NextGeo VSM HW VS, SW, and HW Mesh, with `dynamic_clusters > 0`, `overflow == 0`, `sink == 0`.
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const ProbeScript = preload("res://tests/nextgeo_sw_raster/nextgeo_sw_raster_probe.gd")

const SETTLE_FRAMES := 14
const VSM_SETTLE_FRAMES := 12
const VSM_SIZE := 256
const VSM_EXTENT := 4.0
const MAX_DEPTH_ERR := 1e-5

const PLAIN_REF_SHADER := """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.35 + 0.45 * UV.x, 0.25 + 0.55 * UV.y, 0.72);
	ROUGHNESS = 0.65;
	METALLIC = 0.1;
}
"""

const ZERO_PDO_SHADER := """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.35 + 0.45 * UV.x, 0.25 + 0.55 * UV.y, 0.72);
	ROUGHNESS = 0.65;
	METALLIC = 0.1;
	DEPTH_OFFSET = 0.0;
}
"""

const NEGATIVE_PDO_SHADER := """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.35 + 0.45 * UV.x, 0.25 + 0.55 * UV.y, 0.72);
	ROUGHNESS = 0.65;
	METALLIC = 0.1;
	DEPTH_OFFSET = -0.25;
}
"""

const CONST_PDO_SHADER := """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.82, 0.36, 0.22);
	ROUGHNESS = 0.8;
	DEPTH_OFFSET = 0.35;
}
"""

const CONST_PLAIN_TWIN_SHADER := """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.82, 0.36, 0.22);
	ROUGHNESS = 0.8;
}
"""

const RAMP_PDO_SHADER := """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.24, 0.76, 0.42);
	ROUGHNESS = 0.75;
	// Left side negative (clamped to 0.0), right side pushes up to +0.55m away from camera.
	DEPTH_OFFSET = mix(-0.20, 0.55, UV.x);
}
"""

const TEX_PDO_SHADER := """
shader_type spatial;
uniform sampler2D pdo_tex : filter_linear_mipmap;
void fragment() {
	float h = texture(pdo_tex, UV).r;
	ALBEDO = vec3(0.30, 0.55, 0.88);
	ROUGHNESS = 0.7;
	DEPTH_OFFSET = h * 0.45;
}
"""

const MASKED_TWO_SIDED_PDO_SHADER := """
shader_type spatial;
render_mode cull_disabled;
void fragment() {
	ALBEDO = FRONT_FACING ? vec3(0.88, 0.74, 0.22) : vec3(0.76, 0.24, 0.72);
	ROUGHNESS = 0.8;
	ALPHA = step(0.5, fract(UV.x * 6.0));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
	DEPTH_OFFSET = 0.25;
}
"""

const OCCLUSION_PDO_SHADER := """
shader_type spatial;
void fragment() {
	// Bright red PDO plane placed at z = +0.20 in front of blue plain plane at z = 0.00.
	// UV.x < 0.40 stays in front of plain; UV.x > 0.40 pushes behind plain.
	ALBEDO = vec3(0.95, 0.04, 0.04);
	ROUGHNESS = 0.9;
	DEPTH_OFFSET = UV.x * 0.50;
}
"""

const OCCLUSION_PLAIN_SHADER := """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.04, 0.06, 0.95);
	ROUGHNESS = 0.9;
}
"""

const VSM_HALF_PDO_CASTER_SHADER := """
shader_type spatial;
render_mode cull_disabled;
void fragment() {
	ALBEDO = vec3(0.85, 0.85, 0.85);
	ROUGHNESS = 1.0;
	// Left half (UV.x < 0.5): DEPTH_OFFSET = 0.0 -> casts full shadow onto floor.
	// Right half (UV.x >= 0.5): DEPTH_OFFSET = 2.0m -> pushes depth behind floor -> no shadow.
	DEPTH_OFFSET = (UV.x >= 0.5) ? 2.0 : 0.0;
}
"""

var _probe: CompositorEffect
var _camera: Camera3D
var _sun: DirectionalLight3D
var _plane_mesh: Mesh
var _sphere_mesh: Mesh
var _box_mesh: Mesh
var _pdo_tex: ImageTexture
var _instances: Dictionary = {}
var _records: Dictionary = {}
var _temp_files: Array[String] = []
var _failures: Array[String] = []
var _report: Dictionary = {}
var _expect_bins := false
var _expect_mesh := false


func _ready() -> void:
	# Default #149 admission: debug_programmable_raster MUST stay OFF.
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", false)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/occlusion", false)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_local", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_directional", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/prefiltered_distant", false)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_hiz", true)

	var rd := RenderingServer.get_rendering_device()
	var image_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_IMAGE_INT64_ATOMICS)
	var buffer_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_INT64_ATOMICS)
	var indirect_count := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT)
	var mesh_shader := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_MESH_SHADER)
	var mesh_indirect := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_MESH_SHADER_INDIRECT)
	_expect_bins = indirect_count and (image_int64 or buffer_int64)
	_expect_mesh = _expect_bins and mesh_shader and mesh_indirect

	_report["device"] = {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"expect_programmable_bins": _expect_bins,
		"expect_mesh_hw": _expect_mesh,
	}

	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 0.5

	_build_scene()
	_run.call_deferred()


func _make_pdo_texture() -> ImageTexture:
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			var u := float(x) / 63.0
			var v := float(y) / 63.0
			var h := clampf(0.15 + 0.75 * (0.5 + 0.5 * sin(u * PI * 2.0) * cos(v * PI * 2.0)), 0.0, 1.0)
			img.set_pixel(x, y, Color(h, h, h, 1.0))
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


func _virtualize(p_prim: PrimitiveMesh, p_tag: String) -> Mesh:
	var path := "user://nextgeo_depth_offset_%s.ngm" % p_tag
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
	_pdo_tex = _make_pdo_texture()

	var box_prim := BoxMesh.new()
	box_prim.size = Vector3(1.0, 1.0, 1.0)
	var bg_mat := StandardMaterial3D.new()
	bg_mat.albedo_color = Color(0.20, 0.22, 0.26)
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
	_camera.near = 0.1
	_camera.far = 50.0
	_camera.position = Vector3(0.0, 0.0, 5.8)
	_camera.current = true
	add_child(_camera)
	_camera.look_at(Vector3.ZERO)

	_sun = DirectionalLight3D.new()
	_sun.rotation_degrees = Vector3(-22.0, 16.0, 0.0)
	_sun.light_energy = 1.25
	_sun.shadow_enabled = false
	add_child(_sun)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.05, 0.06, 0.08)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.10, 0.10, 0.10)
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

	# Opaque background wall behind all test surfaces.
	_add_inst("Background", _box_mesh, Transform3D(Basis.from_scale(Vector3(16.0, 10.0, 0.4)), Vector3(0.0, 0.0, -2.5)), null)

	# Row 1 (y = +1.10): PDO materials + plain twin at identical Z for depth-shift verification.
	# Flat planes have zero DAG geometric LOD error, isolating pure PDO & textureGrad depth accuracy (<= 1e-5).
	_add_inst("PlainTwin", _plane_mesh, Transform3D(face_cam, Vector3(-2.7, 1.10, 0.0)), _shader_mat(CONST_PLAIN_TWIN_SHADER))
	_add_inst("ConstPDO", _plane_mesh, Transform3D(face_cam, Vector3(-0.9, 1.10, 0.0)), _shader_mat(CONST_PDO_SHADER))
	_add_inst("RampPDO", _plane_mesh, Transform3D(face_cam, Vector3(0.9, 1.10, 0.0)), _shader_mat(RAMP_PDO_SHADER))
	var mat_tex_pdo := _shader_mat(TEX_PDO_SHADER)
	mat_tex_pdo.set_shader_parameter("pdo_tex", _pdo_tex)
	_add_inst("TexPDO", _plane_mesh, Transform3D(face_cam, Vector3(2.7, 1.10, 0.0)), mat_tex_pdo)

	# Row 2 (y = -1.00): Combined PDO + masked + two-sided back-face, curved high-cluster PDO sphere,
	# and intersecting PDO/plain occlusion test.
	_add_inst("MaskedTwoSidedPDO", _plane_mesh, Transform3D(face_away, Vector3(-2.8, -1.00, 0.0)), _shader_mat(MASKED_TWO_SIDED_PDO_SHADER))
	_add_inst("CurvedSpherePDO", _sphere_mesh, Transform3D(Basis.from_scale(Vector3.ONE * 0.9), Vector3(-1.0, -1.00, 0.0)), mat_tex_pdo)

	# Intersecting occlusion pair: OcclusionPDO sits geometrically at z = +0.20 in front of OcclusionPlain at z = 0.00.
	# With DEPTH_OFFSET = UV.x * 0.50, UV.x < 0.40 wins over plain, UV.x > 0.40 gets occluded by plain!
	_add_inst("OcclusionPlain", _plane_mesh, Transform3D(face_cam.scaled(Vector3(1.5, 1.0, 1.0)), Vector3(1.5, -1.00, 0.0)), _shader_mat(OCCLUSION_PLAIN_SHADER))
	_add_inst("OcclusionPDO", _plane_mesh, Transform3D(face_cam.scaled(Vector3(1.5, 1.0, 1.0)), Vector3(1.5, -1.00, 0.20)), _shader_mat(OCCLUSION_PDO_SHADER))


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


func _bits_to_float(p_bits: int) -> float:
	var b := PackedByteArray()
	b.resize(4)
	b.encode_s32(0, p_bits)
	return b.decode_float(0)


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


func _run() -> void:
	if not _failures.is_empty():
		_finish()
		return

	# Check (a): Forward+ DEPTH_OFFSET = 0.0 (and negative clamped -0.25) byte-identity vs no DEPTH_OFFSET.
	await _test_forward_zero_byte_identity()

	# Check (b) & (c): NextGeo HW VS, SW, VIS64 buffer, and HW Mesh depth parity (<= 1e-5) + occlusion ordering.
	await _test_nextgeo_depth_and_occlusion()

	# Check (d): VSM PDO shadows (directional + spot) across non-Nanite VSM, NextGeo HW VS, SW, and HW Mesh.
	await _test_vsm_pdo_shadows()

	_finish()


func _test_forward_zero_byte_identity() -> void:
	# Keep virtualized Background visible so NextGeo textures exist for ProbeScript readback,
	# while hiding all foreground test instances so the non-virtualized sphere dominates the view.
	for inst_name: String in _instances:
		(_instances[inst_name] as MeshInstance3D).visible = (inst_name == "Background")

	# Use a non-virtualized sphere so this strictly tests Forward+ (scene_forward_clustered.glsl) with and without DEPTH_OFFSET.
	var sphere := SphereMesh.new()
	sphere.radius = 1.25
	sphere.height = 2.5
	sphere.radial_segments = 64
	sphere.rings = 32

	var inst := MeshInstance3D.new()
	inst.name = "ForwardZeroPDOTarget"
	inst.mesh = sphere
	inst.position = Vector3(0.0, 0.0, 0.0)
	add_child(inst)

	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 1)
	ProjectSettings.set_setting("rendering/lights_and_shadows/shadow_method", 0)

	# 1. Reference shader without DEPTH_OFFSET.
	inst.material_override = _shader_mat(PLAIN_REF_SHADER)
	await _frames(SETTLE_FRAMES)
	var cap_ref := await _capture_probe()
	var img_ref := get_viewport().get_texture().get_image()
	img_ref.convert(Image.FORMAT_RGBA8)
	var bytes_ref := img_ref.get_data()

	var zero_report := {}
	for pair in [["zero_pdo", ZERO_PDO_SHADER], ["negative_clamped_pdo", NEGATIVE_PDO_SHADER]]:
		var label: String = pair[0]
		inst.material_override = _shader_mat(pair[1])
		await _frames(SETTLE_FRAMES)
		var cap_cur := await _capture_probe()
		var img_cur := get_viewport().get_texture().get_image()
		img_cur.convert(Image.FORMAT_RGBA8)
		var bytes_cur := img_cur.get_data()

		var color_byte_diffs := 0
		if bytes_ref.size() != bytes_cur.size():
			color_byte_diffs = -1
		else:
			for i in bytes_ref.size():
				if bytes_ref[i] != bytes_cur[i]:
					color_byte_diffs += 1

		var depth_bit_diffs := 0
		var w_ref: PackedInt32Array = cap_ref["words"]
		var w_cur: PackedInt32Array = cap_cur["words"]
		var px_count := w_ref.size() / ProbeScript.WORDS_PER_PIXEL
		var nonzero_depth_px := 0
		for i in px_count:
			var b := i * ProbeScript.WORDS_PER_PIXEL
			if w_ref[b + 2] != 0:
				nonzero_depth_px += 1
			if w_ref[b + 2] != w_cur[b + 2]:
				depth_bit_diffs += 1

		zero_report[label] = {
			"color_byte_diffs": color_byte_diffs,
			"depth_bit_diffs": depth_bit_diffs,
			"nonzero_depth_px": nonzero_depth_px,
		}
		if nonzero_depth_px < 500:
			_failures.append("forward_%s: too few rendered pixels (%d)" % [label, nonzero_depth_px])
		if color_byte_diffs != 0:
			_failures.append("forward_%s: not byte-identical in RGBA8 color (%d differing bytes)" % [label, color_byte_diffs])
		if depth_bit_diffs != 0:
			_failures.append("forward_%s: not bit-identical in depth buffer (%d differing pixels)" % [label, depth_bit_diffs])

	_report["forward_byte_identity"] = zero_report

	inst.queue_free()
	await _frames(2)
	for inst_name: String in _instances:
		(_instances[inst_name] as MeshInstance3D).visible = true


func _test_nextgeo_depth_and_occlusion() -> void:
	# 1. Capture Forward+ legacy reference (raster_mode = 1 renders programmable surfaces through Forward+).
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 1)
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 8.0)
	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", false)
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
	await _frames(SETTLE_FRAMES)
	var cap_legacy := await _capture_probe()
	var img_legacy := get_viewport().get_texture().get_image()

	# 2. Switch to NextGeo default admission (debug_programmable_raster = false, raster_mode = 0).
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)

	var phases: Array[Dictionary] = [
		{"name": "hw_vs", "path": 1, "t": 0.0, "buf": false},
		{"name": "sw", "path": 1, "t": 32.0, "buf": false},
		{"name": "sw_buf", "path": 1, "t": 32.0, "buf": true},
	]
	if _expect_mesh:
		phases.append({"name": "hw_mesh", "path": 2, "t": 0.0, "buf": false})

	var caps := {}
	var imgs := {}
	var stats := {}
	var routes_checked := false

	for phase in phases:
		var pname: String = phase["name"]
		ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", int(phase["path"]))
		ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", float(phase["t"]))
		ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", bool(phase["buf"]))
		await _frames(SETTLE_FRAMES)

		if not routes_checked:
			routes_checked = true
			var routes := {}
			for inst_name: String in _instances:
				routes[inst_name] = _route_of(_instances[inst_name])
			_report["routes"] = routes
			var pdo_names := ["ConstPDO", "RampPDO", "TexPDO", "MaskedTwoSidedPDO", "CurvedSpherePDO", "OcclusionPDO"]
			var fixed_names := ["Background", "PlainTwin", "OcclusionPlain"]
			for n in fixed_names:
				var r: Array = routes[n]
				if r[0] != "virtual" or r[1] != "NONE":
					_failures.append("route/%s: got %s/%s expected virtual/NONE" % [n, r[0], r[1]])
			for n in pdo_names:
				var r: Array = routes[n]
				var exp_route := "virtual" if _expect_bins else "legacy"
				if r[0] != exp_route:
					_failures.append("route/%s: got %s/%s expected %s" % [n, r[0], r[1], exp_route])

		caps[pname] = await _capture_probe()
		imgs[pname] = get_viewport().get_texture().get_image()
		stats[pname] = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())

	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", false)
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)

	if not _expect_bins:
		return

	# Check stats across HW VS, SW, and HW Mesh.
	var st_hw: Dictionary = stats["hw_vs"]
	var st_sw: Dictionary = stats["sw"]
	if int(st_hw.get("programmable_hw_clusters", 0)) <= 0 or int(st_hw.get("programmable_sw_clusters", -1)) != 0:
		_failures.append("hw_vs: expected prog_hw > 0 and prog_sw == 0, got hw=%s sw=%s" % [str(st_hw.get("programmable_hw_clusters")), str(st_hw.get("programmable_sw_clusters"))])
	if int(st_sw.get("programmable_sw_clusters", 0)) <= 0:
		_failures.append("sw: expected prog_sw > 0, got %s" % str(st_sw.get("programmable_sw_clusters")))
	if _expect_mesh:
		var st_mesh: Dictionary = stats["hw_mesh"]
		if String(st_mesh.get("hw_raster_path", "")) != "mesh" or int(st_mesh.get("programmable_hw_clusters", 0)) <= 0:
			_failures.append("hw_mesh: expected path=mesh and prog_hw > 0, got path=%s prog_hw=%s" % [str(st_mesh.get("hw_raster_path")), str(st_mesh.get("programmable_hw_clusters"))])

	# Verify depth offset actually shifts depth relative to PlainTwin (in reversed-Z, smaller depth = farther away).
	var plain_twin_rec := int(_records["PlainTwin"])
	var const_pdo_rec := int(_records["ConstPDO"])
	var occ_pdo_rec := int(_records["OcclusionPDO"])
	var occ_plain_rec := int(_records["OcclusionPlain"])

	var legacy_depth_words: PackedInt32Array = cap_legacy["words"]
	var size: Vector2i = cap_legacy["size"]

	var phase_results := {}
	for phase in phases:
		var pname: String = phase["name"]
		var cap: Dictionary = caps[pname]
		var words: PackedInt32Array = cap["words"]
		var img: Image = imgs[pname]

		# (1) Depth error vs Forward+ legacy reference across interior pixels (excluding 1px silhouette transitions).
		var pdo_rec_names := {
			int(_records["ConstPDO"]): "ConstPDO",
			int(_records["RampPDO"]): "RampPDO",
			int(_records["TexPDO"]): "TexPDO",
			int(_records["MaskedTwoSidedPDO"]): "MaskedTwoSidedPDO",
			occ_pdo_rec: "OcclusionPDO",
		}
		var per_inst_max_err := {}
		var max_depth_err := 0.0
		var interior_px := 0
		var plain_twin_depth_sum := 0.0
		var plain_twin_px := 0
		var const_pdo_depth_sum := 0.0
		var const_pdo_px := 0

		for y in range(1, size.y - 1):
			for x in range(1, size.x - 1):
				var idx := (y * size.x + x) * ProbeScript.WORDS_PER_PIXEL
				var rec := words[idx]
				var d_cur := _bits_to_float(words[idx + 2])
				if rec == plain_twin_rec:
					plain_twin_depth_sum += d_cur
					plain_twin_px += 1
				elif rec == const_pdo_rec:
					const_pdo_depth_sum += d_cur
					const_pdo_px += 1

				if not pdo_rec_names.has(rec):
					continue
				# Only compare strictly interior pixels where the 3x3 neighborhood has the same record ID,
				# avoiding 1-px silhouette/intersection edge tie-breaking differences.
				var interior := true
				for dy: int in [-1, 0, 1]:
					for dx: int in [-1, 0, 1]:
						var nidx: int = ((y + dy) * size.x + (x + dx)) * ProbeScript.WORDS_PER_PIXEL
						if words[nidx] != rec:
							interior = false
							break
					if not interior:
						break
				if not interior:
					continue

				var d_leg := _bits_to_float(legacy_depth_words[idx + 2])
				var err := absf(d_cur - d_leg)
				var rname: String = pdo_rec_names[rec]
				if err > float(per_inst_max_err.get(rname, 0.0)):
					per_inst_max_err[rname] = err
				if err > max_depth_err:
					max_depth_err = err
				interior_px += 1

		# (2) Occlusion ordering check on the intersecting OcclusionPDO / OcclusionPlain region:
		# Compare red (PDO) vs blue (Plain) pixel winners against Forward+ legacy image!
		var img_w := img.get_width()
		var img_h := img.get_height()
		var leg_red := 0
		var leg_blue := 0
		var cur_red := 0
		var cur_blue := 0
		var occ_agree := 0
		var occ_total := 0
		var color_err_sum := 0.0
		var color_px := 0

		for y in img_h:
			for x in img_w:
				var cl := img_legacy.get_pixel(x, y)
				var cc := img.get_pixel(x, y)
				color_err_sum += (absf(cl.r - cc.r) + absf(cl.g - cc.g) + absf(cl.b - cc.b)) / 3.0
				color_px += 1

				var l_is_red := cl.r > 0.35 and cl.r > cl.b * 2.0 and cl.r > cl.g * 2.0
				var l_is_blue := cl.b > 0.35 and cl.b > cl.r * 2.0 and cl.b > cl.g * 2.0
				var c_is_red := cc.r > 0.35 and cc.r > cc.b * 2.0 and cc.r > cc.g * 2.0
				var c_is_blue := cc.b > 0.35 and cc.b > cc.r * 2.0 and cc.b > cc.g * 2.0
				if l_is_red:
					leg_red += 1
				elif l_is_blue:
					leg_blue += 1
				if c_is_red:
					cur_red += 1
				elif c_is_blue:
					cur_blue += 1
				if l_is_red or l_is_blue or c_is_red or c_is_blue:
					occ_total += 1
					if (l_is_red and c_is_red) or (l_is_blue and c_is_blue):
						occ_agree += 1

		var occ_agree_frac := float(occ_agree) / float(maxi(occ_total, 1))
		var color_mae := color_err_sum / float(maxi(color_px, 1))
		var plain_mean_d := plain_twin_depth_sum / float(maxi(plain_twin_px, 1))
		var const_mean_d := const_pdo_depth_sum / float(maxi(const_pdo_px, 1))

		phase_results[pname] = {
			"interior_pdo_px": interior_px,
			"max_depth_err": max_depth_err,
			"per_inst_max_err": per_inst_max_err,
			"plain_twin_depth": snappedf(plain_mean_d, 1e-6),
			"const_pdo_depth": snappedf(const_mean_d, 1e-6),
			"color_mae": snappedf(color_mae, 1e-6),
			"occ_leg_red": leg_red,
			"occ_leg_blue": leg_blue,
			"occ_cur_red": cur_red,
			"occ_cur_blue": cur_blue,
			"occ_agree_frac": snappedf(occ_agree_frac, 1e-5),
		}

		if interior_px < 1000:
			_failures.append("%s: too few interior PDO pixels (%d)" % [pname, interior_px])
		if max_depth_err > MAX_DEPTH_ERR:
			_failures.append("%s: max depth error %.7f exceeds 1e-5 (per_inst=%s)" % [pname, max_depth_err, str(per_inst_max_err)])
		if const_mean_d >= plain_mean_d - 0.0005:
			_failures.append("%s: ConstPDO depth (%.6f) was not pushed behind PlainTwin (%.6f)" % [pname, const_mean_d, plain_mean_d])
		if color_mae > 0.015:
			_failures.append("%s: shaded color MAE %.5f exceeds 0.015 vs Forward+" % [pname, color_mae])
		if leg_red < 200 or leg_blue < 200 or cur_red < 200 or cur_blue < 200:
			_failures.append("%s: intersecting occlusion pair missing PDO or Plain half (leg=%d/%d cur=%d/%d)" % [pname, leg_red, leg_blue, cur_red, cur_blue])
		if occ_agree_frac < 0.97:
			_failures.append("%s: occlusion ordering agreement %.4f < 0.97 vs Forward+" % [pname, occ_agree_frac])

	# Verify VIS64 buffer storage is bit-identical to VIS64 image storage in SW.
	var w_sw: PackedInt32Array = caps["sw"]["words"]
	var w_buf: PackedInt32Array = caps["sw_buf"]["words"]
	var buf_diff := 0
	for i in w_sw.size() / ProbeScript.WORDS_PER_PIXEL:
		var b := i * ProbeScript.WORDS_PER_PIXEL
		if w_sw[b] != w_buf[b] or (w_sw[b + 1] & 0x7F) != (w_buf[b + 1] & 0x7F) or w_sw[b + 2] != w_buf[b + 2]:
			buf_diff += 1
	if buf_diff != 0:
		_failures.append("sw_buf: %d pixels differ from VIS64 image mode" % buf_diff)

	_report["nextgeo_depth_and_occlusion"] = phase_results


func _test_vsm_pdo_shadows() -> void:
	for inst_name: String in _instances:
		(_instances[inst_name] as MeshInstance3D).visible = false
	_sun.visible = false
	_camera.current = false

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

	# Virtualized caster AND plain (non-virtual) caster with identical geometry + half-PDO shader.
	# Placed at (-1.2, 0.6, 0.0) with light shining along (2, -1, 0) so the shadow lands at (0, 0, 0),
	# completely separate from the caster in the top-down view (slant distance to floor = 0.6*sqrt(5) = 1.34m).
	# Left half has DEPTH_OFFSET = 0.0 -> casts full shadow; Right half has DEPTH_OFFSET = 2.0m -> pushed behind floor -> NO shadow!
	var caster_prim := PlaneMesh.new()
	caster_prim.size = Vector2(1.0, 1.0)
	caster_prim.subdivide_width = 48
	caster_prim.subdivide_depth = 48
	var caster_vmesh := _virtualize(caster_prim, "vsm_pdo_caster")
	var mat_vsm_pdo := _shader_mat(VSM_HALF_PDO_CASTER_SHADER)

	var caster := MeshInstance3D.new()
	caster.mesh = caster_vmesh
	caster.material_override = mat_vsm_pdo
	caster.position = Vector3(-1.2, 0.6, 0.0)
	caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	add_child(caster)

	var vsm_out := {}

	# --- A. Directional light (sun along (2, -1, 0); projects (-1.2, 0.6, 0) onto floor at (0, 0, 0)) ---
	# Capture unshadowed reference luminance.
	ProjectSettings.set_setting("rendering/lights_and_shadows/shadow_method", 0)
	caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	var dir_ref_light := _make_vsm_sun(false)
	await _frames(VSM_SETTLE_FRAMES)
	var dir_unshadowed := _capture_subviewport_lum(sv)
	dir_ref_light.queue_free()
	await _frames(2)

	# Capture Forward+ classic PCF shadow map reference (non-virtualized mesh, shadow_method = 0).
	caster.mesh = caster_prim
	caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	var dir_pcf := _make_vsm_sun(false)
	await _frames(VSM_SETTLE_FRAMES)
	var dir_pcf_lum := _capture_subviewport_lum(sv)
	dir_pcf.queue_free()
	await _frames(2)

	var pcf_halves := _vsm_left_right_shadow_fractions(dir_unshadowed, dir_pcf_lum, Vector3(0.0, 0.0, 0.0), 0.5)
	vsm_out["dir_forward_pcf"] = pcf_halves
	if float(pcf_halves["left"]) < 0.85 or float(pcf_halves["right"]) > 0.10:
		_failures.append("dir_forward_pcf: expected left shadowed (>0.85) and right pushed-past (<0.10), got %s" % str(pcf_halves))

	# Enable VSM (`shadow_method = 1`) and test:
	# 1. Forward+ non-Nanite VSM (plain mesh)
	# 2. NextGeo VSM HW VS (`path=1, sw_max=0`)
	# 3. NextGeo VSM SW (`path=1, sw_max=24`)
	# 4. NextGeo VSM HW Mesh (`path=2, sw_max=0`, when supported)
	ProjectSettings.set_setting("rendering/lights_and_shadows/shadow_method", 1)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", false)

	var dir_cases: Array[Dictionary] = [
		{"name": "dir_vsm_non_nanite", "virtual": false, "path": 1, "sw_max": 0.0, "expect": "non_nanite"},
		{"name": "dir_vsm_nextgeo_hw_vs", "virtual": true, "path": 1, "sw_max": 0.0, "expect": "hw"},
		{"name": "dir_vsm_nextgeo_sw", "virtual": true, "path": 1, "sw_max": 24.0, "expect": "sw"},
	]
	if _expect_mesh:
		dir_cases.append({"name": "dir_vsm_nextgeo_hw_mesh", "virtual": true, "path": 2, "sw_max": 0.0, "expect": "mesh"})

	for c in dir_cases:
		var cname: String = c["name"]
		caster.mesh = caster_vmesh if bool(c["virtual"]) else caster_prim
		ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", int(c["path"]))
		ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", float(c["sw_max"]))
		var sun := _make_vsm_sun(true)
		var peak := await _settle_vsm_peak(VSM_SETTLE_FRAMES)
		var lit_lum := _capture_subviewport_lum(sv)
		sun.queue_free()
		await _frames(2)

		var halves := _vsm_left_right_shadow_fractions(dir_unshadowed, lit_lum, Vector3(0.0, 0.0, 0.0), 0.5)
		var mae := _lum_mae_box(dir_pcf_lum, lit_lum, Vector3(0.0, 0.0, 0.0), 0.5)
		vsm_out[cname] = {
			"left": halves["left"],
			"right": halves["right"],
			"mae_vs_forward_pcf": snappedf(mae, 1e-5),
			"prog_hw": int(peak["prog_hw_clusters"]),
			"prog_sw": int(peak["prog_sw_clusters"]),
			"dynamic": int(peak["dynamic_clusters"]),
			"overflow": int(peak["overflow"]),
			"sink": int(peak["sink"]),
		}
		if float(halves["left"]) < 0.85 or float(halves["right"]) > 0.10:
			_failures.append("%s: expected left shadowed (>0.85) and right pushed-past (<0.10), got %s" % [cname, str(halves)])
		if mae > 0.04:
			_failures.append("%s: MAE vs Forward+ PCF %.4f > 0.04" % [cname, mae])
		if not bool(peak["vsm_active"]):
			_failures.append("%s: VSM inactive" % cname)
		if int(peak["overflow"]) != 0 or int(peak["sink"]) != 0:
			_failures.append("%s: VSM overflow=%d sink=%d" % [cname, int(peak["overflow"]), int(peak["sink"])])
		var expect_mode: String = c["expect"]
		if expect_mode != "non_nanite" and _expect_bins:
			if not bool(peak["nanite_active"]) or not bool(peak["gpu_counters_valid"]):
				_failures.append("%s: Nanite VSM inactive or counters invalid" % cname)
			if int(peak["dynamic_clusters"]) <= 0:
				_failures.append("%s: expected dynamic_clusters > 0 for PDO caster (#212), got %d" % [cname, int(peak["dynamic_clusters"])])
			if (expect_mode == "hw" or expect_mode == "mesh") and int(peak["prog_hw_clusters"]) <= 0:
				_failures.append("%s: expected prog_hw_clusters > 0, got %d" % [cname, int(peak["prog_hw_clusters"])])
			if expect_mode == "sw" and int(peak["prog_sw_clusters"]) <= 0:
				_failures.append("%s: expected prog_sw_clusters > 0, got %d" % [cname, int(peak["prog_sw_clusters"])])
			if expect_mode == "mesh" and String(peak["hw_raster_path"]) != "mesh":
				_failures.append("%s: expected VSM hw_raster_path=mesh, got %s" % [cname, String(peak["hw_raster_path"])])

	# --- B. Spot light (local perspective VSM map) ---
	# Spot at (-2.4, 1.2, 0.0) shining at (0, 0, 0), caster at (-1.2, 0.6, 0.0) (halfway along the ray).
	# Caster spans x in [-1.7, -0.7]; its perspective shadow spans x in [-1.0, +1.0], so x in [-0.5, +0.5] is clear.
	caster.position = Vector3(-1.2, 0.6, 0.0)

	ProjectSettings.set_setting("rendering/lights_and_shadows/shadow_method", 0)
	caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	var spot_ref_light := _make_vsm_spot(false)
	await _frames(VSM_SETTLE_FRAMES)
	var spot_unshadowed := _capture_subviewport_lum(sv)
	spot_ref_light.queue_free()
	await _frames(2)

	caster.mesh = caster_prim
	caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	var spot_pcf := _make_vsm_spot(false)
	await _frames(VSM_SETTLE_FRAMES)
	var spot_pcf_lum := _capture_subviewport_lum(sv)
	spot_pcf.queue_free()
	await _frames(2)

	var spot_pcf_halves := _vsm_left_right_shadow_fractions(spot_unshadowed, spot_pcf_lum, Vector3(0.0, 0.0, 0.0), 0.5)
	vsm_out["spot_forward_pcf"] = spot_pcf_halves
	if float(spot_pcf_halves["left"]) < 0.85 or float(spot_pcf_halves["right"]) > 0.10:
		_failures.append("spot_forward_pcf: expected left shadowed (>0.85) and right pushed-past (<0.10), got %s" % str(spot_pcf_halves))

	ProjectSettings.set_setting("rendering/lights_and_shadows/shadow_method", 1)
	for pair in [["spot_vsm_nextgeo_hw_vs", 1, 0.0], ["spot_vsm_nextgeo_sw", 1, 128.0]]:
		var cname: String = pair[0]
		caster.mesh = caster_vmesh
		ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", int(pair[1]))
		ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", float(pair[2]))
		var spot := _make_vsm_spot(true)
		var peak := await _settle_vsm_peak(VSM_SETTLE_FRAMES)
		var lit_lum := _capture_subviewport_lum(sv)
		spot.queue_free()
		await _frames(2)

		var halves := _vsm_left_right_shadow_fractions(spot_unshadowed, lit_lum, Vector3(0.0, 0.0, 0.0), 0.5)
		var mae := _lum_mae_box(spot_pcf_lum, lit_lum, Vector3(0.0, 0.0, 0.0), 0.5)
		vsm_out[cname] = {
			"left": halves["left"],
			"right": halves["right"],
			"mae_vs_forward_pcf": snappedf(mae, 1e-5),
			"prog_hw": int(peak["prog_hw_clusters"]),
			"prog_sw": int(peak["prog_sw_clusters"]),
			"dynamic": int(peak["dynamic_clusters"]),
			"overflow": int(peak["overflow"]),
			"sink": int(peak["sink"]),
		}
		if float(halves["left"]) < 0.85 or float(halves["right"]) > 0.10:
			_failures.append("%s: expected left shadowed (>0.85) and right pushed-past (<0.10), got %s" % [cname, str(halves)])
		if mae > 0.05:
			_failures.append("%s: MAE vs Forward+ PCF %.4f > 0.05" % [cname, mae])
		if int(peak["overflow"]) != 0 or int(peak["sink"]) != 0:
			_failures.append("%s: VSM overflow=%d sink=%d" % [cname, int(peak["overflow"]), int(peak["sink"])])
		if _expect_bins and int(peak["dynamic_clusters"]) <= 0:
			_failures.append("%s: expected dynamic_clusters > 0 for PDO spot caster" % cname)

	_report["vsm_pdo"] = vsm_out

	caster.queue_free()
	floor_inst.queue_free()
	sv.queue_free()
	await _frames(2)


func _make_vsm_sun(p_virtual: bool) -> DirectionalLight3D:
	var sun := DirectionalLight3D.new()
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_ORTHOGONAL
	sun.directional_shadow_max_distance = 12.0
	sun.shadow_enabled = true
	sun.shadow_method = Light3D.SHADOW_METHOD_VIRTUAL if p_virtual else Light3D.SHADOW_METHOD_ATLAS
	sun.transform = Transform3D(Basis.looking_at(Vector3(2.0, -1.0, 0.0).normalized(), Vector3.UP), Vector3(0.0, 5.0, 0.0))
	add_child(sun)
	return sun


func _make_vsm_spot(p_virtual: bool) -> SpotLight3D:
	var spot := SpotLight3D.new()
	spot.spot_angle = 55.0
	spot.spot_range = 10.0
	spot.light_energy = 5.0
	spot.shadow_enabled = true
	spot.shadow_method = Light3D.SHADOW_METHOD_VIRTUAL if p_virtual else Light3D.SHADOW_METHOD_ATLAS
	spot.transform = Transform3D(Basis.looking_at(Vector3(2.0, -1.0, 0.0).normalized(), Vector3.UP), Vector3(-2.4, 1.2, 0.0))
	add_child(spot)
	return spot


func _settle_vsm_peak(p_frames: int) -> Dictionary:
	var peak := {
		"vsm_active": false,
		"nanite_active": false,
		"gpu_counters_valid": false,
		"hw_raster_path": "",
		"prog_sw_clusters": 0,
		"prog_hw_clusters": 0,
		"dynamic_clusters": 0,
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
			peak["hw_raster_path"] = String(n.get("hw_raster_path", ""))
		if bool(n.get("gpu_counters_valid", false)):
			peak["gpu_counters_valid"] = true
			for k in ["prog_sw_clusters", "prog_hw_clusters", "dynamic_clusters", "overflow", "sink"]:
				peak[k] = maxi(int(peak[k]), int(n.get(k, 0)))
	return peak


func _capture_subviewport_lum(p_sv: SubViewport) -> PackedFloat32Array:
	var img := p_sv.get_texture().get_image()
	img.convert(Image.FORMAT_RF)
	return img.get_data().to_float32_array()


func _vsm_left_right_shadow_fractions(p_unshadowed: PackedFloat32Array, p_lit: PackedFloat32Array, p_center: Vector3, p_half: float) -> Dictionary:
	var left_in := 0
	var left_dark := 0
	var right_in := 0
	var right_dark := 0
	var margin := 0.12
	for py in VSM_SIZE:
		var wz := ((float(py) + 0.5) / float(VSM_SIZE) - 0.5) * VSM_EXTENT
		if wz < p_center.z - p_half + margin or wz > p_center.z + p_half - margin:
			continue
		var row := py * VSM_SIZE
		for px in VSM_SIZE:
			var wx := ((float(px) + 0.5) / float(VSM_SIZE) - 0.5) * VSM_EXTENT
			if wx < p_center.x - p_half + margin or wx > p_center.x + p_half - margin:
				continue
			if absf(wx - p_center.x) < margin:
				continue
			var r := p_unshadowed[row + px]
			if r < 0.02:
				continue
			var shadowed := (p_lit[row + px] / r) < 0.5
			if wx < p_center.x:
				left_in += 1
				if shadowed:
					left_dark += 1
			else:
				right_in += 1
				if shadowed:
					right_dark += 1
	return {
		"left": snappedf(float(left_dark) / float(maxi(left_in, 1)), 1e-4),
		"right": snappedf(float(right_dark) / float(maxi(right_in, 1)), 1e-4),
		"left_px": left_in,
		"right_px": right_in,
	}


func _lum_mae_box(p_ref: PackedFloat32Array, p_tst: PackedFloat32Array, p_center: Vector3, p_half: float) -> float:
	var sum := 0.0
	var count := 0
	var margin := 0.12
	for py in VSM_SIZE:
		var wz := ((float(py) + 0.5) / float(VSM_SIZE) - 0.5) * VSM_EXTENT
		if wz < p_center.z - p_half + margin or wz > p_center.z + p_half - margin:
			continue
		var row := py * VSM_SIZE
		for px in VSM_SIZE:
			var wx := ((float(px) + 0.5) / float(VSM_SIZE) - 0.5) * VSM_EXTENT
			if wx < p_center.x - p_half + margin or wx > p_center.x + p_half - margin:
				continue
			if absf(wx - p_center.x) < margin:
				continue
			sum += absf(p_ref[row + px] - p_tst[row + px])
			count += 1
	return sum / float(maxi(count, 1))


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
