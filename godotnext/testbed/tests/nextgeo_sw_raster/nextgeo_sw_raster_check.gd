extends Node3D
## NextGeo hybrid HW/SW raster check (#162, plan §21.4 N1-N3).
##
## Renders one moving scene of virtualized meshes (two dense spheres far away, one of them mirrored,
## a row of small static spheres further out, a row of tiny ones high up, some of them mirrored, a
## coarse non-uniformly scaled box close by, a large mirrored and non-uniformly scaled box that crosses
## the near plane, and a legacy box in front of a sphere) in a sequence of phases, and reads back every
## internal pixel's NextGeo instance ID, visibility ID, depth and velocity at the same two phase frames:
## - `hardware`: rendering/nextgeo/raster_mode = Hardware, the v2.4 raster (the reference).
## - `hybrid_t0`: Hybrid with sw_raster_max_edge_px = 0, so every cluster takes the hardware path
##   of the 64-bit visibility buffer. Instance, triangle and depth must equal the reference on every
##   pixel; velocity, which the export rebuilds from depth, within VELOCITY_TOLERANCE.
## - `hybrid_t8`, `hybrid_t32`: the triangle-size switch sends the small, far spheres' clusters to the
##   software rasterizer and keeps the boxes on hardware; the mirrored box near the camera must stay on
##   hardware, where the near plane clips it. Coverage, ID mismatches and depth ULPs against the
##   reference must stay within a few pixels; GN-1's own lines are reported (measured in PQ).
## - `hybrid_t8_buffer`: rendering/nextgeo/debug_vis64_buffer, the storage-buffer form used where
##   images lack 64-bit atomics. Must equal `hybrid_t8` exactly.
## - `hardware_again`: the reference once more, which must equal the first.
## On a device below the Nanite tier (N1, for example D3D12 until #123), the hybrid phases must stay
## on the v2.4 raster with the reason NO_INT64_ATOMICS and equal the reference exactly.
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const ProbeScript = preload("res://tests/nextgeo_sw_raster/nextgeo_sw_raster_probe.gd")
const SENTINEL := -1 # 0xFFFFFFFF read as int32.
const CAPTURE_FRAMES: Array[int] = [40, 41]
const STATS_FRAME := 46
const VELOCITY_TOLERANCE := 2e-4
const DISTANT_SPHERES := 6
const TINY_SPHERES := 16
# Functional bounds for the software phases, per capture, from the exact match measured on the L4
# (IoU 1.0, 0 ID mismatches, depth within 1 ULP) with a few pixels of slack. GN-1's own thresholds
# (ID mismatch <= 0.1%, IoU >= 0.999, depth <= 1 ULP) are reported, not enforced (§22).
const MAX_SOFTWARE_ONE_SIDED_PIXELS := 4 # Covered in only one of the two captures.
const MAX_SOFTWARE_ID_MISMATCH_PIXELS := 4
const MAX_SOFTWARE_DEPTH_OVER_1_ULP_PIXELS := 4
const MAX_SOFTWARE_DEPTH_ULP := 64

var _phases: Array[Dictionary] = [
	{ "name": "hardware", "mode": 1, "t": 8.0, "buffer": false },
	{ "name": "hybrid_t0", "mode": 0, "t": 0.0, "buffer": false },
	{ "name": "hybrid_t8", "mode": 0, "t": 8.0, "buffer": false },
	{ "name": "hybrid_t32", "mode": 0, "t": 32.0, "buffer": false },
	{ "name": "hybrid_t8_buffer", "mode": 0, "t": 8.0, "buffer": true },
	{ "name": "hardware_again", "mode": 1, "t": 8.0, "buffer": false },
]

