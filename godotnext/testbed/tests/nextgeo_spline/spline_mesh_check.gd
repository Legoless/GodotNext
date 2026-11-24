extends Node3D
## Spline meshes GPU verification (#161, plan §21.4 contract N12, GN-8, GN-12, N8).
##
## Verifies:
##   (a) Plain MeshInstance3D byte-identity: rendering a plain MeshInstance3D before vs after
##       setting and clearing instance_geometry_set_spline_deform(rid, {}) produces an exact
##       0-byte image diff.
##   (b) Curved S-bend + rolled/scaled SplineMeshInstance3D (and a Path3D-driven segment via
##       set_from_path()) on Legacy Forward+ vs NextGeo HW VS, NextGeo SW, NextGeo HW Mesh (#154),
##       and NextGeo Hybrid (0 culled-but-visible clusters, 0 overflow, tight image & visibility parity).
##   (c) VSM shadow alignment & dynamic invalidation (#212/#217) on a spline arch: shadow lands under
##       the deformed arch rather than the undeformed straight pose, and shifts cleanly when the arch
##       moves live.
## Prints `GODOTNEXT_RESULT {...}` and exits 0/1.

const ProbeScript = preload("res://tests/nextgeo_sw_raster/nextgeo_sw_raster_probe.gd")
const CAPTURE_FRAME := 20
const STATS_FRAME := 24

const MAX_NG_COLOR_MAE := 0.008
const MAX_NG_COLOR_P99 := 0.04
const MAX_LEGACY_NG_COLOR_MAE := 0.03
const MAX_VIS_DIFF_FRACTION := 0.015

var _phases: Array[Dictionary] = [
	{ "name": "plain_ref", "mode": "plain", "clear_spline": false },
	{ "name": "plain_cleared", "mode": "plain", "clear_spline": true },
	{ "name": "legacy_spline", "mode": "spline_legacy", "path": 1, "t": 0.0 },
	{ "name": "nextgeo_hw_vs", "mode": "spline_ng", "path": 1, "t": 0.0 },
	{ "name": "nextgeo_sw", "mode": "spline_ng", "path": 1, "t": 512.0 },
	{ "name": "nextgeo_hw_mesh", "mode": "spline_ng", "path": 2, "t": 0.0 },
	{ "name": "nextgeo_hybrid", "mode": "spline_ng", "path": 1, "t": 28.0 },
	{ "name": "arch_moved", "mode": "spline_ng_moved", "path": 1, "t": 0.0 },
]

var _probe: CompositorEffect
var _phase_idx := 0
var _phase_frame := 0
var _captures: Dictionary = {}
var _images: Dictionary = {}
var _ng_stats: Dictionary = {}
var _vsm_stats: Dictionary = {}
var _vsm_peak_overflow: Dictionary = {}
var _vsm_peak_sink: Dictionary = {}
var _temp_files: Array[String] = []
var _failures: Array[String] = []
var _report: Dictionary = {}
var _expect_hybrid := false
var _expect_mesh := false

var _sun: DirectionalLight3D
var _plain_root: Node3D
var _plain_box: MeshInstance3D
var _spline_root: Node3D
var _spline_sbend: SplineMeshInstance3D
var _spline_arch: SplineMeshInstance3D
var _spline_path_seg: SplineMeshInstance3D
var _floor_inst: MeshInstance3D

