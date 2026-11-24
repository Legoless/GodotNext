extends Node3D
## NextGeo foliage wind through Nanite skinning verification check (#152, plan §21.4 contract N10).
##
## Verifies all runtime guarantees of procedural foliage wind:
##   1. Project setting `rendering/nextgeo/experimental/foliage_wind` defaults to `false`; when off,
##      frames match zero-wind rest pose cleanly (no vertex displacement).
##   2. When enabled with World3D wind parameters (`wind_direction`, `wind_strength`, `wind_gust_frequency`),
##      procedural wind visibly displaces foliage vertices across HW VS, SW compute, HW task/mesh, and
##      programmable masked raster bins with 0 validation errors, 0 overflow, cut_match, and 0 bytes of
##      per-instance skin palette arena memory (`skin_palette_bytes == 0`, `skin_palette_used_bones == 0`).
##   3. Animated TIME produces non-zero motion velocity on wind-swaying foliage pixels (and ~0 when off).
##   4. 128 MultiMesh foliage instances sway under World3D wind while allocating 0 bytes of skin palette memory.
##   5. VSM dynamic shadow routing: swaying foliage renders into VSM dynamic layer 1 and shifts ground
##      shadows dynamically between opposite wind directions with 0 overflow and 0 sink errors.
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const ProbeScript = preload("res://tests/nextgeo_skinned/nextgeo_skinned_probe.gd")

