extends Node3D
## NextGeo displacement material output check (#150, plan §21.4 N11).
##
## Verifies on GPU (Vulkan and D3D12):
## 1. Custom spatial shaders with `displacement()` (using VERTEX, NORMAL, UV, UV2, COLOR,
##    INSTANCE_CUSTOM, TIME, uniforms, textures, helper functions) and StandardMaterial3D with
##    `heightmap_mode = HEIGHTMAP_MODE_DISPLACEMENT` compile and render cleanly with 0 errors.
## 2. `displacement_magnitude = 0.0` renders byte-identical to the identical material without
##    `displacement()` (both custom ShaderMaterial and StandardMaterial3D), and routes identically.
## 3. `displacement_magnitude > 0.0` sets RASTER_FEATURE_DISPLACED (acquiring a programmable raster
##    bin under `debug_programmable_raster = true` on Nanite-tier devices), inflates cull bounds
##    (bringing a just-outside-frustum instance into the frustum while preserving `cut_match`),
##    and rejects FRAGCOORD-using displaced shaders with UNSUPPORTED_RASTER_FEATURE.
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const SETTLE_FRAMES := 16

const PLAIN_SHADER := """
shader_type spatial;
uniform sampler2D height_tex;
uniform vec2 uv_scale = vec2(2.0, 2.0);
uniform float bias = 0.05;
void fragment() {
	ALBEDO = vec3(0.7, 0.35, 0.25);
	ROUGHNESS = 0.6;
}
"""

const DISP_SHADER := """
shader_type spatial;
uniform sampler2D height_tex;
uniform vec2 uv_scale = vec2(2.0, 2.0);
uniform float bias = 0.05;
float sample_h(vec2 uv) {
	return texture(height_tex, uv * uv_scale).r;
}
void displacement() {
	float h = sample_h(UV) + texture(height_tex, UV2).r * 0.25;
	float w = sin(dot(VERTEX, NORMAL) + TIME) * 0.02 + COLOR.r * 0.01 + INSTANCE_CUSTOM.x * 0.01 + bias;
	DISPLACEMENT = clamp(h + w, 0.0, 1.0);
}
void fragment() {
	ALBEDO = vec3(0.7, 0.35, 0.25);
	ROUGHNESS = 0.6;
}
"""

const DISP_FRAGCOORD_SHADER := """
shader_type spatial;
uniform sampler2D height_tex;
void displacement() {
	DISPLACEMENT = texture(height_tex, UV).r;
}
void fragment() {
	ALBEDO = vec3(0.25 + 0.5 * fract(FRAGCOORD.x * 0.0625), 0.5, 0.7);
}
"""

const PLAIN_FRAGCOORD_SHADER := """
shader_type spatial;
uniform sampler2D height_tex;
void fragment() {
	ALBEDO = vec3(0.25 + 0.5 * fract(FRAGCOORD.x * 0.0625), 0.7, 0.4);
}
"""

var _phases: Array[Dictionary] = [
	{ "name": "no_disp", "use_disp": false, "mag": 0.0, "offscreen_mag": 0.0, "bins": false, "t": 8.0 },
	{ "name": "mag_zero", "use_disp": true, "mag": 0.0, "offscreen_mag": 0.0, "bins": false, "t": 8.0 },
	{ "name": "mag_pos_fixed", "use_disp": true, "mag": 0.3, "offscreen_mag": 1.2, "bins": false, "t": 8.0 },
	{ "name": "mag_pos_bins_hw", "use_disp": true, "mag": 0.3, "offscreen_mag": 1.2, "bins": true, "t": 0.0 },
	{ "name": "mag_pos_bins_sw", "use_disp": true, "mag": 0.3, "offscreen_mag": 1.2, "bins": true, "t": 32.0 },
]