var _raw_tube_mesh: Mesh
var _ng_tube_mesh: Mesh
var _raw_arch_mesh: Mesh
var _ng_arch_mesh: Mesh
var _raw_floor_mesh: Mesh
var _ng_floor_mesh: Mesh


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", true)
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/occlusion", false)
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

	_report["device"] = {
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

	_build_scene()
	_apply_phase()


func _make_raw_array_mesh(p_prim: PrimitiveMesh) -> ArrayMesh:
	var importer := ImporterMesh.from_mesh(p_prim)
	return importer.get_mesh()


func _virtualize(p_prim: PrimitiveMesh, p_name: String) -> Mesh:
	var path := "user://nextgeo_spline_%s.ngm" % p_name
	_temp_files.append(path)
	var importer := ImporterMesh.from_mesh(p_prim)
	var err := importer.build_virtual_geometry(path)
	var mesh := importer.get_mesh()
	if err != OK or mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(%s) failed (err=%d)" % [p_name, err])
	return mesh


func _build_scene() -> void:
	var camera := Camera3D.new()
	camera.fov = 52.0
	camera.near = 0.05
	camera.far = 80.0
	camera.position = Vector3(0.0, 3.8, 7.6)
	camera.current = true
	add_child(camera)
	camera.look_at(Vector3(0.0, 0.3, 0.0))

	_sun = DirectionalLight3D.new()
	_sun.rotation_degrees = Vector3(-62.0, 18.0, 0.0)
	_sun.light_energy = 1.6
	_sun.shadow_enabled = true
	add_child(_sun)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.08, 0.1, 0.14)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.14, 0.14, 0.16)
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

	# Dense bar mesh along Z for S-bend and Path3D segment.
	var bar_mat := StandardMaterial3D.new()
	bar_mat.albedo_color = Color(0.76, 0.42, 0.28)
	bar_mat.roughness = 0.65
	var bar_prim := BoxMesh.new()
	bar_prim.size = Vector3(0.45, 0.35, 2.0)
	bar_prim.subdivide_width = 6
	bar_prim.subdivide_height = 6
	bar_prim.subdivide_depth = 48
	bar_prim.material = bar_mat
	_raw_tube_mesh = _make_raw_array_mesh(bar_prim)
	_ng_tube_mesh = _virtualize(bar_prim, "tube")

	# Arch beam along X for VSM shadow alignment test.
	var arch_mat := StandardMaterial3D.new()
	arch_mat.albedo_color = Color(0.32, 0.66, 0.82)
	arch_mat.roughness = 0.6
	var arch_prim := BoxMesh.new()
	arch_prim.size = Vector3(2.0, 0.38, 0.48)
	arch_prim.subdivide_width = 48
	arch_prim.subdivide_height = 6
	arch_prim.subdivide_depth = 6
	arch_prim.material = arch_mat
	_raw_arch_mesh = _make_raw_array_mesh(arch_prim)
	_ng_arch_mesh = _virtualize(arch_prim, "arch")

	# Bright floor catching VSM shadows.
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.86, 0.86, 0.86)
	floor_mat.roughness = 0.9
	var floor_prim := BoxMesh.new()
	floor_prim.size = Vector3(14.0, 0.3, 12.0)
	floor_prim.subdivide_width = 16
	floor_prim.subdivide_height = 2
	floor_prim.subdivide_depth = 16
	floor_prim.material = floor_mat
	_raw_floor_mesh = _make_raw_array_mesh(floor_prim)
	_ng_floor_mesh = _virtualize(floor_prim, "floor")

	_floor_inst = MeshInstance3D.new()
	_floor_inst.name = "Floor"
	_floor_inst.mesh = _raw_floor_mesh
	_floor_inst.position = Vector3(0.0, -0.35, 0.0)
	add_child(_floor_inst)

	# Plain MeshInstance3D for byte-identity check.
	_plain_root = Node3D.new()
	add_child(_plain_root)
	_plain_box = MeshInstance3D.new()
	_plain_box.name = "PlainBox"
	_plain_box.mesh = _raw_tube_mesh
	_plain_box.position = Vector3(0.0, 0.8, 0.0)
	_plain_root.add_child(_plain_box)

	# Spline scene root.
	_spline_root = Node3D.new()
	add_child(_spline_root)

	# 1) Curved S-bend with roll & non-uniform cross-section scale.
	_spline_sbend = SplineMeshInstance3D.new()
	_spline_sbend.name = "SBend"
	_spline_sbend.position = Vector3(-2.4, 0.65, -0.6)
	_spline_sbend.forward_axis = SplineMeshInstance3D.AXIS_Z
	_spline_sbend.start_position = Vector3(-0.6, 0.0, -1.8)
	_spline_sbend.start_tangent = Vector3(4.2, 0.6, 3.2)
	_spline_sbend.end_position = Vector3(0.6, 0.5, 1.8)
	_spline_sbend.end_tangent = Vector3(4.2, -0.4, 3.2)
	_spline_sbend.start_roll = -0.45
	_spline_sbend.end_roll = 1.15
	_spline_sbend.start_scale = Vector2(0.85, 1.35)
	_spline_sbend.end_scale = Vector2(1.45, 0.8)
	_spline_sbend.up_vector = Vector3(0.0, 1.0, 0.0)
	_spline_root.add_child(_spline_sbend)

	# 2) High spline arch along AXIS_X casting a crisp curved VSM shadow onto the bright floor.
	_spline_arch = SplineMeshInstance3D.new()
	_spline_arch.name = "Arch"
	_spline_arch.position = Vector3(1.2, 0.0, 0.0)
	_spline_arch.forward_axis = SplineMeshInstance3D.AXIS_X
	_spline_arch.start_position = Vector3(-1.8, 0.25, -0.6)
	_spline_arch.start_tangent = Vector3(1.5, 6.8, 0.8)
	_spline_arch.end_position = Vector3(1.8, 0.25, -0.6)
	_spline_arch.end_tangent = Vector3(1.5, -6.8, -0.8)
	_spline_arch.start_scale = Vector2(1.1, 1.1)
	_spline_arch.end_scale = Vector2(1.1, 1.1)
	_spline_arch.up_vector = Vector3(0.0, 1.0, 0.0)
	_spline_root.add_child(_spline_arch)

	# 3) Path3D-driven segment exercising set_from_path().
	var path := Path3D.new()
	path.position = Vector3(0.0, 0.45, 2.0)
	var curve := Curve3D.new()
	curve.add_point(Vector3(-2.6, 0.0, 0.0), Vector3.ZERO, Vector3(0.8, 0.6, 0.9))
	curve.add_point(Vector3(0.2, 0.2, 0.4), Vector3(-0.9, 0.4, 0.6), Vector3.ZERO)
	curve.set_point_tilt(0, 0.2)
	curve.set_point_tilt(1, -0.35)
	path.curve = curve
	_spline_root.add_child(path)

	var path_mat := StandardMaterial3D.new()
	path_mat.albedo_color = Color(0.42, 0.78, 0.36)
	path_mat.roughness = 0.65
	_spline_path_seg = SplineMeshInstance3D.new()
	_spline_path_seg.name = "PathSegment"
	_spline_path_seg.forward_axis = SplineMeshInstance3D.AXIS_Z
	_spline_path_seg.material_override = path_mat
	_spline_root.add_child(_spline_path_seg)
	_spline_path_seg.set_from_path(path, 0)