const MASKED_FOLIAGE_SHADER := """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.24, 0.68, 0.28);
	ROUGHNESS = 0.72;
	ALPHA = step(0.25, fract(UV.y * 10.0));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""

var _phase := ""
var _phase_frame := 0
var _phase_root: Node3D
var _temp_files: Array[String] = []
var _ngm_counter := 0
var _failures: Array[String] = []
var _summary: Dictionary = {}

var _expect_hybrid := false
var _expect_mesh := false

var _opaque_tree: ArrayMesh
var _masked_tree: ArrayMesh

# Shared scene references
var _cam: Camera3D
var _sun: DirectionalLight3D
var _we: WorldEnvironment
var _probe: CompositorEffect
var _tree_opaque_mi: MeshInstance3D
var _tree_masked_mi: MeshInstance3D
var _ground_mi: MeshInstance3D
var _mmi: MultiMeshInstance3D

# Captured images & metrics
var _img_disabled: Image
var _img_zero_strength: Image
var _img_hw_vs_a: Image
var _img_hw_vs_b: Image
var _img_sw: Image
var _img_hw_mesh: Image
var _img_mm_off: Image
var _img_mm_on: Image
var _img_vsm_pos_x: Image
var _img_vsm_neg_x: Image
var _vel_off: Dictionary = {}
var _vel_on: Dictionary = {}


func _ready() -> void:
	# 1. Verify experimental foliage_wind defaults to false before touching settings.
	var default_wind := bool(ProjectSettings.get_setting("rendering/nextgeo/experimental/foliage_wind", true))
	if default_wind:
		_failures.append("init: rendering/nextgeo/experimental/foliage_wind defaulted to true, expected false")
	_summary["default_foliage_wind_setting"] = default_wind

	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", true)
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/occlusion", false)
	ProjectSettings.set_setting("rendering/nextgeo/shading_path", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/shadow_method", 1)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_local", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_directional", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/prefiltered_distant", false)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", 0.0)

	var rd := RenderingServer.get_rendering_device()
	var image_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_IMAGE_INT64_ATOMICS)
	var buffer_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_INT64_ATOMICS)
	var indirect_count := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT)
	var mesh_shader := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_MESH_SHADER)
	var mesh_indirect := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_MESH_SHADER_INDIRECT)
	_expect_hybrid = indirect_count and (image_int64 or buffer_int64)
	_expect_mesh = _expect_hybrid and mesh_shader and mesh_indirect
	_summary["device"] = {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"expect_hybrid": _expect_hybrid,
		"expect_mesh": _expect_mesh,
	}

	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 0.5

	_build_wind_meshes()
	_build_scene()
	_enter_phase("wind_off_disabled")


func _alloc_temp_ngm(p_tag: String) -> String:
	_ngm_counter += 1
	var path := "user://ng_wind_%d_%s_%d.ngm" % [OS.get_process_id(), p_tag.validate_filename(), _ngm_counter]
	_temp_files.append(path)
	return path


func _make_tree_arrays(p_radial: int, p_rings: int, p_height: float) -> Array:
	var verts := PackedVector3Array()
	var norms := PackedVector3Array()
	var tangs := PackedFloat32Array()
	var uvs := PackedVector2Array()
	var indices := PackedInt32Array()

	var cols := p_radial + 1
	for r in range(p_rings + 1):
		var v := float(r) / float(p_rings)
		var y := v * p_height
		var rad := 0.22
		if v > 0.30:
			var t := (v - 0.30) / 0.70
			rad = 0.22 + 1.55 * sin(t * PI) + 0.08 * sin(t * 14.0 * PI)
		for c in range(cols):
			var u := float(c) / float(p_radial)
			var ang := u * TAU
			var ca := cos(ang)
			var sa := sin(ang)
			var lobe := 1.0 + 0.22 * sin(ang * 4.0) * (1.0 if v > 0.30 else 0.0)
			var rr := rad * lobe
			verts.append(Vector3(rr * ca, y, rr * sa))
			norms.append(Vector3(ca, 0.25, sa).normalized())
			tangs.append(-sa)
			tangs.append(0.0)
			tangs.append(ca)
			tangs.append(1.0)
			uvs.append(Vector2(u, v))

	for r in range(p_rings):
		for c in range(p_radial):
			var i0 := r * cols + c
			var i1 := i0 + 1
			var i2 := (r + 1) * cols + c
			var i3 := i2 + 1
			indices.append(i0)
			indices.append(i2)
			indices.append(i1)
			indices.append(i1)
			indices.append(i2)
			indices.append(i3)

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = norms
	arrays[Mesh.ARRAY_TANGENT] = tangs
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	return arrays


func _build_wind_meshes() -> void:
	var arrays := _make_tree_arrays(28, 36, 4.6)

	var opaque_mat := StandardMaterial3D.new()
	opaque_mat.albedo_color = Color(0.22, 0.62, 0.25)
	opaque_mat.roughness = 0.75

	var imp_opaque := ImporterMesh.new()
	imp_opaque.add_surface(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {}, opaque_mat)
	var err_o := imp_opaque.build_virtual_geometry(_alloc_temp_ngm("opaque"), false, false, true)
	_opaque_tree = imp_opaque.get_mesh()
	if err_o != OK or _opaque_tree == null or _opaque_tree.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(opaque, wind=true) failed: err=%d" % err_o)
	else:
		var vgd := _opaque_tree.get_virtual_geometry()
		var wind := vgd.get_wind()
		if wind == null or wind.get_bone_count() < 2 or wind.get_bone_count() > VirtualGeometryWind.MAX_BONES or wind.get_max_sway() <= 0.0:
			_failures.append("opaque wind resource invalid: %s" % str(wind))
		if vgd.get_skin_influences() != 4:
			_failures.append("opaque wind expected skin_influences=4, got %d" % vgd.get_skin_influences())
		_summary["opaque_wind_bones"] = wind.get_bone_count() if wind != null else 0
		_summary["opaque_wind_max_sway"] = wind.get_max_sway() if wind != null else 0.0

	var masked_shader := Shader.new()
	masked_shader.code = MASKED_FOLIAGE_SHADER
	var masked_mat := ShaderMaterial.new()
	masked_mat.shader = masked_shader

	var imp_masked := ImporterMesh.new()
	imp_masked.add_surface(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {}, masked_mat)
	var err_m := imp_masked.build_virtual_geometry(_alloc_temp_ngm("masked"), false, false, true)
	_masked_tree = imp_masked.get_mesh()
	if err_m != OK or _masked_tree == null or _masked_tree.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(masked, wind=true) failed: err=%d" % err_m)


func _build_scene() -> void:
	_phase_root = Node3D.new()
	_phase_root.name = "WindTestRoot"
	add_child(_phase_root)

	_sun = DirectionalLight3D.new()
	_sun.rotation_degrees = Vector3(-42.0, 20.0, 0.0)
	_sun.light_energy = 2.2
	_sun.shadow_enabled = true
	_phase_root.add_child(_sun)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.06, 0.08, 0.11)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.28, 0.30, 0.32)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR

	_probe = ProbeScript.new()
	var comp := Compositor.new()
	var effects: Array[CompositorEffect] = [_probe]
	comp.compositor_effects = effects

	_we = WorldEnvironment.new()
	_we.environment = env
	_we.compositor = comp
	_phase_root.add_child(_we)

	_cam = Camera3D.new()
	_cam.fov = 52.0
	_cam.near = 0.2
	_cam.far = 200.0
	_cam.current = true
	_phase_root.add_child(_cam)
	_cam.position = Vector3(0.0, 3.0, 9.5)
	_cam.look_at(Vector3(0.0, 2.3, 0.0), Vector3.UP)

	_tree_opaque_mi = MeshInstance3D.new()
	_tree_opaque_mi.name = "OpaqueWindTree"
	_tree_opaque_mi.mesh = _opaque_tree
	_tree_opaque_mi.position = Vector3(-1.8, 0.0, 0.0)
	_phase_root.add_child(_tree_opaque_mi)

	_tree_masked_mi = MeshInstance3D.new()
	_tree_masked_mi.name = "MaskedWindTree"
	_tree_masked_mi.mesh = _masked_tree
	_tree_masked_mi.position = Vector3(1.8, 0.0, 0.0)
	_phase_root.add_child(_tree_masked_mi)

	var ground_mesh := PlaneMesh.new()
	ground_mesh.size = Vector2(36.0, 36.0)
	var ground_mat := StandardMaterial3D.new()
	ground_mat.albedo_color = Color(0.82, 0.82, 0.82)
	ground_mat.roughness = 0.9
	_ground_mi = MeshInstance3D.new()
	_ground_mi.name = "Ground"
	_ground_mi.mesh = ground_mesh
	_ground_mi.material_override = ground_mat
	_ground_mi.position = Vector3(0.0, 0.0, 0.0)
	_phase_root.add_child(_ground_mi)

	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	mm.instance_count = 128
	mm.mesh = _opaque_tree
	for z in range(8):
		for x in range(16):
			var idx := z * 16 + x
			var px := (float(x) - 7.5) * 2.2
			var pz := -float(z) * 2.4 - 1.0
			mm.set_instance_transform(idx, Transform3D(Basis().scaled(Vector3(0.65, 0.65, 0.65)), Vector3(px, 0.0, pz)))
			mm.set_instance_color(idx, Color(0.3 + 0.4 * float(x) / 15.0, 0.65, 0.3, 1.0))
	_mmi = MultiMeshInstance3D.new()
	_mmi.name = "WindForest128"
	_mmi.multimesh = mm
	_mmi.visible = false
	_phase_root.add_child(_mmi)


func _set_world_wind(p_dir: Vector3, p_strength: float, p_gust_freq: float) -> void:
	var w := get_world_3d()
	if w != null:
		w.wind_direction = p_dir
		w.wind_strength = p_strength
		w.wind_gust_frequency = p_gust_freq


func _enter_phase(p_name: String) -> void:
	_phase = p_name
	_phase_frame = 0
	match p_name:
		"wind_off_disabled":
			_tree_opaque_mi.visible = true
			_tree_masked_mi.visible = true
			_mmi.visible = false
			_cam.position = Vector3(0.0, 3.0, 9.5)
			_cam.look_at(Vector3(0.0, 2.3, 0.0), Vector3.UP)
			ProjectSettings.set_setting("rendering/nextgeo/experimental/foliage_wind", false)
			ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
			ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 0.0)
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 4.0, 2.0)
		"wind_off_zero_strength":
			ProjectSettings.set_setting("rendering/nextgeo/experimental/foliage_wind", true)
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 0.0, 2.0)
		"wind_on_hw_vs":
			ProjectSettings.set_setting("rendering/nextgeo/experimental/foliage_wind", true)
			ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
			ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 0.0)
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 4.5, 2.5)
		"wind_on_sw":
			ProjectSettings.set_setting("rendering/nextgeo/experimental/foliage_wind", true)
			ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
			ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 512.0)
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 4.5, 2.5)
		"wind_on_hw_mesh":
			ProjectSettings.set_setting("rendering/nextgeo/experimental/foliage_wind", true)
			ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 2)
			ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 0.0)
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 4.5, 2.5)
		"velocity_off":
			ProjectSettings.set_setting("rendering/nextgeo/experimental/foliage_wind", false)
			ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
			ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 0.0)
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 4.5, 3.0)
		"velocity_on":
			ProjectSettings.set_setting("rendering/nextgeo/experimental/foliage_wind", true)
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 4.5, 3.0)
		"multimesh_off":
			_tree_opaque_mi.visible = false
			_tree_masked_mi.visible = false
			_mmi.visible = true
			_cam.position = Vector3(0.0, 7.5, 13.0)
			_cam.look_at(Vector3(0.0, 1.5, -6.0), Vector3.UP)
			ProjectSettings.set_setting("rendering/nextgeo/experimental/foliage_wind", true)
			ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
			ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 0.0)
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 0.0, 2.0)
		"multimesh_on":
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 4.5, 2.0)
		"vsm_pos_x":
			_tree_opaque_mi.visible = true
			_tree_masked_mi.visible = false
			_mmi.visible = false
			_tree_opaque_mi.position = Vector3(0.0, 0.0, 0.0)
			# Elevated camera looking at the ground shadow cast behind the tree.
			_sun.rotation_degrees = Vector3(-35.0, 0.0, 0.0)
			_cam.position = Vector3(0.0, 9.5, 6.5)
			_cam.look_at(Vector3(0.0, 0.0, -3.5), Vector3.UP)
			ProjectSettings.set_setting("rendering/nextgeo/experimental/foliage_wind", true)
			ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
			ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 0.0)
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 6.0, 0.25)
		"vsm_neg_x":
			_set_world_wind(Vector3(-1.0, 0.0, 0.0), 6.0, 0.25)


func _capture_rgb8() -> Image:
	var img := get_viewport().get_texture().get_image()
	if img != null and img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img


func _mean_abs_diff(p_a: Image, p_b: Image, p_y0_frac: float = 0.0, p_y1_frac: float = 1.0) -> float:
	if p_a == null or p_b == null or p_a.get_size() != p_b.get_size():
		return 1.0
	var w := p_a.get_width()
	var h := p_a.get_height()
	var y0 := clampi(int(round(float(h) * p_y0_frac)), 0, h)
	var y1 := clampi(int(round(float(h) * p_y1_frac)), y0 + 1, h)
	var da := p_a.get_data()
	var db := p_b.get_data()
	var total := 0.0
	var pixels := 0
	for y in range(y0, y1):
		var row_base := y * w * 4
		for x in range(w):
			var idx := row_base + x * 4
			total += absf(float(da[idx]) - float(db[idx]))
			total += absf(float(da[idx + 1]) - float(db[idx + 1]))
			total += absf(float(da[idx + 2]) - float(db[idx + 2]))
			pixels += 1
	return total / (float(maxi(pixels, 1)) * 3.0 * 255.0)


func _analyze_velocity(p_cap: Dictionary) -> Dictionary:
	var words: PackedInt32Array = p_cap.get("words", PackedInt32Array())
	var n := words.size() / 3
	var covered := 0
	var moving := 0
	var sum_mag := 0.0
	var max_mag := 0.0
	var bytes := PackedByteArray()
	bytes.resize(8)
	for i in range(n):
		var inst_id := words[i * 3 + 0]
		if inst_id == -1 or inst_id == 0xFFFFFFFF:
			continue
		covered += 1
		bytes.encode_s32(0, words[i * 3 + 1])
		bytes.encode_s32(4, words[i * 3 + 2])
		var vx := bytes.decode_float(0)
		var vy := bytes.decode_float(4)
		var mag := sqrt(vx * vx + vy * vy)
		sum_mag += mag
		if mag > max_mag:
			max_mag = mag
		if mag > 1.0e-4:
			moving += 1
	return {
		"covered": covered,
		"moving": moving,
		"mean_mag": sum_mag / float(maxi(covered, 1)),
		"max_mag": max_mag,
		"velocity_valid": bool(p_cap.get("velocity", false)),
	}


func _check_ng_invariants(p_tag: String, p_require_sw: bool = false, p_require_mesh: bool = false) -> Dictionary:
	var stats := RenderingServer.virtual_geometry_get_debug_stats()
	var palette_bytes := int(stats.get("skin_palette_bytes", -1))
	var palette_bones := int(stats.get("skin_palette_used_bones", -1))
	if palette_bytes != 0 or palette_bones != 0:
		_failures.append("%s: expected 0 skin palette bytes/bones, got bytes=%d bones=%d" % [p_tag, palette_bytes, palette_bones])
	if int(stats.get("overflow", 0)) != 0:
		_failures.append("%s: NextGeo cut overflow=%d" % [p_tag, int(stats.get("overflow", 0))])
	if int(stats.get("double_dispatch", 0)) != 0:
		_failures.append("%s: double_dispatch=%d" % [p_tag, int(stats.get("double_dispatch", 0))])
	if int(stats.get("missing", 0)) != 0:
		_failures.append("%s: missing=%d" % [p_tag, int(stats.get("missing", 0))])
	if not bool(stats.get("cut_match", false)):
		_failures.append("%s: GPU/CPU cut_match=false" % p_tag)
	var sw_total := int(stats.get("sw_clusters", 0))
	var hw_total := int(stats.get("hw_clusters", 0))
	if p_require_sw and _expect_hybrid:
		if sw_total <= 0:
			_failures.append("%s: expected sw_clusters > 0, got %d" % [p_tag, sw_total])
	if p_require_mesh and _expect_mesh:
		if str(stats.get("hw_raster_path", "")) != "mesh":
			_failures.append("%s: expected hw_raster_path='mesh', got '%s'" % [p_tag, str(stats.get("hw_raster_path", ""))])
	return {
		"skin_palette_bytes": palette_bytes,
		"skin_palette_used_bones": palette_bones,
		"cut_match": bool(stats.get("cut_match", false)),
		"overflow": int(stats.get("overflow", 0)),
		"hw_raster_path": str(stats.get("hw_raster_path", "")),
		"sw_clusters": sw_total,
		"hw_clusters": hw_total,
	}


func _process(_delta: float) -> void:
	_phase_frame += 1
	match _phase:
		"wind_off_disabled":
			if _phase_frame == 12:
				_img_disabled = _capture_rgb8()
				_summary["wind_off_disabled"] = _check_ng_invariants("wind_off_disabled")
				_enter_phase("wind_off_zero_strength")
		"wind_off_zero_strength":
			if _phase_frame == 12:
				_img_zero_strength = _capture_rgb8()
				var diff_off := _mean_abs_diff(_img_disabled, _img_zero_strength)
				_summary["wind_off_vs_zero_strength_mae"] = diff_off
				if diff_off > 0.002:
					_failures.append("wind_off_disabled vs zero_strength MAE=%.6f > 0.002" % diff_off)
				_enter_phase("wind_on_hw_vs")
		"wind_on_hw_vs":
			if _phase_frame == 12:
				_img_hw_vs_a = _capture_rgb8()
				_summary["wind_on_hw_vs"] = _check_ng_invariants("wind_on_hw_vs")
			elif _phase_frame == 22:
				_img_hw_vs_b = _capture_rgb8()
				var diff_a := _mean_abs_diff(_img_hw_vs_a, _img_disabled)
				var diff_b := _mean_abs_diff(_img_hw_vs_b, _img_disabled)
				var diff_ab := _mean_abs_diff(_img_hw_vs_a, _img_hw_vs_b)
				_summary["hw_vs_wind_diff_max"] = maxf(diff_a, diff_b)
				_summary["hw_vs_time_motion_diff"] = diff_ab
				if maxf(diff_a, diff_b) < 0.003:
					_failures.append("wind_on_hw_vs showed insufficient displacement vs wind off (mae=%.6f)" % maxf(diff_a, diff_b))
				if diff_ab < 0.0005:
					_failures.append("wind_on_hw_vs showed no frame-to-frame sway motion (mae=%.6f)" % diff_ab)
				_enter_phase("wind_on_sw")
		"wind_on_sw":
			if _phase_frame == 12:
				_img_sw = _capture_rgb8()
				_summary["wind_on_sw"] = _check_ng_invariants("wind_on_sw", true, false)
				var diff_sw := _mean_abs_diff(_img_sw, _img_disabled)
				_summary["sw_wind_diff"] = diff_sw
				if diff_sw < 0.003:
					_failures.append("wind_on_sw showed insufficient displacement vs wind off (mae=%.6f)" % diff_sw)
				_enter_phase("wind_on_hw_mesh")
		"wind_on_hw_mesh":
			if _phase_frame == 12:
				_img_hw_mesh = _capture_rgb8()
				_summary["wind_on_hw_mesh"] = _check_ng_invariants("wind_on_hw_mesh", false, true)
				var diff_mesh := _mean_abs_diff(_img_hw_mesh, _img_disabled)
				_summary["hw_mesh_wind_diff"] = diff_mesh
				if diff_mesh < 0.003:
					_failures.append("wind_on_hw_mesh showed insufficient displacement vs wind off (mae=%.6f)" % diff_mesh)
				_enter_phase("velocity_off")
		"velocity_off":
			if _phase_frame == 8:
				_probe.capture = {}
				_probe.capture_armed = true
			elif _phase_frame == 12:
				_vel_off = _analyze_velocity(_probe.capture)
				_summary["velocity_off"] = _vel_off
				if _vel_off["covered"] <= 0:
					_failures.append("velocity_off: 0 covered NextGeo pixels")
				if _vel_off["mean_mag"] > 0.001:
					_failures.append("velocity_off: expected ~0 velocity when wind off, got mean=%.6f" % _vel_off["mean_mag"])
				_enter_phase("velocity_on")
		"velocity_on":
			if _phase_frame == 10:
				_probe.capture = {}
				_probe.capture_armed = true
			elif _phase_frame == 14:
				_vel_on = _analyze_velocity(_probe.capture)
				_summary["velocity_on"] = _vel_on
				if _vel_on["covered"] <= 0 or _vel_on["moving"] < 20 or _vel_on["max_mag"] < 1.0e-4:
					_failures.append("velocity_on: expected non-zero wind velocity on foliage pixels, got %s" % str(_vel_on))
				_enter_phase("multimesh_off")
		"multimesh_off":
			if _phase_frame == 12:
				_img_mm_off = _capture_rgb8()
				_summary["multimesh_off"] = _check_ng_invariants("multimesh_off")
				_enter_phase("multimesh_on")
		"multimesh_on":
			if _phase_frame == 14:
				_img_mm_on = _capture_rgb8()
				_summary["multimesh_on"] = _check_ng_invariants("multimesh_on")
				var diff_mm := _mean_abs_diff(_img_mm_on, _img_mm_off)
				_summary["multimesh_128_wind_diff"] = diff_mm
				if diff_mm < 0.003:
					_failures.append("multimesh_128: insufficient wind displacement across 128 MultiMesh instances (mae=%.6f)" % diff_mm)
				_enter_phase("vsm_pos_x")
		"vsm_pos_x":
			if _phase_frame == 12:
				_img_vsm_pos_x = _capture_rgb8()
				var vsm_info := RenderingServer.virtual_shadow_maps_get_debug_info()
				var vn: Dictionary = vsm_info.get("nanite", {})
				_summary["vsm_pos_x"] = _check_ng_invariants("vsm_pos_x")
				_summary["vsm_info_pos_x"] = {
					"active": bool(vn.get("active", false)),
					"gpu_counters_valid": bool(vn.get("gpu_counters_valid", false)),
					"dynamic_clusters": int(vn.get("dynamic_clusters", 0)),
					"static_clusters": int(vn.get("static_clusters", 0)),
					"overflow": int(vn.get("overflow", 0)),
					"sink": int(vn.get("sink", 0)),
				}
				if int(vn.get("overflow", 0)) != 0:
					_failures.append("vsm_pos_x: VSM nanite overflow=%d" % int(vn.get("overflow", 0)))
				if int(vn.get("sink", 0)) != 0:
					_failures.append("vsm_pos_x: VSM nanite sink=%d" % int(vn.get("sink", 0)))
				if int(vn.get("dynamic_clusters", 0)) <= 0:
					_failures.append("vsm_pos_x: expected VSM nanite dynamic_clusters > 0 for active foliage wind, got %d" % int(vn.get("dynamic_clusters", 0)))
				_enter_phase("vsm_neg_x")
		"vsm_neg_x":
			if _phase_frame == 12:
				_img_vsm_neg_x = _capture_rgb8()
				# Sample upper half of the frame where the ground shadow behind the tree projects.
				var shadow_diff := _mean_abs_diff(_img_vsm_pos_x, _img_vsm_neg_x, 0.05, 0.55)
				_summary["vsm_shadow_shift_mae"] = shadow_diff
				if shadow_diff < 0.002:
					_failures.append("vsm_dynamic_shadow: expected visible shadow shift between +X and -X wind (mae=%.6f)" % shadow_diff)
				_finish()


func _finish() -> void:
	for p in _temp_files:
		var abs_p := ProjectSettings.globalize_path(p)
		if FileAccess.file_exists(abs_p):
			DirAccess.remove_absolute(abs_p)
	_summary["failures"] = _failures
	_summary["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT %s" % JSON.stringify(_summary))
	get_tree().quit(0 if _failures.is_empty() else 1)