var _height_tex: ImageTexture
var _plain_shader_mat: ShaderMaterial
var _disp_shader_mat: ShaderMaterial
var _plain_std_mat: StandardMaterial3D
var _disp_std_mat: StandardMaterial3D
var _parallax_std_mat: StandardMaterial3D
var _fragcoord_disp_mat: ShaderMaterial
var _fragcoord_plain_mat: ShaderMaterial
var _offscreen_mat: StandardMaterial3D

var _inst_shader: MeshInstance3D
var _inst_std: MeshInstance3D
var _inst_parallax: MeshInstance3D
var _inst_fragcoord: MeshInstance3D
var _inst_fragcoord_plain: MeshInstance3D
var _inst_offscreen: MeshInstance3D
var _camera: Camera3D

var _phase_idx := 0
var _phase_frame := 0
var _images: Dictionary = {}
var _stats: Dictionary = {}
var _routes: Dictionary = {}
var _offscreen_in_frustum: Dictionary = {}
var _failures: Array[String] = []
var _report: Dictionary = {}
var _expect_bins := false


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/debug_cpu_parity", true)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
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
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 0.5

	_build_scene()
	_apply_phase()


func _make_height_texture() -> ImageTexture:
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			var fx := float(x) / 63.0
			var fy := float(y) / 63.0
			var h := 0.5 + 0.5 * sin(fx * TAU * 2.0) * cos(fy * TAU * 2.0)
			img.set_pixel(x, y, Color(h, h, h, 1.0))
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


func _virtualize(p_mesh: PrimitiveMesh, p_ngm_path: String) -> Mesh:
	var importer := ImporterMesh.from_mesh(p_mesh)
	var err := importer.build_virtual_geometry(p_ngm_path)
	var mesh := importer.get_mesh()
	if err != OK or mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(%s) failed (err=%d)" % [p_ngm_path, err])
	return mesh


func _shader_mat(p_code: String) -> ShaderMaterial:
	var sh := Shader.new()
	sh.code = p_code
	var mat := ShaderMaterial.new()
	mat.shader = sh
	mat.set_shader_parameter("height_tex", _height_tex)
	return mat