func _set_arch_pose(p_shifted: bool) -> void:
	if p_shifted:
		_spline_arch.start_position = Vector3(-1.8, 0.25, 1.3)
		_spline_arch.start_tangent = Vector3(1.5, 6.8, -0.8)
		_spline_arch.end_position = Vector3(1.8, 0.25, 1.3)
		_spline_arch.end_tangent = Vector3(1.5, -6.8, 0.8)
	else:
		_spline_arch.start_position = Vector3(-1.8, 0.25, -0.6)
		_spline_arch.start_tangent = Vector3(1.5, 6.8, 0.8)
		_spline_arch.end_position = Vector3(1.8, 0.25, -0.6)
		_spline_arch.end_tangent = Vector3(1.5, -6.8, -0.8)


func _apply_phase() -> void:
	var phase: Dictionary = _phases[_phase_idx]
	var mode: String = phase["mode"]
	var pname: String = phase["name"]
	_vsm_peak_overflow[pname] = 0
	_vsm_peak_sink[pname] = 0

	if mode == "plain":
		_plain_root.visible = true
		_spline_root.visible = false
		_floor_inst.mesh = _raw_floor_mesh
		if bool(phase["clear_spline"]):
			RenderingServer.instance_geometry_set_spline_deform(_plain_box.get_instance(), {
				"start_position": Vector3(0, 0, -1),
				"start_tangent": Vector3(0, 1, 2),
				"end_position": Vector3(0, 0, 1),
				"end_tangent": Vector3(0, -1, 2),
			})
			RenderingServer.instance_geometry_set_spline_deform(_plain_box.get_instance(), {})
	else:
		_plain_root.visible = false
		_spline_root.visible = true
		ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", int(phase["path"]))
		ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", float(phase["t"]))
		ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", float(phase["t"]))
		var use_ng := mode != "spline_legacy"
		_floor_inst.mesh = _ng_floor_mesh if use_ng else _raw_floor_mesh
		_spline_sbend.mesh = _ng_tube_mesh if use_ng else _raw_tube_mesh
		_spline_arch.mesh = _ng_arch_mesh if use_ng else _raw_arch_mesh
		_spline_path_seg.mesh = _ng_tube_mesh if use_ng else _raw_tube_mesh
		_set_arch_pose(mode == "spline_ng_moved")

	# For non-moved phases reset VSM shadows briefly so each phase rasterizes cleanly;
	# for `arch_moved`, keep shadows ON across the live spline move so #212 dynamic invalidation is tested directly!
	if mode != "spline_ng_moved":
		_sun.shadow_enabled = false
	_phase_frame = 0