var _probe: CompositorEffect
var _sphere_a: MeshInstance3D
var _sphere_b: MeshInstance3D
var _box: MeshInstance3D
var _legacy: MeshInstance3D
var _phase_idx := 0
var _phase_frame := 0
var _captures: Dictionary = {} # phase name -> Array of captures, one per CAPTURE_FRAMES entry.
var _stats: Dictionary = {} # phase name -> debug stats.
var _report: Dictionary = {}
var _failures: Array[String] = []
var _expect_hybrid := false
var _expect_storage := "none"


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	var rd := RenderingServer.get_rendering_device()
	var image_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_IMAGE_INT64_ATOMICS)
	var buffer_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_INT64_ATOMICS)
	var indirect_count := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT)
	_expect_hybrid = indirect_count and (image_int64 or buffer_int64)
	if _expect_hybrid:
		_expect_storage = "image" if image_int64 else "buffer"
	_report["device"] = {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"image_int64_atomics": image_int64,
		"buffer_int64_atomics": buffer_int64,
		"draw_indirect_count": indirect_count,
		"expect_hybrid": _expect_hybrid,
	}
	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 0.5
	_build_scene()
	_apply_phase()


func _build_scene() -> void:
	var sphere := SphereMesh.new()
	sphere.radius = 1.0
	sphere.height = 2.0
	sphere.radial_segments = 160
	sphere.rings = 80
	var sphere_material := StandardMaterial3D.new()
	sphere_material.albedo_color = Color(0.3, 0.7, 0.4)
	sphere.material = sphere_material
	var sphere_mesh := _virtualize(sphere, "user://nextgeo_sw_raster_sphere.ngm")

	var box := BoxMesh.new()
	box.size = Vector3(1.6, 1.6, 1.6)
	var box_material := StandardMaterial3D.new()
	box_material.albedo_color = Color(0.7, 0.5, 0.3)
	box.material = box_material
	var box_mesh := _virtualize(box, "user://nextgeo_sw_raster_box.ngm")

	_sphere_a = MeshInstance3D.new()
	_sphere_a.name = "SphereA"
	_sphere_a.mesh = sphere_mesh
	add_child(_sphere_a)
	_sphere_b = MeshInstance3D.new()
	_sphere_b.name = "SphereMirrored"
	_sphere_b.mesh = sphere_mesh
	add_child(_sphere_b)
	_box = MeshInstance3D.new()
	_box.name = "Box"
	_box.mesh = box_mesh
	add_child(_box)
	# Small, distant and static: clusters of a few pixels each, for the software rasterizer.
	for i in DISTANT_SPHERES:
		var distant := MeshInstance3D.new()
		distant.name = "Distant%d" % i
		distant.mesh = sphere_mesh
		distant.transform = Transform3D(Basis.from_scale(Vector3.ONE * (0.5 + 0.1 * float(i))), Vector3(-6.0 + 2.4 * float(i), -1.0 + 0.3 * float(i % 2), -22.0 - 2.0 * float(i)))
		add_child(distant)
	# Tiny, about 20 m away and high in the frame, clear of the other meshes: the cut keeps clusters
	# whose longest edges project to a few pixels, below the switch's estimate at T = 8. Every fourth
	# one is mirrored, so the software rasterizer's mirrored winding is compared too.
	for i in TINY_SPHERES:
		var tiny := MeshInstance3D.new()
		tiny.name = "Tiny%d" % i
		tiny.mesh = sphere_mesh
		var tiny_scale := Vector3(-0.1 if i % 4 == 0 else 0.1, 0.1, 0.1)
		tiny.transform = Transform3D(Basis.from_scale(tiny_scale), Vector3(-7.5 + float(i), 4.6 + 0.4 * float(i % 2), -13.4))
		add_child(tiny)
	# Mirrored, non-uniformly scaled and crossing the near plane at the lower right: the switch must
	# keep it on hardware, which clips it. Routed by the instance record's signed scale (-0.3125 here),
	# it would go to the software rasterizer, which drops triangles with a vertex behind the eye.
	var near_mirrored := MeshInstance3D.new()
	near_mirrored.name = "NearMirrored"
	near_mirrored.mesh = box_mesh
	near_mirrored.transform = Transform3D(Basis.from_scale(Vector3(-0.3125, 0.5, 3.125)), Vector3(0.55, -0.25, 6.5))
	add_child(near_mirrored)

	var legacy_box := BoxMesh.new()
	legacy_box.size = Vector3(0.5, 0.5, 0.5)
	_legacy = MeshInstance3D.new()
	_legacy.name = "LegacyBox"
	_legacy.mesh = legacy_box
	_legacy.position = Vector3(0.35, 0.3, 2.0)
	add_child(_legacy)

	var camera := Camera3D.new()
	camera.fov = 60.0
	camera.near = 0.05
	camera.far = 200.0
	camera.position = Vector3(0.0, 0.6, 7.0)
	camera.current = true
	add_child(camera)
	camera.look_at(Vector3(0.0, 0.0, 0.0))

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-45.0, 30.0, 0.0)
	add_child(sun)
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.1, 0.12, 0.16)
	var compositor := Compositor.new()
	_probe = ProbeScript.new()
	var effects: Array[CompositorEffect] = [_probe]
	compositor.compositor_effects = effects
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	world_environment.compositor = compositor
	add_child(world_environment)


