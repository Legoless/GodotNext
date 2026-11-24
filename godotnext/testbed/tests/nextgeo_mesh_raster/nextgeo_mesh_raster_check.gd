extends Node3D
## NextGeo mesh-shader HW raster parity check (#154, plan §21.4 contract N9, GN-10).
##
## Renders virtualized NextGeo meshes (high-cluster fixed-function meshes including mirrored
## negative-scale and two-sided surfaces, plus programmable alpha-scissor masked and WPO
## vertex-offset bins) with VSM shadows across BOTH `rendering/nextgeo/hw_raster_path = vertex`
## and `mesh` (as well as `auto`, verifying `auto` stays `vertex` until GN-10 passes in PQ):
##   - `auto_hw`: hw_raster_path = Auto (0), T = 0, VIS64 image -> verifies `hw_raster_path == "vertex"`.
##   - `vs_hw_image`: hw_raster_path = Vertex (1), T = 0, VIS64 image -> VS HW reference.
##   - `mesh_hw_image`: hw_raster_path = Mesh (2), T = 0, VIS64 image -> must match `vs_hw_image`
##     (visibility IDs, depth, shaded color + VSM shadows) within tight edge tolerance.
##   - `vs_hw_buffer` vs `mesh_hw_buffer`: VIS64 buffer storage parity.
##   - `vs_hybrid` vs `mesh_hybrid`: T = 8 hybrid SW+HW parity (both fixed-function & programmable bins).
##   - `vs_backface` vs `mesh_backface`: debug_backface parity.
## Also verifies VSM nanite stats (`hw_raster_path == "mesh"` when supported, `hw_clusters > 0`,
## `overflow == 0`, `sink == 0`) and prints `GODOTNEXT_RESULT {...}` before exiting 0/1.

const ProbeScript = preload("res://tests/nextgeo_sw_raster/nextgeo_sw_raster_probe.gd")
const CAPTURE_FRAME := 28
const STATS_FRAME := 32

# Tight parity tolerances between VS vertex-pulling and Task/Mesh HW rasterization:
# both invoke the exact same HW rasterizer on identical clip-space positions, so pixels should match
# virtually 1:1 (allowing tiny floating-point/edge tie-breaking slack on rasterized silhouettes).
const MAX_VIS_DIFF_FRACTION := 0.002
const MAX_COLOR_MAE := 0.005
const MAX_COLOR_P99 := 0.02