func _process(_delta: float) -> void:
	var phase: Dictionary = _phases[_phase_idx]
	var pname: String = phase["name"]
	var mode: String = phase["mode"]
	if _phase_frame == 3 and mode != "spline_ng_moved":
		_sun.shadow_enabled = true
	if _phase_frame >= 5 and mode != "plain":
		var vsm_now: Dictionary = RenderingServer.virtual_shadow_maps_get_debug_info()
		var vn: Dictionary = vsm_now.get("nanite", {})
		if bool(vn.get("active", false)) and bool(vn.get("gpu_counters_valid", false)):
			_vsm_peak_overflow[pname] = maxi(int(_vsm_peak_overflow.get(pname, 0)), int(vn.get("overflow", 0)))
			_vsm_peak_sink[pname] = maxi(int(_vsm_peak_sink.get(pname, 0)), int(vn.get("sink", 0)))
	if _phase_frame == CAPTURE_FRAME + 1 and mode != "plain":
		_captures[pname] = _probe.get("capture")
		_probe.set("capture", {})
	if _phase_frame == CAPTURE_FRAME and mode != "plain":
		_probe.set("capture_armed", true)
	if _phase_frame == STATS_FRAME:
		_ng_stats[pname] = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
		_vsm_stats[pname] = RenderingServer.virtual_shadow_maps_get_debug_info()
		_images[pname] = get_viewport().get_texture().get_image()
		_phase_idx += 1
		if _phase_idx >= _phases.size():
			_finish()
			return
		_apply_phase()
		return
	_phase_frame += 1