func _virtualize(p_mesh: PrimitiveMesh, p_ngm_path: String) -> Mesh:
	var importer := ImporterMesh.from_mesh(p_mesh)
	var err := importer.build_virtual_geometry(p_ngm_path)
	var mesh := importer.get_mesh()
	if err != OK or mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(%s) failed (err=%d)" % [p_ngm_path, err])
	return mesh


# The scene at phase frame p_frame, the same in every phase.
func _step(p_frame: int) -> void:
	var t := float(p_frame)
	_sphere_a.position = Vector3(-0.6 + 0.4 * sin(t * 0.15), 0.0, -2.0)
	_sphere_b.transform = Transform3D(Basis.from_scale(Vector3(-0.8, 0.8, 0.8)), Vector3(2.4, 0.4, -9.0 + 0.5 * sin(t * 0.1)))
	_box.transform = Transform3D(Basis(Vector3.UP, deg_to_rad(15.0 + t * 1.5)).scaled(Vector3(1.2, 0.8, 1.0)), Vector3(-1.4, -0.2, 3.2))


func _apply_phase() -> void:
	var phase: Dictionary = _phases[_phase_idx]
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", int(phase["mode"]))
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", float(phase["t"]))
	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", bool(phase["buffer"]))
	_phase_frame = 0
	_captures[phase["name"]] = []


func _process(_delta: float) -> void:
	var phase: Dictionary = _phases[_phase_idx]
	var phase_name: String = phase["name"]
	# The capture armed on the previous frame was taken while rendering that frame.
	if CAPTURE_FRAMES.has(_phase_frame - 1):
		var capture: Dictionary = _probe.get("capture")
		if capture.is_empty():
			_failures.append("%s: no capture at frame %d" % [phase_name, _phase_frame - 1])
		_captures[phase_name].append(capture)
		_probe.set("capture", {})
	_step(_phase_frame)
	if CAPTURE_FRAMES.has(_phase_frame):
		_probe.set("capture_armed", true)
	if _phase_frame == STATS_FRAME:
		_stats[phase_name] = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
		_phase_idx += 1
		if _phase_idx >= _phases.size():
			_finish()
			return
		_apply_phase()
		_step(_phase_frame)
		return
	_phase_frame += 1


