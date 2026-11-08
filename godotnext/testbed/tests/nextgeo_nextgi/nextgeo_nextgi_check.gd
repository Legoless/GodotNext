extends Node
## NextGeo in NextGI end-to-end GPU check (#77; plan D6, D13, §7.1, §8.2, §8.3 Stage 2):
## 1. Virtualized G5-6 scenes (`cbr_single_40m_vg.tscn` and `courtyard_vg.tscn`) enter
##    `tier == "HWRT"`, build compacted proxy BLASes within line 1b's 250k tri/frame cap,
##    capture Stage-2 SRC cards once per card (`<= 64` cards / `<= 1M` texels per frame,
##    `0` shadow-batch refusals), and shade non-black indirect/direct lighting.
## 2. Toggling `nextgi_set_hardware_ray_tracing` to `Off` drops `proxy_blas_bytes_compacted`
##    and `rt_arena` bytes to `0`; restoring `Auto` rebuilds them; freeing the last instance
##    drops all proxy and RT arena bytes back to `0`.
## 3. Slow IO Flight (`slow_io_flight.tscn`) under a tight 2 MiB (16-slot) page cache and
##    wall-time IO delay triggers `page_evictions > 0` while rebuilding `0` BLASes.

const Flight := preload("res://scenes/slow_io_flight/flight.gd")

const LINE_1B_TRI_CAP := 250000
const CAPTURE_TEXELS_CAP := 1048576
const MAX_CARD_TEXELS := 128
const CACHE_MB := 2

var _failures := PackedStringArray()
var _temp_files: Array[String] = []
var _ngm_counter := 0


func _ready() -> void:
	var result := await run()
	_cleanup_temp_files()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if bool(result["pass"]) else 1)


func _check(p_condition: bool, p_what: String) -> void:
	if not p_condition:
		_failures.append(p_what)


func _alloc_temp_ngm(p_tag: String) -> String:
	_ngm_counter += 1
	var path := "user://ng_nextgi_%s_%d.ngm" % [p_tag, _ngm_counter]
	_temp_files.append(path)
	return path


func _cleanup_temp_files() -> void:
	for path in _temp_files:
		var abs_path := ProjectSettings.globalize_path(path)
		if FileAccess.file_exists(abs_path):
			DirAccess.remove_absolute(abs_path)
	_temp_files.clear()


func run() -> Dictionary:
	var vp := get_viewport()
	vp.use_taa = false
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 1.0
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/debug/coverage_counter", true)

	var enabled: bool = ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false)
	_check(enabled, "nextgi/enabled must be true (run via run_nextgeo_nextgi_check.sh)")
	if not enabled:
		return {"pass": false, "failures": _failures}

	var hall_summary := await _check_virtualized_scene("res://scenes/color_box_room/cbr_single_40m_vg.tscn", true)
	var courtyard_summary := await _check_virtualized_scene("res://scenes/concave_courtyard/courtyard_vg.tscn", false)
	var flight_summary := await _check_slow_io_flight_eviction()

	return {
		"pass": _failures.is_empty(),
		"device": RenderingServer.get_video_adapter_name(),
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"cbr_single_40m_vg": hall_summary,
		"courtyard_vg": courtyard_summary,
		"slow_io_flight": flight_summary,
		"failures": _failures,
	}


func _prepare_scene(p_root: Node) -> void:
	var has_sun := false
	var stack: Array[Node] = [p_root]
	while not stack.is_empty():
		var cur: Node = stack.pop_back()
		if cur is WorldEnvironment:
			var we := cur as WorldEnvironment
			if we.environment != null:
				var env: Environment = we.environment.duplicate()
				env.nextgi_enabled = true
				we.environment = env
		elif cur is OmniLight3D:
			(cur as OmniLight3D).shadow_enabled = true
		elif cur is DirectionalLight3D:
			(cur as DirectionalLight3D).shadow_enabled = true
			has_sun = true
		elif cur is Camera3D and cur.name == &"CaptureCam":
			(cur as Camera3D).current = true
		for child in cur.get_children():
			stack.append(child)
	if not has_sun:
		var sun := DirectionalLight3D.new()
		sun.name = "CheckSun"
		sun.shadow_enabled = true
		sun.light_energy = 0.25
		sun.rotation_degrees = Vector3(-50.0, 30.0, 0.0)
		p_root.add_child(sun)