func _build_scene() -> void:
	_height_tex = _make_height_texture()

	var sphere := SphereMesh.new()
	sphere.radius = 0.65
	sphere.height = 1.3
	sphere.radial_segments = 64
	sphere.rings = 32
	var sphere_mesh := _virtualize(sphere, "user://nextgeo_displacement_sphere.ngm")

	var box := BoxMesh.new()
	box.size = Vector3(1.0, 1.0, 1.0)
	var box_mesh := _virtualize(box, "user://nextgeo_displacement_box.ngm")

	_plain_shader_mat = _shader_mat(PLAIN_SHADER)
	_disp_shader_mat = _shader_mat(DISP_SHADER)
	_fragcoord_disp_mat = _shader_mat(DISP_FRAGCOORD_SHADER)
	_fragcoord_plain_mat = _shader_mat(PLAIN_FRAGCOORD_SHADER)

	_plain_std_mat = StandardMaterial3D.new()
	_plain_std_mat.albedo_color = Color(0.25, 0.65, 0.45)
	_plain_std_mat.roughness = 0.5

	_disp_std_mat = StandardMaterial3D.new()
	_disp_std_mat.albedo_color = Color(0.25, 0.65, 0.45)
	_disp_std_mat.roughness = 0.5
	_disp_std_mat.heightmap_enabled = true
	_disp_std_mat.heightmap_mode = BaseMaterial3D.HEIGHTMAP_MODE_DISPLACEMENT
	_disp_std_mat.heightmap_texture = _height_tex

	_parallax_std_mat = StandardMaterial3D.new()
	_parallax_std_mat.albedo_color = Color(0.45, 0.5, 0.75)
	_parallax_std_mat.heightmap_enabled = true
	_parallax_std_mat.heightmap_mode = BaseMaterial3D.HEIGHTMAP_MODE_PARALLAX
	_parallax_std_mat.heightmap_texture = _height_tex

	_offscreen_mat = StandardMaterial3D.new()
	_offscreen_mat.albedo_color = Color(0.9, 0.8, 0.2)
	_offscreen_mat.heightmap_enabled = true
	_offscreen_mat.heightmap_mode = BaseMaterial3D.HEIGHTMAP_MODE_DISPLACEMENT
	_offscreen_mat.heightmap_texture = _height_tex

	_camera = Camera3D.new()
	_camera.fov = 60.0
	_camera.near = 0.05
	_camera.far = 100.0
	_camera.position = Vector3(0.0, 0.0, 4.5)
	_camera.current = true
	add_child(_camera)
	_camera.look_at(Vector3.ZERO)

	_inst_shader = _add("ShaderSphere", sphere_mesh, Transform3D(Basis(), Vector3(-1.3, 0.0, 0.0)))
	_inst_std = _add("StdBox", box_mesh, Transform3D(Basis(), Vector3(0.0, 0.0, 0.0)))
	_inst_parallax = _add("ParallaxBox", box_mesh, Transform3D(Basis(), Vector3(1.4, 0.0, 0.0)))
	_inst_parallax.material_override = _parallax_std_mat
	_inst_fragcoord = _add("FragcoordDispBox", box_mesh, Transform3D(Basis.from_scale(Vector3.ONE * 0.5), Vector3(-0.5, 1.35, 0.0)))
	_inst_fragcoord.material_override = _fragcoord_disp_mat
	_inst_fragcoord_plain = _add("FragcoordPlainBox", box_mesh, Transform3D(Basis.from_scale(Vector3.ONE * 0.5), Vector3(0.5, 1.35, 0.0)))
	_inst_fragcoord_plain.material_override = _fragcoord_plain_mat

	# Place offscreen box just outside the left frustum plane at its back face (z = -0.5, dist = 5.0 from camera at z = 4.5):
	# Un-inflated box has half-width 0.5, so placing center at -(back_half_w + 0.5 + 0.35) leaves a 0.35m gap
	# outside the frustum at magnitude 0, while displacement_magnitude = 1.2 inflates the AABB 0.85m inside the frustum.
	var aspect := float(get_viewport().get_visible_rect().size.x) / maxf(float(get_viewport().get_visible_rect().size.y), 1.0)
	var back_half_w := 5.0 * tan(deg_to_rad(30.0)) * aspect
	var offscreen_x := -(back_half_w + 0.5 + 0.35)
	_inst_offscreen = _add("OffscreenDispBox", box_mesh, Transform3D(Basis(), Vector3(offscreen_x, 0.0, 0.0)))
	_inst_offscreen.material_override = _offscreen_mat

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-30.0, 25.0, 0.0)
	sun.shadow_enabled = true
	add_child(sun)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.1, 0.12, 0.16)
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)


func _add(p_name: String, p_mesh: Mesh, p_xform: Transform3D) -> MeshInstance3D:
	var inst := MeshInstance3D.new()
	inst.name = p_name
	inst.mesh = p_mesh
	inst.transform = p_xform
	add_child(inst)
	return inst


func _route(p_inst: MeshInstance3D) -> Array:
	var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(p_inst.get_instance())
	var route := String(st.get("route", ""))
	var reason := String(st.get("reason", ""))
	var surfaces: Array = st.get("surfaces", [])
	if not surfaces.is_empty():
		var s0: Dictionary = surfaces[0]
		route = String(s0.get("route", ""))
		reason = String(s0.get("reason", ""))
	return [route, reason]