func _compare_images(p_a_name: String, p_b_name: String, p_max_mae: float, p_max_p99: float) -> Dictionary:
	var out := {}
	var img_a: Image = _images.get(p_a_name)
	var img_b: Image = _images.get(p_b_name)
	if img_a == null or img_b == null or img_a.get_size() != img_b.get_size():
		_failures.append("%s vs %s: missing viewport images" % [p_b_name, p_a_name])
		return out
	var w := img_a.get_width()
	var h := img_a.get_height()
	var sum_err := 0.0
	var diffs: Array[float] = []
	diffs.resize(w * h)
	for y in h:
		for x in w:
			var ca := img_a.get_pixel(x, y)
			var cb := img_b.get_pixel(x, y)
			var d := (absf(ca.r - cb.r) + absf(ca.g - cb.g) + absf(ca.b - cb.b)) / 3.0
			sum_err += d
			diffs[y * w + x] = d
	diffs.sort()
	var mae := sum_err / float(maxi(w * h, 1))
	var p99 := diffs[mini(int(float(diffs.size()) * 0.99), diffs.size() - 1)]
	out["color_mae"] = snappedf(mae, 1e-6)
	out["color_p99"] = snappedf(p99, 1e-6)
	if mae > p_max_mae or p99 > p_max_p99:
		_failures.append("%s vs %s: color/shadow parity exceeded tolerance (mae=%.5f > %.5f, p99=%.5f > %.5f)" % [p_b_name, p_a_name, mae, p_max_mae, p99, p_max_p99])
	return out


func _compare_vis(p_a_name: String, p_b_name: String) -> Dictionary:
	var out := {}
	var cap_a: Dictionary = _captures.get(p_a_name, {})
	var cap_b: Dictionary = _captures.get(p_b_name, {})
	if cap_a.is_empty() or cap_b.is_empty():
		_failures.append("%s vs %s: missing visibility capture" % [p_b_name, p_a_name])
		return out
	var wa: PackedInt32Array = cap_a["words"]
	var wb: PackedInt32Array = cap_b["words"]
	var pixels := wa.size() / ProbeScript.WORDS_PER_PIXEL
	var ng_pixels := 0
	var inst_diff := 0
	for i in pixels:
		var base := i * ProbeScript.WORDS_PER_PIXEL
		var ia := wa[base]
		var ib := wb[base]
		if ia != -1 or ib != -1:
			ng_pixels += 1
		if ia != ib:
			inst_diff += 1
	var frac := float(inst_diff) / float(maxi(ng_pixels, 1))
	out["ng_pixels"] = ng_pixels
	out["inst_diff"] = inst_diff
	out["inst_diff_fraction"] = snappedf(frac, 1e-6)
	if ng_pixels < 2000:
		_failures.append("%s vs %s: too few NextGeo pixels (%d)" % [p_b_name, p_a_name, ng_pixels])
	if frac > MAX_VIS_DIFF_FRACTION:
		_failures.append("%s vs %s: instance visibility differs on %d/%d pixels (%.4f > %.4f)" % [p_b_name, p_a_name, inst_diff, ng_pixels, frac, MAX_VIS_DIFF_FRACTION])
	return out


func _cleanup_temp_files() -> void:
	for path in _temp_files:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	_temp_files.clear()