func _max_image_luma(p_vp: Viewport) -> float:
	var img := p_vp.get_texture().get_image()
	if img == null or img.is_empty():
		return 0.0
	var max_luma := 0.0
	var step_x := maxi(img.get_width() / 16, 1)
	var step_y := maxi(img.get_height() / 16, 1)
	for y in range(step_y / 2, img.get_height(), step_y):
		for x in range(step_x / 2, img.get_width(), step_x):
			var c := img.get_pixel(x, y)
			if not is_finite(c.r) or not is_finite(c.g) or not is_finite(c.b):
				_failures.append("non-finite pixel (%s) at (%d, %d)" % [str(c), x, y])
				return -1.0
			max_luma = maxf(max_luma, c.get_luminance())
	return max_luma


func _check_virtualized_scene(p_scene_path: String, p_test_hwrt_toggle: bool) -> Dictionary:
	var label := p_scene_path.get_file().get_basename()
	var packed := load(p_scene_path) as PackedScene
	_check(packed != null, "%s: scene loads" % label)
	if packed == null:
		return {}

	var root := packed.instantiate() as Node3D
	add_child(root)
	_prepare_scene(root)

	var max_blas_builds := 0
	var steady_blas_builds := 0
	var max_capture_texels := 0
	var info: Dictionary = {}
	var counters: Dictionary = {}

	for frame in range(32):
		await RenderingServer.frame_post_draw
		info = RenderingServer.nextgi_get_tier_info()
		counters = RenderingServer.nextgi_get_counters()
		var builds := int(info.get("rt_blas_build_count", 0))
		max_blas_builds = maxi(max_blas_builds, builds)
		if frame >= 8:
			steady_blas_builds += builds
		max_capture_texels = maxi(max_capture_texels, int(counters.get("src_capture_texels", 0)))
		var proxy_tris := int(counters.get("proxy_triangles", 0))
		if builds > 0:
			_check(proxy_tris <= LINE_1B_TRI_CAP * maxi(builds, 1), "%s: proxy build within line 1b cap" % label)

	if String(info.get("tier", "")) != "HWRT":
		_check(false, "%s: expected tier == 'HWRT', got '%s'" % [label, String(info.get("tier", ""))])
		remove_child(root)
		root.free()
		return {"tier": info.get("tier", "")}

	var proxy_parts := int(counters.get("proxy_blas_parts", 0))
	var proxy_tris := int(counters.get("proxy_triangles", 0))
	var proxy_compacted := int(counters.get("proxy_blas_bytes_compacted", 0))
	var proxy_uncompacted := int(counters.get("proxy_blas_bytes_uncompacted", 0))
	var stage2_captured := int(counters.get("src_stage2_cards_captured", 0))
	var card_count := int(info.get("src_card_count", 0))
	var captured_cards := int(info.get("src_captured_card_count", -1))

	_check(proxy_parts >= 1, "%s: proxy_blas_parts >= 1 (got %d)" % [label, proxy_parts])
	_check(proxy_tris > 0, "%s: proxy_triangles > 0 (got %d)" % [label, proxy_tris])
	_check(proxy_compacted > 0, "%s: proxy_blas_bytes_compacted > 0 (got %d)" % [label, proxy_compacted])
	_check(proxy_compacted <= proxy_uncompacted, "%s: compacted (%d) <= uncompacted (%d)" % [label, proxy_compacted, proxy_uncompacted])
	_check(int(info.get("rt_instance_count", 0)) >= 1, "%s: TLAS holds >= 1 virtualized instance" % label)
	_check(max_blas_builds >= 1, "%s: proxy BLAS built on initial frames" % label)
	_check(steady_blas_builds == 0, "%s: 0 BLAS rebuilds on static frames (got %d)" % [label, steady_blas_builds])
	_check(max_capture_texels <= CAPTURE_TEXELS_CAP, "%s: capture texels/frame <= 1M (got %d)" % [label, max_capture_texels])
	_check(card_count > 0, "%s: src_card_count > 0 (got %d)" % [label, card_count])
	_check(captured_cards == card_count, "%s: all %d Stage-2 cards captured (got %d)" % [label, card_count, captured_cards])
	_check(stage2_captured == card_count, "%s: src_stage2_cards_captured (%d) == card_count (%d)" % [label, stage2_captured, card_count])
	_check(int(info.get("src_max_captures_per_card", -1)) == 1, "%s: each Stage-2 card captured once" % label)
	_check(int(info.get("src_shadow_batch_refusals", -1)) == 0, "%s: 0 shadow-batch capture refusals" % label)

	var sizes: PackedInt32Array = info.get("src_card_sizes", PackedInt32Array())
	_check(sizes.size() == card_count * 2, "%s: two dimensions per Stage-2 card" % label)
	for side in sizes:
		_check(side >= 4 and side <= MAX_CARD_TEXELS, "%s: card side %d within [4, 128]" % [label, side])

	var max_luma := _max_image_luma(get_viewport())
	_check(max_luma > 0.01, "%s: rendered non-black lit output (max_luma=%.4f)" % [label, max_luma])

	var cov_cam := root.get_node_or_null("CovCam0") as Camera3D
	if cov_cam != null:
		cov_cam.current = true
		for _i in range(6):
			await RenderingServer.frame_post_draw
		info = RenderingServer.nextgi_get_tier_info()

	var cov_rays := int(info.get("src_coverage_rays", 0))
	var cov_hits := int(info.get("src_coverage_hits", 0))
	var cov_ratio := float(cov_hits) / float(maxi(cov_rays, 1))
	_check(cov_rays > 0, "%s: src_coverage_rays > 0 (got %d)" % [label, cov_rays])
	_check(cov_hits > 0, "%s: src_coverage_hits > 0 (got %d)" % [label, cov_hits])
	_check(cov_ratio >= 0.85, "%s: Stage-2 card hit coverage >= 0.85 (got %.3f = %d/%d)" % [label, cov_ratio, cov_hits, cov_rays])

	var off_proxy_bytes := -1
	var off_rt_arena := -1
	if p_test_hwrt_toggle:
		RenderingServer.nextgi_set_hardware_ray_tracing(1) # Off
		for _i in range(3):
			await RenderingServer.frame_post_draw
		var off_counters: Dictionary = RenderingServer.nextgi_get_counters()
		var off_mem: Dictionary = RenderingServer.virtual_geometry_get_memory_report()
		off_proxy_bytes = int(off_counters.get("proxy_blas_bytes_compacted", -1))
		off_rt_arena = int(off_mem.get("rt_arena", -1))
		_check(off_proxy_bytes == 0, "%s: 0 compacted proxy bytes with HWRT Off (got %d)" % [label, off_proxy_bytes])
		_check(int(off_counters.get("proxy_blas_parts", -1)) == 0, "%s: 0 proxy_blas_parts with HWRT Off" % label)
		_check(int(off_counters.get("proxy_triangles", -1)) == 0, "%s: 0 proxy_triangles with HWRT Off" % label)
		_check(off_rt_arena == 0, "%s: 0 rt_arena bytes with HWRT Off (got %d)" % [label, off_rt_arena])
		_check(int(off_mem.get("blas", -1)) == 0, "%s: 0 BLAS bytes with HWRT Off (got %s)" % [label, str(off_mem.get("blas"))])

		RenderingServer.nextgi_set_hardware_ray_tracing(0) # Auto
		for _i in range(8):
			await RenderingServer.frame_post_draw
		var re_counters: Dictionary = RenderingServer.nextgi_get_counters()
		_check(int(re_counters.get("proxy_blas_parts", 0)) >= 1, "%s: proxy BLAS rebuilt when HWRT returns to Auto" % label)
		_check(int(re_counters.get("proxy_blas_bytes_compacted", 0)) > 0, "%s: proxy BLAS re-compacted when HWRT returns to Auto" % label)

	remove_child(root)
	root.free()
	for _i in range(3):
		await RenderingServer.frame_post_draw

	var freed_counters: Dictionary = RenderingServer.nextgi_get_counters()
	var freed_mem: Dictionary = RenderingServer.virtual_geometry_get_memory_report()
	_check(int(freed_counters.get("proxy_blas_bytes_compacted", -1)) == 0, "%s: 0 proxy bytes after last instance freed" % label)
	_check(int(freed_counters.get("proxy_triangles", -1)) == 0, "%s: 0 proxy_triangles after last instance freed" % label)
	_check(int(freed_mem.get("rt_arena", -1)) == 0, "%s: 0 rt_arena bytes after last instance freed" % label)

	return {
		"proxy_blas_parts": proxy_parts,
		"proxy_triangles": proxy_tris,
		"proxy_blas_bytes_compacted": proxy_compacted,
		"proxy_blas_bytes_uncompacted": proxy_uncompacted,
		"src_card_count": card_count,
		"src_stage2_cards_captured": stage2_captured,
		"src_coverage_hits": cov_hits,
		"src_coverage_rays": cov_rays,
		"src_coverage_ratio": snappedf(cov_ratio, 0.0001),
		"max_luma": snappedf(max_luma, 0.0001),
		"off_proxy_bytes": off_proxy_bytes,
		"off_rt_arena": off_rt_arena,
	}