# Per-pixel comparison of two captures of the same frame.
func _compare(p_reference: Dictionary, p_test: Dictionary) -> Dictionary:
	var result := {
		"pixels": 0,
		"covered_reference": 0,
		"covered_test": 0,
		"covered_both": 0,
		"covered_one_side": 0,
		"id_mismatch": 0,
		"depth_max_ulp": 0,
		"depth_over_1_ulp": 0,
		"other_pixel_mismatch": 0,
		"velocity_max_error": 0.0,
		"velocity_compared": 0,
		"velocity_moving": 0,
	}
	if p_reference.is_empty() or p_test.is_empty() or p_reference["size"] != p_test["size"]:
		result["error"] = "missing or mismatched captures"
		return result
	var a: PackedInt32Array = p_reference["words"]
	var b: PackedInt32Array = p_test["words"]
	var a_float := a.to_byte_array().to_float32_array()
	var b_float := b.to_byte_array().to_float32_array()
	var size: Vector2i = p_reference["size"]
	var pixels := size.x * size.y
	if a.size() != pixels * ProbeScript.WORDS_PER_PIXEL or b.size() != a.size():
		result["error"] = "capture sizes do not match the target"
		return result
	result["pixels"] = pixels
	var covered_reference := 0
	var covered_test := 0
	var covered_both := 0
	var id_mismatch := 0
	var depth_max_ulp := 0
	var depth_over := 0
	var other_mismatch := 0
	var velocity_max := 0.0
	var velocity_compared := 0
	var velocity_moving := 0
	for i in pixels:
		var base := i * ProbeScript.WORDS_PER_PIXEL
		var in_a := a[base] != SENTINEL
		var in_b := b[base] != SENTINEL
		covered_reference += 1 if in_a else 0
		covered_test += 1 if in_b else 0
		if not in_a and not in_b:
			# Legacy or background: depth and velocity come from the other passes, unchanged.
			if a[base + 2] != b[base + 2] or a[base + 3] != b[base + 3] or a[base + 4] != b[base + 4]:
				other_mismatch += 1
			continue
		if not (in_a and in_b):
			continue
		covered_both += 1
		if a[base] != b[base] or (a[base + 1] & 0x7F) != (b[base + 1] & 0x7F):
			id_mismatch += 1
			continue
		var ulp: int = absi(a[base + 2] - b[base + 2])
		depth_max_ulp = maxi(depth_max_ulp, ulp)
		depth_over += 1 if ulp > 1 else 0
		var error := maxf(absf(a_float[base + 3] - b_float[base + 3]), absf(a_float[base + 4] - b_float[base + 4]))
		velocity_max = maxf(velocity_max, error)
		velocity_compared += 1
		velocity_moving += 1 if absf(a_float[base + 3]) + absf(a_float[base + 4]) > 1e-3 else 0
	result["covered_reference"] = covered_reference
	result["covered_test"] = covered_test
	result["covered_both"] = covered_both
	result["covered_one_side"] = covered_reference + covered_test - 2 * covered_both
	result["id_mismatch"] = id_mismatch
	result["depth_max_ulp"] = depth_max_ulp
	result["depth_over_1_ulp"] = depth_over
	result["other_pixel_mismatch"] = other_mismatch
	result["velocity_max_error"] = velocity_max
	result["velocity_compared"] = velocity_compared
	result["velocity_moving"] = velocity_moving
	var union := covered_reference + covered_test - covered_both
	result["iou"] = float(covered_both) / float(maxi(union, 1))
	result["id_mismatch_fraction"] = float(id_mismatch) / float(maxi(covered_both, 1))
	return result


# Every capture of p_test against the capture of p_reference at the same frame.
func _compare_phase(p_reference: String, p_test: String) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	var reference: Array = _captures.get(p_reference, [])
	var test: Array = _captures.get(p_test, [])
	if reference.size() != CAPTURE_FRAMES.size() or test.size() != CAPTURE_FRAMES.size():
		_failures.append("%s vs %s: expected %d captures each" % [p_test, p_reference, CAPTURE_FRAMES.size()])
		return out
	for i in CAPTURE_FRAMES.size():
		out.append(_compare(reference[i], test[i]))
	return out


func _check_identical(p_label: String, p_comparisons: Array[Dictionary], p_velocity_tolerance: float) -> void:
	for c in p_comparisons:
		if c.has("error"):
			_failures.append("%s: %s" % [p_label, c["error"]])
			continue
		if int(c["covered_reference"]) <= 0:
			_failures.append("%s: no NextGeo pixels" % p_label)
		if int(c["covered_reference"]) != int(c["covered_test"]) or int(c["covered_both"]) != int(c["covered_reference"]):
			_failures.append("%s: coverage differs (%d vs %d, both %d)" % [p_label, c["covered_reference"], c["covered_test"], c["covered_both"]])
		if int(c["id_mismatch"]) != 0 or int(c["depth_max_ulp"]) != 0:
			_failures.append("%s: %d ID mismatches, depth max %d ULP" % [p_label, c["id_mismatch"], c["depth_max_ulp"]])
		if int(c["other_pixel_mismatch"]) != 0:
			_failures.append("%s: %d legacy or background pixels changed" % [p_label, c["other_pixel_mismatch"]])
		if float(c["velocity_max_error"]) > p_velocity_tolerance:
			_failures.append("%s: velocity error %.6f > %.6f" % [p_label, c["velocity_max_error"], p_velocity_tolerance])