func _finish() -> void:
	# (a) Plain MeshInstance3D byte-identity check.
	var img_plain_a: Image = _images.get("plain_ref")
	var img_plain_b: Image = _images.get("plain_cleared")
	var plain_byte_diffs := -1
	if img_plain_a != null and img_plain_b != null:
		var ba := img_plain_a.get_data()
		var bb := img_plain_b.get_data()
		plain_byte_diffs = 0
		if ba.size() != bb.size():
			plain_byte_diffs = maxi(ba.size(), bb.size())
		else:
			for i in ba.size():
				if ba[i] != bb[i]:
					plain_byte_diffs += 1
	if plain_byte_diffs != 0:
		_failures.append("plain MeshInstance3D byte-identity failed: %d differing bytes" % plain_byte_diffs)
	_report["plain_byte_diffs"] = plain_byte_diffs

	# (b) Legacy vs NextGeo HW VS / SW / HW Mesh / Hybrid parity.
	var comparisons := {}
	comparisons["ng_vs_vs_legacy"] = _compare_images("legacy_spline", "nextgeo_hw_vs", MAX_LEGACY_NG_COLOR_MAE, 0.15)
	comparisons["ng_sw_vs_hw_vs"] = _compare_images("nextgeo_hw_vs", "nextgeo_sw", MAX_NG_COLOR_MAE, MAX_NG_COLOR_P99)
	comparisons["ng_mesh_vs_hw_vs"] = _compare_images("nextgeo_hw_vs", "nextgeo_hw_mesh", MAX_NG_COLOR_MAE, MAX_NG_COLOR_P99)
	comparisons["ng_hybrid_vs_hw_vs"] = _compare_images("nextgeo_hw_vs", "nextgeo_hybrid", MAX_NG_COLOR_MAE, MAX_NG_COLOR_P99)
	comparisons["vis_sw_vs_hw_vs"] = _compare_vis("nextgeo_hw_vs", "nextgeo_sw")
	comparisons["vis_mesh_vs_hw_vs"] = _compare_vis("nextgeo_hw_vs", "nextgeo_hw_mesh")
	comparisons["vis_hybrid_vs_hw_vs"] = _compare_vis("nextgeo_hw_vs", "nextgeo_hybrid")

	# (c) Arch live move + VSM dynamic invalidation check: moving the arch must meaningfully change
	# pixels/shadows relative to the unmoved pose (verifying live spline deformation + shadow shift).
	var img_unmoved: Image = _images.get("nextgeo_hw_vs")
	var img_moved: Image = _images.get("arch_moved")
	var moved_changed_pixels := 0
	if img_unmoved != null and img_moved != null:
		for y in img_unmoved.get_height():
			for x in img_unmoved.get_width():
				var ca := img_unmoved.get_pixel(x, y)
				var cb := img_moved.get_pixel(x, y)
				if absf(ca.r - cb.r) + absf(ca.g - cb.g) + absf(ca.b - cb.b) > 0.12:
					moved_changed_pixels += 1
	_report["arch_moved_changed_pixels"] = moved_changed_pixels
	if moved_changed_pixels < 300:
		_failures.append("arch_moved did not visibly shift arch/VSM shadow (%d changed pixels)" % moved_changed_pixels)

	# Verify zero overflow/sink across all NextGeo spline phases.
	var stats_summary := {}
	for pname in ["nextgeo_hw_vs", "nextgeo_sw", "nextgeo_hw_mesh", "nextgeo_hybrid", "arch_moved"]:
		var ng: Dictionary = _ng_stats.get(pname, {})
		var vsm_n: Dictionary = _vsm_stats.get(pname, {}).get("nanite", {})
		var ng_ov := int(ng.get("overflow", 0))
		var vsm_ov := maxi(int(vsm_n.get("overflow", 0)), int(_vsm_peak_overflow.get(pname, 0)))
		var vsm_sk := maxi(int(vsm_n.get("sink", 0)), int(_vsm_peak_sink.get(pname, 0)))
		if ng_ov != 0:
			_failures.append("%s: NextGeo overflow=%d" % [pname, ng_ov])
		if vsm_ov != 0:
			_failures.append("%s: VSM overflow=%d" % [pname, vsm_ov])
		if vsm_sk != 0:
			_failures.append("%s: VSM atomic sink tripped" % pname)
		if _expect_hybrid and int(ng.get("hw_clusters", 0)) + int(ng.get("sw_clusters", 0)) <= 0:
			_failures.append("%s: no NextGeo spline clusters rasterized" % pname)
		stats_summary[pname] = {
			"hw_raster_path": ng.get("hw_raster_path"),
			"hw_clusters": ng.get("hw_clusters"),
			"sw_clusters": ng.get("sw_clusters"),
			"overflow": ng_ov,
			"vsm_active": vsm_n.get("active", false),
			"vsm_hw_raster_path": vsm_n.get("hw_raster_path"),
			"vsm_overflow": vsm_ov,
			"vsm_sink": vsm_sk,
		}

	_cleanup_temp_files()
	_report["comparisons"] = comparisons
	_report["stats"] = stats_summary
	_report["failures"] = _failures
	_report["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_report))
	get_tree().quit(0 if _failures.is_empty() else 1)