func _make_dense_flight_quad(p_tag: String, p_color: Color) -> ArrayMesh:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = p_color
	mat.roughness = 0.85
	mat.metallic = 0.0

	var qmesh := QuadMesh.new()
	qmesh.size = Vector2(4.0, 4.0)
	qmesh.subdivide_width = 72
	qmesh.subdivide_depth = 72
	qmesh.material = mat

	var imp := ImporterMesh.from_mesh(qmesh)
	var err := imp.build_virtual_geometry(_alloc_temp_ngm(p_tag))
	if err != OK:
		_failures.append("slow_io_flight: build_virtual_geometry failed for %s (err=%d)" % [p_tag, err])
		return null
	var vmesh := imp.get_mesh()
	if vmesh == null or vmesh.get_virtual_geometry() == null:
		_failures.append("slow_io_flight: missing VirtualGeometryData on %s" % p_tag)
		return null
	return vmesh


func _check_slow_io_flight_eviction() -> Dictionary:
	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 0.0)
	ProjectSettings.set_setting("rendering/limits/nextgeo/geometry_cache_mb", CACHE_MB)
	ProjectSettings.set_setting("rendering/limits/nextgeo/page_upload_kib_per_frame", 256)
	ProjectSettings.set_setting("rendering/nextgeo/streaming/debug_io_delay_ms", Vector2(5.0, 10.0))

	var packed := load("res://scenes/slow_io_flight/slow_io_flight.tscn") as PackedScene
	_check(packed != null, "slow_io_flight: scene loads")
	if packed == null:
		return {}

	var root := packed.instantiate() as Node3D
	root.set_meta("skip_runtime", true)
	add_child(root)

	var we := root.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if we != null and we.environment != null:
		var env: Environment = we.environment.duplicate()
		env.nextgi_enabled = true
		we.environment = env

	var cam := root.get_node_or_null("FlightCam") as Camera3D
	_check(cam != null, "slow_io_flight: FlightCam exists")
	if cam == null:
		remove_child(root)
		root.free()
		return {}
	cam.current = true

	var xf0 := Flight.camera_transform(0)
	var xf80 := Flight.camera_transform(80)
	var separation := xf0.origin.distance_to(xf80.origin)
	_check(separation > 20.0 and separation < 180.0, "slow_io_flight: frame 0..80 separation %.1f m stays within 200 m RT radius" % separation)

	var mesh_a := _make_dense_flight_quad("flight_a", Color(0.20, 0.85, 0.30))
	var mesh_b := _make_dense_flight_quad("flight_b", Color(0.25, 0.45, 0.90))
	if mesh_a == null or mesh_b == null:
		remove_child(root)
		root.free()
		return {}

	var inst_a := MeshInstance3D.new()
	inst_a.name = "FlightVirtualA"
	inst_a.mesh = mesh_a
	inst_a.global_transform = xf0 * Transform3D(Basis.IDENTITY, Vector3(0.0, 0.0, -4.5))
	root.add_child(inst_a)

	var inst_b := MeshInstance3D.new()
	inst_b.name = "FlightVirtualB"
	inst_b.mesh = mesh_b
	inst_b.global_transform = xf80 * Transform3D(Basis.IDENTITY, Vector3(0.0, 0.0, -4.5))
	root.add_child(inst_b)

	cam.global_transform = xf0

	# Let initial BLAS builds & compactions finish at frame 0.
	for _i in range(8):
		await RenderingServer.frame_post_draw

	var base_counters: Dictionary = RenderingServer.nextgi_get_counters()
	var base_proxy_parts := int(base_counters.get("proxy_blas_parts", 0))
	var base_proxy_bytes := int(base_counters.get("proxy_blas_bytes_compacted", 0))
	_check(base_proxy_parts >= 2, "slow_io_flight: both virtualized meshes built proxy BLASes (parts=%d)" % base_proxy_parts)
	_check(base_proxy_bytes > 0, "slow_io_flight: compacted proxy bytes > 0 (bytes=%d)" % base_proxy_bytes)

	var vg_a := mesh_a.get_virtual_geometry()
	var vg_b := mesh_b.get_virtual_geometry()
	var non_root_a := vg_a.get_page_count() - vg_a.get_root_pages().size()
	var non_root_b := vg_b.get_page_count() - vg_b.get_root_pages().size()

	var blas_rebuilds_during_streaming := 0
	var rc_start: Dictionary = RenderingServer.virtual_geometry_get_routing_counters()
	var base_uploads := int(rc_start.get("page_uploads", 0))
	var base_evictions := int(rc_start.get("page_evictions", 0))

	# Stream FlightVirtualA's fine pages into the 16-slot cache at frame 0.
	for _f in range(70):
		OS.delay_msec(8)
		await RenderingServer.frame_post_draw
		var info: Dictionary = RenderingServer.nextgi_get_tier_info()
		blas_rebuilds_during_streaming += int(info.get("rt_blas_build_count", 0))
		var rc: Dictionary = RenderingServer.virtual_geometry_get_routing_counters()
		if int(rc.get("page_uploads", 0)) >= base_uploads + non_root_a:
			break

	var rc_mid: Dictionary = RenderingServer.virtual_geometry_get_routing_counters()
	var uploads_after_a := int(rc_mid.get("page_uploads", 0))

	# Advance FlightCam along the Slow IO Flight spline to frame 80 so FlightVirtualA's
	# off-screen fine pages evict while FlightVirtualB streams in (both remain inside the 200 m TLAS!).
	cam.global_transform = xf80
	var evictions_seen := 0
	for _f in range(70):
		OS.delay_msec(8)
		await RenderingServer.frame_post_draw
		var info: Dictionary = RenderingServer.nextgi_get_tier_info()
		var counters: Dictionary = RenderingServer.nextgi_get_counters()
		blas_rebuilds_during_streaming += int(info.get("rt_blas_build_count", 0))
		_check(int(counters.get("proxy_blas_parts", 0)) == base_proxy_parts, "slow_io_flight: proxy_blas_parts stays constant across page eviction")
		_check(int(counters.get("proxy_blas_bytes_compacted", 0)) == base_proxy_bytes, "slow_io_flight: proxy_blas_bytes_compacted stays constant across page eviction")
		var rc: Dictionary = RenderingServer.virtual_geometry_get_routing_counters()
		evictions_seen = int(rc.get("page_evictions", 0)) - base_evictions
		if int(rc.get("page_uploads", 0)) >= uploads_after_a + non_root_b and evictions_seen > 0:
			break

	_check(evictions_seen > 0, "slow_io_flight: expected page_evictions > 0 under 2 MiB cache, got %d" % evictions_seen)
	_check(blas_rebuilds_during_streaming == 0, "slow_io_flight: page eviction rebuilds 0 BLASes (got %d)" % blas_rebuilds_during_streaming)

	ProjectSettings.set_setting("rendering/nextgeo/streaming/debug_io_delay_ms", Vector2(0.0, 0.0))
	ProjectSettings.set_setting("rendering/limits/nextgeo/geometry_cache_mb", Flight.GEOMETRY_CACHE_MB)

	remove_child(root)
	root.free()
	for _i in range(3):
		await RenderingServer.frame_post_draw

	return {
		"separation_m": snappedf(separation, 0.01),
		"proxy_blas_parts": base_proxy_parts,
		"proxy_blas_bytes_compacted": base_proxy_bytes,
		"page_evictions": evictions_seen,
		"blas_rebuilds_during_eviction": blas_rebuilds_during_streaming,
	}