const MASKED_STRIPES_SHADER := """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.85, 0.35, 0.25);
	ALPHA = step(0.5, fract(UV.x * 6.0));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""

const VERTEX_OFFSET_SHADER := """
shader_type spatial;
uniform float vertex_scale = 1.22;
void vertex() {
	VERTEX *= vertex_scale;
}
void fragment() {
	ALBEDO = vec3(0.25, 0.78, 0.38);
}
"""

var _phases: Array[Dictionary] = [
	{ "name": "auto_hw", "path": 0, "t": 0.0, "buffer": false, "backface": false, "prog": true },
	{ "name": "vs_hw_image", "path": 1, "t": 0.0, "buffer": false, "backface": false, "prog": true },
	{ "name": "mesh_hw_image", "path": 2, "t": 0.0, "buffer": false, "backface": false, "prog": true },
	{ "name": "vs_hw_buffer", "path": 1, "t": 0.0, "buffer": true, "backface": false, "prog": true },
	{ "name": "mesh_hw_buffer", "path": 2, "t": 0.0, "buffer": true, "backface": false, "prog": true },
	{ "name": "vs_hybrid", "path": 1, "t": 8.0, "buffer": false, "backface": false, "prog": true },
	{ "name": "mesh_hybrid", "path": 2, "t": 8.0, "buffer": false, "backface": false, "prog": true },
	{ "name": "vs_backface", "path": 1, "t": 0.0, "buffer": false, "backface": true, "prog": true },
	{ "name": "mesh_backface", "path": 2, "t": 0.0, "buffer": false, "backface": true, "prog": true },
]

var _probe: CompositorEffect
var _phase_idx := 0
var _phase_frame := 0
var _captures: Dictionary = {}
var _images: Dictionary = {}
var _ng_stats: Dictionary = {}
var _vsm_stats: Dictionary = {}
var _vsm_peak_hw: Dictionary = {}
var _vsm_peak_prog_hw: Dictionary = {}
var _vsm_peak_overflow: Dictionary = {}
var _vsm_peak_sink: Dictionary = {}
var _materials: Dictionary = {}
var _temp_files: Array[String] = []
var _failures: Array[String] = []
var _report: Dictionary = {}
var _expect_mesh := false
var _expect_hybrid := false
var _sun: DirectionalLight3D
var _spot: SpotLight3D


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", true)
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/occlusion", false)
	# Enable VSM so every phase exercises both main-view and VSM HW rasterization.
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
		"supports_mesh_shader": mesh_shader,
		"supports_mesh_shader_indirect": mesh_indirect,
		"supports_draw_indirect_count": indirect_count,
		"image_int64_atomics": image_int64,
		"buffer_int64_atomics": buffer_int64,
		"expect_mesh_hw": _expect_mesh,
		"max_task_invocations": rd.limit_get(RenderingDevice.LIMIT_MAX_TASK_WORKGROUP_INVOCATIONS) if rd != null else 0,
		"max_mesh_invocations": rd.limit_get(RenderingDevice.LIMIT_MAX_MESH_WORKGROUP_INVOCATIONS) if rd != null else 0,
		"max_mesh_output_vertices": rd.limit_get(RenderingDevice.LIMIT_MAX_MESH_OUTPUT_VERTICES) if rd != null else 0,
		"max_mesh_output_primitives": rd.limit_get(RenderingDevice.LIMIT_MAX_MESH_OUTPUT_PRIMITIVES) if rd != null else 0,
		"max_task_payload_size": rd.limit_get(RenderingDevice.LIMIT_MAX_TASK_PAYLOAD_SIZE) if rd != null else 0,
	}

	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 0.5

	_build_scene()
	_apply_phase()


func _virtualize(p_mesh: PrimitiveMesh, p_name: String) -> Mesh:
	var path := "user://nextgeo_mesh_raster_%s.ngm" % p_name
	_temp_files.append(path)
	var importer := ImporterMesh.from_mesh(p_mesh)
	var err := importer.build_virtual_geometry(path)
	var mesh := importer.get_mesh()
	if err != OK or mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(%s) failed (err=%d)" % [p_name, err])
	return mesh


func _shader_material(p_code: String) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = p_code
	var mat := ShaderMaterial.new()
	mat.shader = shader
	return mat


func _add(p_name: String, p_mesh: Mesh, p_xform: Transform3D, p_mat: Material) -> MeshInstance3D:
	var inst := MeshInstance3D.new()
	inst.name = p_name
	inst.mesh = p_mesh
	inst.transform = p_xform
	add_child(inst)
	if p_mat != null:
		inst.material_override = p_mat
		_materials[inst] = p_mat
	return inst


func _build_scene() -> void:
	# High-tessellation sphere so > 32 clusters hit HW raster (exercising multiple 32-cluster task workgroups).
	var sphere := SphereMesh.new()
	sphere.radius = 0.75
	sphere.height = 1.5
	sphere.radial_segments = 96
	sphere.rings = 48
	var opaque_mat := StandardMaterial3D.new()
	opaque_mat.albedo_color = Color(0.75, 0.7, 0.65)
	opaque_mat.roughness = 0.75
	sphere.material = opaque_mat
	var sphere_mesh := _virtualize(sphere, "sphere")

	var box := BoxMesh.new()
	box.size = Vector3(1.0, 1.0, 1.0)
	box.subdivide_width = 16
	box.subdivide_height = 16
	box.subdivide_depth = 16
	box.material = opaque_mat
	var box_mesh := _virtualize(box, "box")

	var plane := PlaneMesh.new()
	plane.size = Vector2(2.2, 1.8)
	plane.subdivide_width = 32
	plane.subdivide_depth = 24
	plane.material = opaque_mat
	var plane_mesh := _virtualize(plane, "plane")

	var camera := Camera3D.new()
	camera.fov = 55.0
	camera.near = 0.05
	camera.far = 80.0
	camera.position = Vector3(0.0, 1.4, 5.2)
	camera.current = true
	add_child(camera)
	camera.look_at(Vector3(0.0, 0.2, 0.0))

	# Floor catching shadows from all casters (both fixed-function and programmable).
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.85, 0.85, 0.85)
	floor_mat.roughness = 0.9
	_add("Floor", box_mesh, Transform3D(Basis.from_scale(Vector3(14.0, 0.4, 14.0)), Vector3(0.0, -1.2, -1.0)), floor_mat)

	# Fixed-function high-cluster sphere (normal handedness).
	_add("FixedSphere", sphere_mesh, Transform3D(Basis(), Vector3(-1.8, 0.4, 0.2)), opaque_mat)

	# Mirrored negative-scale sphere (verifies winding flip parity between VS and mesh shaders).
	var mirror_mat := StandardMaterial3D.new()
	mirror_mat.albedo_color = Color(0.35, 0.55, 0.85)
	mirror_mat.roughness = 0.6
	_add("MirroredSphere", sphere_mesh, Transform3D(Basis.from_scale(Vector3(-1.0, 1.0, 1.0)), Vector3(0.0, 0.4, 0.2)), mirror_mat)

	# Two-sided back-facing plane (verifies cull_disabled clip-space winding parity).
	var two_sided_mat := StandardMaterial3D.new()
	two_sided_mat.albedo_color = Color(0.85, 0.72, 0.3)
	two_sided_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	_add("TwoSidedBackPlane", plane_mesh, Transform3D(Basis(Vector3.RIGHT, deg_to_rad(-90.0)).scaled(Vector3.ONE * 0.6), Vector3(-2.6, 1.4, -0.8)), two_sided_mat)

	# Single-sided back-facing plane (culled unless debug_backface is on).
	var single_mat := StandardMaterial3D.new()
	single_mat.albedo_color = Color(0.75, 0.3, 0.75)
	single_mat.cull_mode = BaseMaterial3D.CULL_BACK
	_add("SingleSidedBackPlane", plane_mesh, Transform3D(Basis(Vector3.RIGHT, deg_to_rad(-90.0)).scaled(Vector3.ONE * 0.5), Vector3(2.8, 1.5, -0.6)), single_mat)

	# Programmable bins: masked alpha-scissor stripes + WPO vertex offset.
	_add("MaskedStripes", plane_mesh, Transform3D(Basis(Vector3.RIGHT, deg_to_rad(90.0)).scaled(Vector3.ONE * 0.75), Vector3(1.8, 0.5, 0.3)), _shader_material(MASKED_STRIPES_SHADER))
	_add("VertexOffsetSphere", sphere_mesh, Transform3D(Basis.from_scale(Vector3.ONE * 0.65), Vector3(0.0, 1.4, -1.2)), _shader_material(VERTEX_OFFSET_SHADER))

	# Directional + local VSM lights to exercise both VSM indirect chains (directional + local).
	_sun = DirectionalLight3D.new()
	_sun.rotation_degrees = Vector3(-42.0, 28.0, 0.0)
	_sun.light_energy = 1.4
	_sun.shadow_enabled = true
	add_child(_sun)

	_spot = SpotLight3D.new()
	_spot.position = Vector3(-3.0, 3.2, 2.5)
	_spot.spot_angle = 55.0
	_spot.spot_range = 14.0
	_spot.light_energy = 3.0
	_spot.shadow_enabled = true
	add_child(_spot)
	_spot.look_at(Vector3(0.0, 0.0, 0.0))

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.08, 0.1, 0.14)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.12, 0.12, 0.14)
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


func _apply_phase() -> void:
	var phase: Dictionary = _phases[_phase_idx]
	var pname: String = phase["name"]
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", int(phase["path"]))
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", float(phase["t"]))
	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", bool(phase["buffer"]))
	ProjectSettings.set_setting("rendering/nextgeo/debug_backface", bool(phase["backface"]))
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", bool(phase["prog"]))
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", float(phase["t"]))
	# Disable shadows for the first 3 frames of each phase so VSM drops its cached page maps, then
	# re-enable them at frame 4 at the exact canonical light pose so every phase freshly rasterizes
	# fixed-function and programmable VSM pages with the active hw_raster_path.
	if _sun != null:
		_sun.shadow_enabled = false
	if _spot != null:
		_spot.shadow_enabled = false
	_vsm_peak_hw[pname] = 0
	_vsm_peak_prog_hw[pname] = 0
	_vsm_peak_overflow[pname] = 0
	_vsm_peak_sink[pname] = 0
	_phase_frame = 0


func _process(_delta: float) -> void:
	var phase: Dictionary = _phases[_phase_idx]
	var phase_name: String = phase["name"]
	if _phase_frame == 4:
		if _sun != null:
			_sun.shadow_enabled = true
		if _spot != null:
			_spot.shadow_enabled = true
	if _phase_frame >= 5:
		var vsm_now: Dictionary = RenderingServer.virtual_shadow_maps_get_debug_info()
		var vn: Dictionary = vsm_now.get("nanite", {})
		if bool(vn.get("active", false)) and bool(vn.get("gpu_counters_valid", false)):
			_vsm_peak_hw[phase_name] = maxi(int(_vsm_peak_hw.get(phase_name, 0)), int(vn.get("hw_clusters", 0)))
			_vsm_peak_prog_hw[phase_name] = maxi(int(_vsm_peak_prog_hw.get(phase_name, 0)), int(vn.get("prog_hw_clusters", 0)))
			_vsm_peak_overflow[phase_name] = maxi(int(_vsm_peak_overflow.get(phase_name, 0)), int(vn.get("overflow", 0)))
			_vsm_peak_sink[phase_name] = maxi(int(_vsm_peak_sink.get(phase_name, 0)), int(vn.get("sink", 0)))
	if _phase_frame == CAPTURE_FRAME + 1:
		var capture: Dictionary = _probe.get("capture")
		if capture.is_empty():
			_failures.append("%s: missing visibility probe capture" % phase_name)
		_captures[phase_name] = capture
		_probe.set("capture", {})
	if _phase_frame == CAPTURE_FRAME:
		_probe.set("capture_armed", true)
	if _phase_frame == STATS_FRAME:
		_ng_stats[phase_name] = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
		_vsm_stats[phase_name] = RenderingServer.virtual_shadow_maps_get_debug_info()
		_images[phase_name] = get_viewport().get_texture().get_image()
		_phase_idx += 1
		if _phase_idx >= _phases.size():
			_finish()
			return
		_apply_phase()
		return
	_phase_frame += 1


func _compare_pair(p_ref_name: String, p_test_name: String) -> Dictionary:
	var out := {}
	var cap_a: Dictionary = _captures.get(p_ref_name, {})
	var cap_b: Dictionary = _captures.get(p_test_name, {})
	if cap_a.is_empty() or cap_b.is_empty():
		_failures.append("%s vs %s: missing capture" % [p_test_name, p_ref_name])
		return out
	var wa: PackedInt32Array = cap_a["words"]
	var wb: PackedInt32Array = cap_b["words"]
	var pixels := wa.size() / ProbeScript.WORDS_PER_PIXEL
	var ng_pixels := 0
	var vis_diff := 0
	var depth_diff := 0
	for i in pixels:
		var base := i * ProbeScript.WORDS_PER_PIXEL
		var inst_a := wa[base]
		var inst_b := wb[base]
		if inst_a != -1 or inst_b != -1:
			ng_pixels += 1
		# Compare instance ID, triangle ID (low 7 bits of vis_id; cluster index follows atomic order), and depth.
		if inst_a != inst_b or (wa[base + 1] & 0x7F) != (wb[base + 1] & 0x7F):
			vis_diff += 1
		if wa[base + 2] != wb[base + 2]:
			depth_diff += 1

	var vis_diff_frac := float(vis_diff) / float(maxi(ng_pixels, 1))
	out["pixels"] = pixels
	out["ng_pixels"] = ng_pixels
	out["vis_diff"] = vis_diff
	out["depth_diff"] = depth_diff
	out["vis_diff_fraction"] = snappedf(vis_diff_frac, 1e-6)
	if ng_pixels < 1000:
		_failures.append("%s vs %s: too few NextGeo pixels (%d)" % [p_test_name, p_ref_name, ng_pixels])
	if vis_diff_frac > MAX_VIS_DIFF_FRACTION:
		_failures.append("%s vs %s: visibility differs on %d/%d pixels (%.4f > %.4f)" % [p_test_name, p_ref_name, vis_diff, ng_pixels, vis_diff_frac, MAX_VIS_DIFF_FRACTION])

	var img_a: Image = _images.get(p_ref_name)
	var img_b: Image = _images.get(p_test_name)
	if img_a != null and img_b != null and img_a.get_size() == img_b.get_size():
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
		if mae > MAX_COLOR_MAE or p99 > MAX_COLOR_P99:
			_failures.append("%s vs %s: color/shadow parity exceeded tolerance (mae=%.5f, p99=%.5f)" % [p_test_name, p_ref_name, mae, p99])
	else:
		_failures.append("%s vs %s: missing viewport color images" % [p_test_name, p_ref_name])
	return out


func _check_stats() -> void:
	for phase in _phases:
		var pname: String = phase["name"]
		var ng: Dictionary = _ng_stats.get(pname, {})
		var vsm: Dictionary = _vsm_stats.get(pname, {})
		var vsm_n: Dictionary = vsm.get("nanite", {})
		var want_mesh := int(phase["path"]) == 2 and _expect_mesh
		var expected_path := "mesh" if want_mesh else "vertex"

		if _expect_hybrid:
			var got_path := String(ng.get("hw_raster_path", ""))
			if got_path != expected_path:
				_failures.append("%s: main view hw_raster_path=%s, expected %s (reason=%s)" % [pname, got_path, expected_path, String(ng.get("hw_raster_reason", ""))])
			if int(ng.get("overflow", 0)) != 0:
				_failures.append("%s: main view overflow=%d" % [pname, int(ng.get("overflow", 0))])
			if float(phase["t"]) == 0.0 and int(ng.get("hw_clusters", 0)) <= 0:
				_failures.append("%s: expected main view hw_clusters > 0" % pname)
			if int(phase["path"]) == 0 and String(ng.get("hw_raster_reason", "")) != "AUTO_PENDING_PQ" and _expect_mesh:
				_failures.append("%s: auto on mesh-capable device must report AUTO_PENDING_PQ until PQ, got %s" % [pname, String(ng.get("hw_raster_reason", ""))])

		if OS.get_environment("GODOT_DEV_VSM") != "" and (not bool(vsm.get("active", false)) or not bool(vsm_n.get("active", false))):
			_failures.append("%s: GODOT_DEV_VSM is set but VSM nanite is inactive (vsm_active=%s nanite_active=%s)" % [pname, str(vsm.get("active", false)), str(vsm_n.get("active", false))])
		if bool(vsm.get("active", false)) and bool(vsm_n.get("active", false)):
			var got_vsm_path := String(vsm_n.get("hw_raster_path", ""))
			if got_vsm_path != expected_path:
				_failures.append("%s: VSM hw_raster_path=%s, expected %s (reason=%s)" % [pname, got_vsm_path, expected_path, String(vsm_n.get("hw_raster_reason", ""))])
			if bool(vsm_n.get("gpu_counters_valid", false)):
				var vsm_ov := maxi(int(vsm_n.get("overflow", 0)), int(_vsm_peak_overflow.get(pname, 0)))
				var vsm_sk := maxi(int(vsm_n.get("sink", 0)), int(_vsm_peak_sink.get(pname, 0)))
				var vsm_hw := maxi(int(vsm_n.get("hw_clusters", 0)), int(_vsm_peak_hw.get(pname, 0)))
				if vsm_ov != 0:
					_failures.append("%s: VSM overflow=%d" % [pname, vsm_ov])
				if vsm_sk != 0:
					_failures.append("%s: VSM #277 atomic sink tripped" % pname)
				if float(phase["t"]) == 0.0 and vsm_hw <= 0:
					_failures.append("%s: expected VSM hw_clusters > 0" % pname)


func _cleanup_temp_files() -> void:
	for path in _temp_files:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	_temp_files.clear()


func _finish() -> void:
	_check_stats()
	var comparisons := {}
	comparisons["auto_vs_vertex"] = _compare_pair("vs_hw_image", "auto_hw")
	comparisons["mesh_vs_vs_hw_image"] = _compare_pair("vs_hw_image", "mesh_hw_image")
	comparisons["mesh_vs_vs_hw_buffer"] = _compare_pair("vs_hw_buffer", "mesh_hw_buffer")
	comparisons["mesh_vs_vs_hybrid"] = _compare_pair("vs_hybrid", "mesh_hybrid")
	comparisons["mesh_vs_vs_backface"] = _compare_pair("vs_backface", "mesh_backface")

	var stats_summary := {}
	for phase in _phases:
		var pname: String = phase["name"]
		var ng: Dictionary = _ng_stats.get(pname, {})
		var vsm_n: Dictionary = _vsm_stats.get(pname, {}).get("nanite", {})
		stats_summary[pname] = {
			"hw_raster_path": ng.get("hw_raster_path"),
			"hw_raster_reason": ng.get("hw_raster_reason"),
			"vis64_storage": ng.get("vis64_storage"),
			"hw_clusters": ng.get("hw_clusters"),
			"sw_clusters": ng.get("sw_clusters"),
			"programmable_hw_clusters": ng.get("programmable_hw_clusters"),
			"vsm_hw_raster_path": vsm_n.get("hw_raster_path"),
			"vsm_hw_clusters": maxi(int(vsm_n.get("hw_clusters", 0)), int(_vsm_peak_hw.get(pname, 0))),
			"vsm_prog_hw_clusters": maxi(int(vsm_n.get("prog_hw_clusters", 0)), int(_vsm_peak_prog_hw.get(pname, 0))),
			"vsm_overflow": maxi(int(vsm_n.get("overflow", 0)), int(_vsm_peak_overflow.get(pname, 0))),
			"vsm_sink": maxi(int(vsm_n.get("sink", 0)), int(_vsm_peak_sink.get(pname, 0))),
		}

	_cleanup_temp_files()
	_report["comparisons"] = comparisons
	_report["stats"] = stats_summary
	_report["failures"] = _failures
	_report["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_report))
	get_tree().quit(0 if _failures.is_empty() else 1)