func _check_software(p_label: String, p_comparisons: Array[Dictionary]) -> void:
	for c in p_comparisons:
		if c.has("error"):
			_failures.append("%s: %s" % [p_label, c["error"]])
			continue
		if int(c["covered_reference"]) <= 0:
			_failures.append("%s: no NextGeo pixels" % p_label)
		if int(c["covered_one_side"]) > MAX_SOFTWARE_ONE_SIDED_PIXELS:
			_failures.append("%s: %d pixels covered on one side only (reference %d, test %d) > %d" % [p_label, c["covered_one_side"], c["covered_reference"], c["covered_test"], MAX_SOFTWARE_ONE_SIDED_PIXELS])
		if int(c["id_mismatch"]) > MAX_SOFTWARE_ID_MISMATCH_PIXELS:
			_failures.append("%s: %d ID mismatches > %d" % [p_label, c["id_mismatch"], MAX_SOFTWARE_ID_MISMATCH_PIXELS])
		if int(c["depth_over_1_ulp"]) > MAX_SOFTWARE_DEPTH_OVER_1_ULP_PIXELS or int(c["depth_max_ulp"]) > MAX_SOFTWARE_DEPTH_ULP:
			_failures.append("%s: %d pixels over 1 ULP of depth (at most %d), max %d ULP (at most %d)" % [p_label, c["depth_over_1_ulp"], MAX_SOFTWARE_DEPTH_OVER_1_ULP_PIXELS, c["depth_max_ulp"], MAX_SOFTWARE_DEPTH_ULP])
		if int(c["other_pixel_mismatch"]) != 0:
			_failures.append("%s: %d legacy or background pixels changed" % [p_label, c["other_pixel_mismatch"]])
		if float(c["velocity_max_error"]) > VELOCITY_TOLERANCE:
			_failures.append("%s: velocity error %.6f > %.6f" % [p_label, c["velocity_max_error"], VELOCITY_TOLERANCE])


func _stat(p_phase: String, p_key: String) -> int:
	return int((_stats.get(p_phase, {}) as Dictionary).get(p_key, 0))


func _check_stats() -> void:
	for phase in _phases:
		var phase_name: String = phase["name"]
		var stats: Dictionary = _stats.get(phase_name, {})
		if stats.is_empty() or not stats.has("hw_clusters"):
			_failures.append("%s: no NextGeo cut stats" % phase_name)
			continue
		var hybrid := int(phase["mode"]) == 0 and _expect_hybrid
		var expected_mode := "hybrid" if hybrid else "hardware"
		if str(stats.get("raster_mode")) != expected_mode:
			_failures.append("%s: raster_mode %s, expected %s" % [phase_name, stats.get("raster_mode"), expected_mode])
		var expected_reason := "NONE" if _expect_hybrid else "NO_INT64_ATOMICS"
		if str(stats.get("raster_reason")) != expected_reason:
			_failures.append("%s: raster_reason %s, expected %s" % [phase_name, stats.get("raster_reason"), expected_reason])
		var expected_storage := "none"
		if hybrid:
			expected_storage = "buffer" if bool(phase["buffer"]) else _expect_storage
		if str(stats.get("vis64_storage")) != expected_storage:
			_failures.append("%s: vis64_storage %s, expected %s" % [phase_name, stats.get("vis64_storage"), expected_storage])
		var sw := int(stats.get("sw_clusters", -1))
		var hw := int(stats.get("hw_clusters", -1))
		if not hybrid:
			if sw != 0 or hw != 0:
				_failures.append("%s: the v2.4 raster routed clusters (sw %d, hw %d)" % [phase_name, sw, hw])
		elif float(phase["t"]) == 0.0:
			if sw != 0 or hw <= 0:
				_failures.append("%s: T = 0 must route every cluster to hardware (sw %d, hw %d)" % [phase_name, sw, hw])
		elif sw <= 0 or hw <= 0 or int(stats.get("sw_raster_writes", 0)) <= 0:
			_failures.append("%s: expected both paths in use (sw %d, hw %d, sw writes %d)" % [phase_name, sw, hw, int(stats.get("sw_raster_writes", 0))])
		var visible := int(stats.get("phase1_clusters", 0)) + int(stats.get("phase2_clusters", 0))
		if hybrid and sw + hw != visible:
			_failures.append("%s: %d + %d routed clusters, %d visible" % [phase_name, sw, hw, visible])
	if _expect_hybrid:
		var sw8 := _stat("hybrid_t8", "sw_clusters")
		var sw32 := _stat("hybrid_t32", "sw_clusters")
		var hw8 := _stat("hybrid_t8", "hw_clusters")
		var hw32 := _stat("hybrid_t32", "hw_clusters")
		if sw32 < sw8 or hw32 > hw8:
			_failures.append("a larger T must not move clusters to hardware (T=8: sw %d hw %d, T=32: sw %d hw %d)" % [sw8, hw8, sw32, hw32])