func _apply_phase() -> void:
	var phase: Dictionary = _phases[_phase_idx]
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", bool(phase["bins"]))
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", float(phase["t"]))

	var use_disp := bool(phase["use_disp"])
	var mag := float(phase["mag"])
	var off_mag := float(phase["offscreen_mag"])

	_disp_shader_mat.displacement_magnitude = mag
	_disp_shader_mat.displacement_center = 0.5
	_disp_std_mat.displacement_magnitude = mag
	_disp_std_mat.displacement_center = 0.5
	_fragcoord_disp_mat.displacement_magnitude = mag
	_fragcoord_plain_mat.displacement_magnitude = mag
	_offscreen_mat.displacement_magnitude = off_mag

	_inst_shader.material_override = null
	_inst_shader.material_override = _disp_shader_mat if use_disp else _plain_shader_mat
	_inst_std.material_override = null
	_inst_std.material_override = _disp_std_mat if use_disp else _plain_std_mat
	_inst_fragcoord.material_override = null
	_inst_fragcoord.material_override = _fragcoord_disp_mat
	_inst_fragcoord_plain.material_override = null
	_inst_fragcoord_plain.material_override = _fragcoord_plain_mat
	_inst_offscreen.material_override = null
	_inst_offscreen.material_override = _offscreen_mat

	_phase_frame = 0


func _process(_delta: float) -> void:
	if _phase_frame < SETTLE_FRAMES:
		_phase_frame += 1
		return

	var phase: Dictionary = _phases[_phase_idx]
	var pname: String = phase["name"]
	_images[pname] = get_viewport().get_texture().get_image()
	_stats[pname] = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	_routes[pname] = {
		"ShaderSphere": _route(_inst_shader),
		"StdBox": _route(_inst_std),
		"ParallaxBox": _route(_inst_parallax),
		"FragcoordDispBox": _route(_inst_fragcoord),
		"FragcoordPlainBox": _route(_inst_fragcoord_plain),
		"OffscreenDispBox": _route(_inst_offscreen),
	}

	var frustum_planes: Array[Plane] = _camera.get_frustum()
	var culled: PackedInt64Array = RenderingServer.instances_cull_convex(frustum_planes, get_world_3d().scenario)
	var off_id := _inst_offscreen.get_instance_id()
	_offscreen_in_frustum[pname] = culled.has(off_id)

	_phase_idx += 1
	if _phase_idx >= _phases.size():
		_finish()
		return
	_apply_phase()


func _compare_images(p_a: Image, p_b: Image) -> Dictionary:
	var ba: PackedByteArray = p_a.get_data()
	var bb: PackedByteArray = p_b.get_data()
	var diff_bytes := 0
	var max_delta := 0
	var n := mini(ba.size(), bb.size())
	for i in n:
		var d := absi(int(ba[i]) - int(bb[i]))
		if d != 0:
			diff_bytes += 1
			if d > max_delta:
				max_delta = d
	return {
		"same_size": ba.size() == bb.size() and p_a.get_size() == p_b.get_size(),
		"total_bytes": ba.size(),
		"diff_bytes": diff_bytes,
		"max_delta": max_delta,
	}