func _finish() -> void:
	_check_identical("hardware_again vs hardware", _compare_phase("hardware", "hardware_again"), 0.0)
	var t0 := _compare_phase("hardware", "hybrid_t0")
	var t8 := _compare_phase("hardware", "hybrid_t8")
	var t32 := _compare_phase("hardware", "hybrid_t32")
	var buffer := _compare_phase("hybrid_t8", "hybrid_t8_buffer")
	_check_identical("hybrid_t0 vs hardware", t0, VELOCITY_TOLERANCE if _expect_hybrid else 0.0)
	_check_identical("hybrid_t8_buffer vs hybrid_t8", buffer, 0.0)
	if _expect_hybrid:
		_check_software("hybrid_t8 vs hardware", t8)
		_check_software("hybrid_t32 vs hardware", t32)
	else:
		# Below the Nanite tier every phase is the v2.4 raster.
		_check_identical("hybrid_t8 vs hardware", t8, 0.0)
		_check_identical("hybrid_t32 vs hardware", t32, 0.0)
	_check_stats()
	for comparisons in [t0, t8, t32, buffer]:
		for c in comparisons:
			if int(c.get("velocity_moving", 0)) <= 0:
				_failures.append("no moving NextGeo pixels: the velocity comparison would be empty")
	var stats_report := {}
	for phase_name in _stats:
		var s: Dictionary = _stats[phase_name]
		stats_report[phase_name] = {
			"raster_mode": s.get("raster_mode"),
			"raster_reason": s.get("raster_reason"),
			"vis64_storage": s.get("vis64_storage"),
			"sw_clusters": s.get("sw_clusters"),
			"hw_clusters": s.get("hw_clusters"),
			"sw_raster_writes": s.get("sw_raster_writes"),
			"raster_indirect_draws": s.get("raster_indirect_draws"),
		}
	_report["stats"] = stats_report
	_report["hybrid_t0_vs_hardware"] = t0
	_report["hybrid_t8_vs_hardware"] = t8
	_report["hybrid_t32_vs_hardware"] = t32
	_report["buffer_vs_image"] = buffer
	# GN-1's lines (§21.4), reported at PR time and measured in PQ.
	var gn1 := []
	for comparisons in [t8, t32]:
		for c in comparisons:
			gn1.append({
				"id_mismatch_percent": snappedf(100.0 * float(c.get("id_mismatch_fraction", 1.0)), 1e-4),
				"depth_max_ulp": c.get("depth_max_ulp"),
				"iou": snappedf(float(c.get("iou", 0.0)), 1e-6),
			})
	_report["gn1_report_only"] = gn1
	_report["failures"] = _failures
	_report["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_report))
	get_tree().quit(0 if _failures.is_empty() else 1)