func _finish() -> void:
	# 1. Magnitude 0 renders byte-identical to the same materials without displacement().
	var cmp := _compare_images(_images["no_disp"], _images["mag_zero"])
	_report["mag_zero_vs_no_disp"] = cmp
	if not bool(cmp["same_size"]) or int(cmp["diff_bytes"]) != 0:
		_failures.append("mag_zero vs no_disp mismatch: diff_bytes=%d max_delta=%d" % [int(cmp["diff_bytes"]), int(cmp["max_delta"])])

	# 2. Check routing across all phases.
	_report["routes"] = _routes
	for pname in ["no_disp", "mag_zero"]:
		for inst_name in ["ShaderSphere", "StdBox", "ParallaxBox", "FragcoordDispBox", "FragcoordPlainBox"]:
			var r: Array = _routes[pname][inst_name]
			if r[0] != "virtual" or r[1] != "NONE":
				_failures.append("%s/%s: route=%s/%s expected virtual/NONE" % [pname, inst_name, r[0], r[1]])

	for pname in ["mag_pos_fixed", "mag_pos_bins_hw", "mag_pos_bins_sw"]:
		var r_fc: Array = _routes[pname]["FragcoordDispBox"]
		if r_fc[0] != "legacy" or r_fc[1] != "UNSUPPORTED_RASTER_FEATURE":
			_failures.append("%s/FragcoordDispBox: route=%s/%s expected legacy/UNSUPPORTED_RASTER_FEATURE" % [pname, r_fc[0], r_fc[1]])
		# A FRAGCOORD shader without displacement() must not gain RASTER_FEATURE_DISPLACED even if displacement_magnitude > 0.
		var r_fc_plain: Array = _routes[pname]["FragcoordPlainBox"]
		if r_fc_plain[0] != "virtual" or r_fc_plain[1] != "NONE":
			_failures.append("%s/FragcoordPlainBox: route=%s/%s expected virtual/NONE" % [pname, r_fc_plain[0], r_fc_plain[1]])
		for inst_name in ["ShaderSphere", "StdBox"]:
			var r: Array = _routes[pname][inst_name]
			if r[0] != "virtual" or r[1] != "NONE":
				_failures.append("%s/%s: route=%s/%s expected virtual/NONE" % [pname, inst_name, r[0], r[1]])

	# 3. Bound inflation: offscreen box is culled at mag=0, visible in frustum at mag=1.2.
	_report["offscreen_in_frustum"] = _offscreen_in_frustum
	if bool(_offscreen_in_frustum["no_disp"]) or bool(_offscreen_in_frustum["mag_zero"]):
		_failures.append("OffscreenDispBox should be outside frustum when displacement_magnitude == 0")
	if not bool(_offscreen_in_frustum["mag_pos_fixed"]) or not bool(_offscreen_in_frustum["mag_pos_bins_hw"]) or not bool(_offscreen_in_frustum["mag_pos_bins_sw"]):
		_failures.append("OffscreenDispBox should enter frustum when displacement_magnitude == 1.2")

	# 4. NextGeo stats: 0 double-dispatched / missing, cut_match == true, programmable bins active in mag_pos_bins.
	_report["stats"] = {}
	for phase in _phases:
		var pname: String = phase["name"]
		var st: Dictionary = _stats.get(pname, {})
		_report["stats"][pname] = {
			"double_dispatched": int(st.get("double_dispatched_surfaces", 0)),
			"missing": int(st.get("missing_surfaces", 0)),
			"cut_match": bool(st.get("cut_match", false)),
			"programmable_bins": int(st.get("programmable_bins", -1)),
			"programmable_sw_clusters": int(st.get("programmable_sw_clusters", -1)),
			"programmable_hw_clusters": int(st.get("programmable_hw_clusters", -1)),
			"phase1_instances": int(st.get("phase1_instances", -1)),
		}
		if st.is_empty():
			_failures.append("%s: missing NextGeo stats" % pname)
			continue
		if int(st.get("double_dispatched_surfaces", 0)) != 0 or int(st.get("missing_surfaces", 0)) != 0:
			_failures.append("%s: double_dispatched=%d missing=%d" % [pname, int(st.get("double_dispatched_surfaces", 0)), int(st.get("missing_surfaces", 0))])
		if st.has("cut_match") and not bool(st["cut_match"]):
			_failures.append("%s: NextGeo GPU vs CPU cut_match failed" % pname)
		var expect_p_bins := bool(phase["bins"]) and _expect_bins
		var p_bins := int(st.get("programmable_bins", 0))
		if expect_p_bins and p_bins < 2:
			_failures.append("%s: expected >= 2 programmable bins for displaced materials, got %d" % [pname, p_bins])
		elif not expect_p_bins and p_bins != 0:
			_failures.append("%s: expected 0 programmable bins, got %d" % [pname, p_bins])

	_report["pass"] = _failures.is_empty()
	_report["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(_report))
	get_tree().quit(0 if _failures.is_empty() else 1)
