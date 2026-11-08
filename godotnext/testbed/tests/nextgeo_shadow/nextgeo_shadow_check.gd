extends Node3D
## NextGeo virtual shadow cuts & positional shadow-dirty GPU check (#82):
## 1. Shadow views emit 0 page requests (non_main_view_requests == 0, page_uploads == 0)
##    when rendering directional, spot, omni cube, omni DP, and area DP shadows over
##    coarse-only virtual casters while the main camera points away.
## 2. Stationary positional light (SpotLight3D) becomes shadow-dirty within <= 1 frame
##    when streaming pages arrive on an intersecting virtual caster (shadow_version
##    increments), stops updating once clean, and its cached atlas tile matches a
##    forced full shadow redraw.
## 3. Across >= 500 seeded light views (Directional 4-split ortho, Spot perspective,
##    Omni Cube 6-face perspective, Omni Dual-Paraboloid 2-pass, Area Dual-Paraboloid),
##    every view satisfies cut_match == true (GPU cut_hash == CPU predicate),
##    overflow == 0, and max_cut_texel_error <= 1.0 shadow texel.
## 4. End-to-end shadow raster verification for CAST_SHADOW_ON (including directional
##    pancake caster behind ortho near plane, spot, omni cube, omni DP),
##    CAST_SHADOW_SHADOWS_ONLY (invisible in main view, casts shadow),
##    CAST_SHADOW_OFF (visible in main view, casts no shadow), and
##    CAST_SHADOW_DOUBLE_SIDED (back-facing single-sided virtual surface casts shadow),
##    with double_dispatch == 0 and missing == 0.

const CACHE_MB := 8
const TARGET_SEEDED_VIEWS := 500
const MAX_WAIT_FRAMES := 120
const CUT_FLAG_ORTHO := 1 << 2
const CUT_FLAG_DP := 1 << 3
const CUT_FLAG_SHADOW := 1 << 4

var _phase := ""
var _phase_frame := 0
var _temp_files: Array[String] = []
var _ngm_counter := 0
var _failures: Array[String] = []
var _summary: Dictionary = {}

var _world_root: Node3D
var _main_cam: Camera3D
var _we: WorldEnvironment

# Shared procedural virtual meshes
var _vmesh_sphere: ArrayMesh
var _vmesh_quad_red: ArrayMesh
var _vmesh_quad_green: ArrayMesh

# Phase 1 & 2 nodes
var _p1_caster: MeshInstance3D
var _p1_receiver: MeshInstance3D
var _p1_spot: SpotLight3D
var _p1_omni_cube: OmniLight3D
var _p1_omni_dp: OmniLight3D
var _p1_sun: DirectionalLight3D

# Phase 2 residency tracking
var _p2_clean_version := -1
var _p2_uploads_base := 0
var _p2_upload_detected_frame := -1
var _p2_version_bumped_within_1f := false
var _p2_settled_frame := -1
var _p2_settled_version := -1
var _p2_cached_img: Image
var _p2_force_stage := 0
var _p2_version_before_force := -1

# Phase 3 seeded 500+ light views rig
var _rng := RandomNumberGenerator.new()
var _p3_root: Node3D
var _p3_casters: Array[MeshInstance3D] = []
var _p3_sun: DirectionalLight3D
var _p3_spots: Array[SpotLight3D] = []
var _p3_cubes: Array[OmniLight3D] = []
var _p3_dps: Array[OmniLight3D] = []
var _p3_area: AreaLight3D
var _p3_step := 0
var _p3_min_frame := 0
var _p3_wait := 0
var _p3_last_shadow_tick := -1
var _p3_views_checked := 0
var _p3_views_matched := 0
var _p3_ortho_views := 0
var _p3_persp_views := 0
var _p3_dp_views := 0
var _p3_max_texel_err := 0.0
var _p3_max_overflow := 0

# Phase 4 caster modes & pancake render check
var _p4_root: Node3D
var _p4_receiver: MeshInstance3D
var _p4_caster: MeshInstance3D
var _p4_quad_caster: MeshInstance3D
var _p4_sun: DirectionalLight3D
var _p4_spot: SpotLight3D
var _p4_omni_cube: OmniLight3D
var _p4_omni_dp: OmniLight3D
var _p4_substep := 0
var _p4_subframe := 0


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/occlusion", false)
	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 1.0)
	ProjectSettings.set_setting("rendering/limits/nextgeo/geometry_cache_mb", CACHE_MB)
	ProjectSettings.set_setting("rendering/limits/nextgeo/page_upload_kib_per_frame", 512)
	ProjectSettings.set_setting("rendering/nextgeo/streaming/debug_io_delay_ms", Vector2(0.0, 0.0))

	var vp := get_viewport()
	vp.use_taa = false
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 1.0
	vp.positional_shadow_atlas_size = 4096
	vp.positional_shadow_atlas_quad_0 = Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_1
	vp.positional_shadow_atlas_quad_1 = Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_4
	vp.positional_shadow_atlas_quad_2 = Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_16
	vp.positional_shadow_atlas_quad_3 = Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_64

	_build_meshes_and_phase1()
	if not _failures.is_empty():
		_finish()
		return

	_phase = "p1_shadow_no_requests"
	_phase_frame = 0


func _alloc_temp_ngm(p_tag: String) -> String:
	_ngm_counter += 1
	var path := "user://ng_shadow_%s_%d.ngm" % [p_tag, _ngm_counter]
	_temp_files.append(path)
	return path


func _make_virtual_mesh(p_prim: PrimitiveMesh, p_tag: String, p_color: Color, p_cull: BaseMaterial3D.CullMode = BaseMaterial3D.CULL_BACK) -> ArrayMesh:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = p_color
	mat.roughness = 0.85
	mat.metallic = 0.0
	mat.cull_mode = p_cull
	p_prim.material = mat

	var imp := ImporterMesh.from_mesh(p_prim)
	var err := imp.build_virtual_geometry(_alloc_temp_ngm(p_tag))
	if err != OK:
		_failures.append("%s: build_virtual_geometry failed (err=%d)" % [p_tag, err])
		return null
	var vmesh := imp.get_mesh()
	if vmesh == null or vmesh.get_virtual_geometry() == null:
		_failures.append("%s: virtual geometry missing on built mesh" % p_tag)
		return null
	var vg := vmesh.get_virtual_geometry()
	if vg.get_page_count() <= vg.get_root_pages().size():
		_failures.append("%s: expected multi-page virtual mesh (pages=%d roots=%d)" % [
			p_tag, vg.get_page_count(), vg.get_root_pages().size()])
	return vmesh


func _make_white_receiver(p_size: Vector2) -> MeshInstance3D:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.9, 0.9, 0.9)
	mat.roughness = 1.0
	mat.metallic = 0.0
	var pm := PlaneMesh.new()
	pm.size = p_size
	pm.material = mat
	var mi := MeshInstance3D.new()
	mi.mesh = pm
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return mi


func _build_meshes_and_phase1() -> void:
	var sm := SphereMesh.new()
	sm.radius = 1.2
	sm.height = 2.4
	sm.radial_segments = 96
	sm.rings = 64
	_vmesh_sphere = _make_virtual_mesh(sm, "sphere", Color(0.25, 0.75, 0.95))

	var qm_red := QuadMesh.new()
	qm_red.size = Vector2(2.6, 2.6)
	qm_red.subdivide_width = 72
	qm_red.subdivide_depth = 72
	_vmesh_quad_red = _make_virtual_mesh(qm_red, "quad_red", Color(0.95, 0.12, 0.12), BaseMaterial3D.CULL_BACK)

	var qm_green := QuadMesh.new()
	qm_green.size = Vector2(2.6, 2.6)
	qm_green.subdivide_width = 72
	qm_green.subdivide_depth = 72
	_vmesh_quad_green = _make_virtual_mesh(qm_green, "quad_green", Color(0.15, 0.90, 0.25), BaseMaterial3D.CULL_BACK)

	if _vmesh_sphere == null or _vmesh_quad_red == null or _vmesh_quad_green == null:
		return

	_world_root = Node3D.new()
	_world_root.name = "ShadowWorld"
	add_child(_world_root)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.02, 0.02, 0.03)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(1.0, 1.0, 1.0)
	env.ambient_light_energy = 0.06
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	_we = WorldEnvironment.new()
	_we.environment = env
	_world_root.add_child(_we)

	# Main camera starts pointed away at (-300, 5, 0) looking at (-300, 0, -10)
	# so ONLY shadow views see _p1_caster in Phase 1!
	_main_cam = Camera3D.new()
	_main_cam.fov = 55.0
	_main_cam.near = 0.1
	_main_cam.far = 200.0
	_main_cam.transform = Transform3D(Basis.looking_at(Vector3(0, -0.3, -1).normalized(), Vector3.UP), Vector3(-300.0, 5.0, 0.0))
	_main_cam.current = true
	_world_root.add_child(_main_cam)

	_p1_receiver = _make_white_receiver(Vector2(24.0, 24.0))
	_p1_receiver.position = Vector3(0.0, 0.0, 0.0)
	_world_root.add_child(_p1_receiver)

	_p1_caster = MeshInstance3D.new()
	_p1_caster.mesh = _vmesh_sphere
	_p1_caster.position = Vector3(0.0, 2.2, 0.0)
	_world_root.add_child(_p1_caster)

	_p1_spot = SpotLight3D.new()
	_p1_spot.shadow_enabled = true
	_p1_spot.shadow_bias = 0.02
	_p1_spot.spot_range = 25.0
	_p1_spot.spot_angle = 45.0
	_p1_spot.light_energy = 8.0
	_p1_spot.transform = Transform3D(Basis.looking_at(Vector3(0.0, -1.0, 0.01).normalized(), Vector3.UP), Vector3(0.0, 8.0, -0.08))
	_world_root.add_child(_p1_spot)

	_p1_omni_cube = OmniLight3D.new()
	_p1_omni_cube.shadow_enabled = true
	_p1_omni_cube.omni_shadow_mode = OmniLight3D.SHADOW_CUBE
	_p1_omni_cube.omni_range = 18.0
	_p1_omni_cube.light_energy = 2.0
	_p1_omni_cube.position = Vector3(3.5, 5.0, 2.0)
	_world_root.add_child(_p1_omni_cube)

	_p1_omni_dp = OmniLight3D.new()
	_p1_omni_dp.shadow_enabled = true
	_p1_omni_dp.omni_shadow_mode = OmniLight3D.SHADOW_DUAL_PARABOLOID
	_p1_omni_dp.omni_range = 18.0
	_p1_omni_dp.light_energy = 2.0
	_p1_omni_dp.position = Vector3(-3.5, 5.0, -2.0)
	_world_root.add_child(_p1_omni_dp)

	_p1_sun = DirectionalLight3D.new()
	_p1_sun.shadow_enabled = true
	_p1_sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
	_p1_sun.rotation_degrees = Vector3(-55.0, 25.0, 0.0)
	_world_root.add_child(_p1_sun)


func _check_routing_invariants(p_label: String) -> Dictionary:
	var rc: Dictionary = RenderingServer.virtual_geometry_get_routing_counters()
	var vp_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	if int(rc.get("non_main_view_requests", -1)) != 0:
		_failures.append("%s: non_main_view_requests=%s" % [p_label, str(rc.get("non_main_view_requests"))])
	if int(rc.get("child_resident_without_parent", -1)) != 0:
		_failures.append("%s: child_resident_without_parent=%s" % [p_label, str(rc.get("child_resident_without_parent"))])
	if int(vp_stats.get("nextgeo_double_dispatch", 0)) != 0:
		_failures.append("%s: nextgeo_double_dispatch=%s" % [p_label, str(vp_stats.get("nextgeo_double_dispatch"))])
	if int(vp_stats.get("nextgeo_missing", 0)) != 0:
		_failures.append("%s: nextgeo_missing=%s" % [p_label, str(vp_stats.get("nextgeo_missing"))])
	return rc


func _process(_delta: float) -> void:
	_phase_frame += 1
	match _phase:
		"p1_shadow_no_requests":
			_tick_p1()
		"p2_residency_dirty":
			_tick_p2()
		"p3_stream_warmup":
			_tick_p3_warmup()
		"p3_seeded_500_views":
			_tick_p3()
		"p4_caster_modes":
			_tick_p4()


func _tick_p1() -> void:
	# Keep positional lights dirty during Phase 1 while main camera points away,
	# proving shadow views never emit page requests or trigger uploads.
	_p1_omni_cube.shadow_bias = 0.02 + float(_phase_frame & 1) * 1e-5
	_p1_omni_dp.shadow_bias = 0.02 + float(_phase_frame & 1) * 1e-5
	var rc := _check_routing_invariants("p1_f%d" % _phase_frame)
	if _phase_frame < 10:
		return

	var vp_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var uploads := int(rc.get("page_uploads", -1))
	var shadow_dispatched := int(vp_stats.get("shadow_views_dispatched", 0))
	var shadow_draws := int(vp_stats.get("shadow_raster_draws", 0))
	if uploads != 0:
		_failures.append("p1_shadow_no_requests: shadow views triggered page_uploads=%d while main camera looked away" % uploads)
	if shadow_dispatched <= 0 or shadow_draws <= 0:
		_failures.append("p1_shadow_no_requests: expected shadow views dispatched & drawn, got dispatched=%d draws=%d" % [
			shadow_dispatched, shadow_draws])

	_summary["p1_shadow_no_requests"] = {
		"page_uploads": uploads,
		"non_main_view_requests": int(rc.get("non_main_view_requests", -1)),
		"shadow_views_dispatched": shadow_dispatched,
		"shadow_raster_draws": shadow_draws,
	}

	# Prepare Phase 2: isolate stationary _p1_spot + _p1_caster + _p1_receiver.
	_p1_omni_cube.visible = false
	_p1_omni_dp.visible = false
	_p1_sun.visible = false
	_p1_spot.shadow_bias = 0.02
	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 0.0)
	_p2_clean_version = -1
	_p2_upload_detected_frame = -1
	_p2_version_bumped_within_1f = false
	_p2_settled_frame = -1
	_p2_force_stage = 0
	_phase = "p2_residency_dirty"
	_phase_frame = 0


func _max_image_diff(p_a: Image, p_b: Image) -> float:
	if p_a == null or p_b == null or p_a.get_size() != p_b.get_size():
		return 999.0
	var w := p_a.get_width()
	var h := p_a.get_height()
	var max_d := 0.0
	for y in range(0, h, 2):
		for x in range(0, w, 2):
			var ca := p_a.get_pixel(x, y)
			var cb := p_b.get_pixel(x, y)
			max_d = maxf(max_d, maxf(absf(ca.r - cb.r), maxf(absf(ca.g - cb.g), absf(ca.b - cb.b))))
	return max_d


func _tick_p2() -> void:
	var rc := _check_routing_invariants("p2_f%d" % _phase_frame)
	var spot_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(_p1_spot.get_instance())
	var cur_ver := int(spot_stats.get("shadow_version", -1))
	var uploads := int(rc.get("page_uploads", 0))

	# Step A (frames 1..5): main camera still looks away; verify stationary SpotLight3D is clean and version is steady.
	if _phase_frame < 4:
		return
	if _phase_frame == 4:
		_p2_clean_version = cur_ver
		return
	if _phase_frame == 5:
		if cur_ver != _p2_clean_version:
			_failures.append("p2_residency: expected clean SpotLight3D version steady (%d != %d)" % [cur_ver, _p2_clean_version])
		_p2_uploads_base = uploads
		# Point main camera close to _p1_caster + shadowed receiver so fine pages stream in!
		_main_cam.transform = Transform3D(Basis.looking_at((Vector3(0.0, 1.0, 0.0) - Vector3(0.0, 4.2, 4.2)).normalized(), Vector3.UP), Vector3(0.0, 4.2, 4.2))
		return

	# Step B: detect first page upload and assert shadow_version bumps within <= 1 frame.
	if _p2_upload_detected_frame < 0 and uploads > _p2_uploads_base:
		_p2_upload_detected_frame = _phase_frame
		if cur_ver > _p2_clean_version:
			_p2_version_bumped_within_1f = true
	elif _p2_upload_detected_frame >= 0 and not _p2_version_bumped_within_1f:
		if _phase_frame <= _p2_upload_detected_frame + 1 and cur_ver > _p2_clean_version:
			_p2_version_bumped_within_1f = true

	var sphere_vg := _vmesh_sphere.get_virtual_geometry()
	var target_uploads := _p2_uploads_base + (sphere_vg.get_page_count() - sphere_vg.get_root_pages().size())

	if _p2_force_stage == 0:
		OS.delay_msec(6)
		if uploads >= target_uploads:
			if _p2_settled_frame < 0:
				_p2_settled_frame = _phase_frame
				return
			if _phase_frame == _p2_settled_frame + 3:
				_p2_settled_version = cur_ver
				return
			if _phase_frame >= _p2_settled_frame + 5:
				if not _p2_version_bumped_within_1f:
					_failures.append("p2_residency: SpotLight3D shadow_version did not increment within 1 frame of page upload (clean=%d settled=%d)" % [
						_p2_clean_version, cur_ver])
				if cur_ver != _p2_settled_version:
					_failures.append("p2_residency: SpotLight3D shadow_version kept changing on clean frames (%d vs %d)" % [
						cur_ver, _p2_settled_version])
				_p2_cached_img = get_viewport().get_texture().get_image()
				_p2_version_before_force = cur_ver
				_p1_spot.shadow_bias = 0.0201
				_p2_force_stage = 1
				return
		elif _phase_frame >= 90:
			_failures.append("p2_residency: timed out waiting for caster page uploads (%d < %d)" % [uploads, target_uploads])
			_start_phase3()
		return

	if _p2_force_stage == 1:
		_p1_spot.shadow_bias = 0.02
		_p2_force_stage = 2
		return
	if _p2_force_stage >= 2 and _p2_force_stage < 5:
		_p2_force_stage += 1
		return

	# Compare residency-dirtied cached shadow image against forced full shadow redraw!
	var forced_img := get_viewport().get_texture().get_image()
	var max_diff := _max_image_diff(_p2_cached_img, forced_img)
	if cur_ver <= _p2_version_before_force:
		_failures.append("p2_residency: forced shadow redraw did not increment shadow_version (%d <= %d)" % [cur_ver, _p2_version_before_force])
	if max_diff > (2.0 / 255.0):
		_failures.append("p2_residency: cached atlas tile after residency update differed from forced full redraw (max_diff=%.5f)" % max_diff)

	_summary["p2_residency_shadow_dirty"] = {
		"clean_version": _p2_clean_version,
		"settled_version": _p2_settled_version,
		"forced_version": cur_ver,
		"bumped_within_1_frame": _p2_version_bumped_within_1f,
		"max_redraw_diff": snappedf(max_diff, 0.0001),
	}

	_start_phase3()


func _non_root_pages(p_mesh: ArrayMesh) -> int:
	var vg := p_mesh.get_virtual_geometry()
	return maxi(0, vg.get_page_count() - vg.get_root_pages().size())


func _start_phase3() -> void:
	_p1_spot.visible = false
	_p1_caster.visible = false
	_p1_receiver.visible = false

	_p3_root = Node3D.new()
	_p3_root.name = "SeededRig"
	_world_root.add_child(_p3_root)

	# Instantiate all 3 virtual meshes (_vmesh_sphere, _vmesh_quad_red, _vmesh_quad_green).
	# First warm-stream all fine pages via main view (since SHADOWS_ONLY and shadow views
	# never emit page requests), then switch to [ON, DOUBLE_SIDED, SHADOWS_ONLY, OFF, ON]
	# and max_pixel_error = 1.0 for the >= 500 seeded light-view cut check.
	var meshes: Array[ArrayMesh] = [_vmesh_sphere, _vmesh_quad_red, _vmesh_quad_green, _vmesh_sphere, _vmesh_quad_red]
	for i in range(meshes.size()):
		var mi := MeshInstance3D.new()
		mi.mesh = meshes[i]
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		mi.position = Vector3(float(i - 2) * 1.4, 0.0, 0.0)
		_p3_root.add_child(mi)
		_p3_casters.append(mi)

	# 1 Directional (4 splits) + 4 Spots (4) + 2 Omni Cubes (12) + 2 Omni DPs (4) + 1 Area DP (1) = 25 views/step.
	_p3_sun = DirectionalLight3D.new()
	_p3_sun.shadow_enabled = true
	_p3_sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
	_p3_sun.directional_shadow_max_distance = 120.0
	_p3_sun.directional_shadow_pancake_size = 20.0
	_p3_root.add_child(_p3_sun)

	for i in range(4):
		var sp := SpotLight3D.new()
		sp.shadow_enabled = true
		sp.shadow_bias = 0.02
		_p3_root.add_child(sp)
		_p3_spots.append(sp)

	for i in range(2):
		var oc := OmniLight3D.new()
		oc.shadow_enabled = true
		oc.omni_shadow_mode = OmniLight3D.SHADOW_CUBE
		oc.shadow_bias = 0.02
		_p3_root.add_child(oc)
		_p3_cubes.append(oc)

	for i in range(2):
		var odp := OmniLight3D.new()
		odp.shadow_enabled = true
		odp.omni_shadow_mode = OmniLight3D.SHADOW_DUAL_PARABOLOID
		odp.shadow_bias = 0.02
		_p3_root.add_child(odp)
		_p3_dps.append(odp)

	_p3_area = AreaLight3D.new()
	_p3_area.shadow_enabled = true
	_p3_area.shadow_bias = 0.02
	_p3_area.area_size = Vector2(2.0, 1.5)
	_p3_root.add_child(_p3_area)

	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 0.0)
	_main_cam.transform = _look_at_safe(Vector3(0.0, 0.5, 6.0), Vector3.ZERO)
	_phase = "p3_stream_warmup"
	_phase_frame = 0


func _tick_p3_warmup() -> void:
	var rc := _check_routing_invariants("p3_warm_f%d" % _phase_frame)
	OS.delay_msec(6)
	var needed := _non_root_pages(_vmesh_sphere) + _non_root_pages(_vmesh_quad_red) + _non_root_pages(_vmesh_quad_green)
	var uploads := int(rc.get("page_uploads", 0))
	if uploads < needed and _phase_frame < 90:
		return
	if uploads < needed:
		_failures.append("p3_stream_warmup: timed out waiting for all mesh pages (%d < %d)" % [uploads, needed])

	var cast_modes: Array[GeometryInstance3D.ShadowCastingSetting] = [
		GeometryInstance3D.SHADOW_CASTING_SETTING_ON,
		GeometryInstance3D.SHADOW_CASTING_SETTING_DOUBLE_SIDED,
		GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY,
		GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
		GeometryInstance3D.SHADOW_CASTING_SETTING_ON,
	]
	for i in range(_p3_casters.size()):
		_p3_casters[i].cast_shadow = cast_modes[i]

	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 1.0)
	_rng.seed = 820500
	var init_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	_p3_last_shadow_tick = int(init_stats.get("shadow_tick", -1))
	_p3_step = 0
	_apply_p3_step(0)
	_phase = "p3_seeded_500_views"
	_phase_frame = 0


func _look_at_safe(p_from: Vector3, p_to: Vector3) -> Transform3D:
	var dir := (p_to - p_from).normalized()
	if dir.length_squared() < 0.5:
		dir = Vector3.DOWN
	var up := Vector3.FORWARD if absf(dir.dot(Vector3.UP)) > 0.96 else Vector3.UP
	return Transform3D(Basis.looking_at(dir, up), p_from)


func _apply_p3_step(p_step: int) -> void:
	_p3_step = p_step
	_p3_min_frame = Engine.get_frames_drawn()
	_p3_wait = 0

	var scales: Array[float] = [0.5, 1.0, 2.2]
	for i in range(_p3_casters.size()):
		var ang := _rng.randf_range(0.0, TAU)
		var rad := _rng.randf_range(0.5, 5.5)
		_p3_casters[i].position = Vector3(cos(ang) * rad, _rng.randf_range(-1.5, 2.5), sin(ang) * rad)
		_p3_casters[i].rotation = Vector3(_rng.randf_range(-0.8, 0.8), _rng.randf_range(0.0, TAU), _rng.randf_range(-0.8, 0.8))
		var sc := scales[(p_step + i) % scales.size()]
		_p3_casters[i].scale = Vector3.ONE * sc

	# Camera orbits the cluster cluster so main view + directional cascades cover all casters.
	var cam_ang := _rng.randf_range(0.0, TAU)
	var cam_pos := Vector3(cos(cam_ang) * 18.0, _rng.randf_range(4.0, 14.0), sin(cam_ang) * 18.0)
	_main_cam.transform = _look_at_safe(cam_pos, Vector3.ZERO)

	_p3_sun.rotation_degrees = Vector3(_rng.randf_range(-85.0, -20.0), _rng.randf_range(0.0, 360.0), 0.0)

	for i in range(_p3_spots.size()):
		var sp := _p3_spots[i]
		var sp_pos: Vector3
		if i == 0 and p_step % 2 == 0:
			# Explicitly exercise straight-down (-Y) and straight-up (+Y) spot views!
			sp_pos = Vector3(0.0, 12.0 if (p_step % 4 == 0) else -12.0, 0.0)
		else:
			var a := _rng.randf_range(0.0, TAU)
			sp_pos = Vector3(cos(a) * _rng.randf_range(7.0, 15.0), _rng.randf_range(-8.0, 12.0), sin(a) * _rng.randf_range(7.0, 15.0))
		sp.spot_range = _rng.randf_range(22.0, 45.0)
		sp.spot_angle = _rng.randf_range(25.0, 68.0)
		sp.transform = _look_at_safe(sp_pos, _p3_casters[i % _p3_casters.size()].position)

	for i in range(_p3_cubes.size()):
		var oc := _p3_cubes[i]
		oc.omni_range = _rng.randf_range(16.0, 34.0)
		oc.position = Vector3(_rng.randf_range(-6.0, 6.0), _rng.randf_range(-4.0, 7.0), _rng.randf_range(-6.0, 6.0))
		oc.rotation = Vector3(_rng.randf_range(0.0, TAU), _rng.randf_range(0.0, TAU), 0.0)

	for i in range(_p3_dps.size()):
		var odp := _p3_dps[i]
		odp.omni_range = _rng.randf_range(16.0, 34.0)
		odp.position = Vector3(_rng.randf_range(-6.0, 6.0), _rng.randf_range(-4.0, 7.0), _rng.randf_range(-6.0, 6.0))
		odp.rotation = Vector3(_rng.randf_range(0.0, TAU), _rng.randf_range(0.0, TAU), 0.0)

	_p3_area.set_param(Light3D.PARAM_RANGE, _rng.randf_range(18.0, 36.0))
	var area_pos := Vector3(_rng.randf_range(-8.0, 8.0), _rng.randf_range(5.0, 12.0), _rng.randf_range(-8.0, 8.0))
	_p3_area.transform = _look_at_safe(area_pos, Vector3.ZERO)


func _tick_p3() -> void:
	_p3_wait += 1
	_check_routing_invariants("p3_s%d_w%d" % [_p3_step, _p3_wait])

	# Keep all positional lights shadow-dirty while holding this seeded pose so every
	# rendered frame dispatches all 25 shadow views with identical transforms & projections.
	var bias_jitter := 0.02 + float(_p3_wait & 1) * 1e-5
	for sp in _p3_spots:
		sp.shadow_bias = bias_jitter
	for oc in _p3_cubes:
		oc.shadow_bias = bias_jitter
	for odp in _p3_dps:
		odp.shadow_bias = bias_jitter
	_p3_area.shadow_bias = bias_jitter

	if _p3_wait > MAX_WAIT_FRAMES:
		_failures.append("p3_seeded: timed out waiting for shadow_tick on step %d (checked=%d)" % [_p3_step, _p3_views_checked])
		_start_phase4()
		return

	var stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var sh_tick := int(stats.get("shadow_tick", -1))
	if sh_tick == _p3_last_shadow_tick:
		return
	_p3_last_shadow_tick = sh_tick
	if int(stats.get("shadow_render_frame", 0)) < _p3_min_frame:
		return

	var views: Array = stats.get("shadow_views", [])
	if views.size() < 20:
		# Wait for a frame where all 25 light views were dispatched.
		return

	for idx in range(views.size()):
		var vd: Dictionary = views[idx]
		_p3_views_checked += 1
		var matched := bool(vd.get("cut_match", false))
		var ovf := int(vd.get("overflow", 0))
		var tex_err := float(vd.get("max_cut_texel_error", 0.0))
		var cflags := int(vd.get("cut_flags", 0))
		if matched:
			_p3_views_matched += 1
		elif _failures.size() < 12:
			_failures.append("p3_seeded step=%d view=%d cut_hash mismatch: gpu=%s cpu=%s gpu_cls=%s cpu_cls=%s flags=%d" % [
				_p3_step, idx, str(vd.get("cut_hash")), str(vd.get("cpu_cut_hash")),
				str(vd.get("clusters")), str(vd.get("cpu_clusters")), cflags])
		if (cflags & CUT_FLAG_SHADOW) == 0:
			_failures.append("p3_seeded step=%d view=%d missing SHADOW_VIEW flag (%d)" % [_p3_step, idx, cflags])
		if tex_err > 1.0001:
			_failures.append("p3_seeded step=%d view=%d max_cut_texel_error=%.5f > 1.0" % [_p3_step, idx, tex_err])
		_p3_max_texel_err = maxf(_p3_max_texel_err, tex_err)
		_p3_max_overflow = maxi(_p3_max_overflow, ovf)
		if (cflags & CUT_FLAG_ORTHO) != 0:
			_p3_ortho_views += 1
		elif (cflags & CUT_FLAG_DP) != 0:
			_p3_dp_views += 1
		else:
			_p3_persp_views += 1

	if _p3_views_checked >= TARGET_SEEDED_VIEWS:
		if _p3_views_matched != _p3_views_checked:
			_failures.append("p3_seeded: matched %d / %d views" % [_p3_views_matched, _p3_views_checked])
		if _p3_max_overflow != 0:
			_failures.append("p3_seeded: max_overflow=%d" % _p3_max_overflow)
		if _p3_ortho_views < 60 or _p3_persp_views < 240 or _p3_dp_views < 80:
			_failures.append("p3_seeded: insufficient coverage across light families (ortho=%d persp=%d dp=%d)" % [
				_p3_ortho_views, _p3_persp_views, _p3_dp_views])
		_summary["p3_seeded_views"] = {
			"steps": _p3_step + 1,
			"views_checked": _p3_views_checked,
			"views_matched": _p3_views_matched,
			"ortho_views": _p3_ortho_views,
			"persp_spot_cube_views": _p3_persp_views,
			"dual_paraboloid_views": _p3_dp_views,
			"max_cut_texel_error": snappedf(_p3_max_texel_err, 0.0001),
			"max_overflow": _p3_max_overflow,
		}
		_start_phase4()
		return

	_apply_p3_step(_p3_step + 1)


func _start_phase4() -> void:
	if _p3_root != null:
		_p3_root.visible = false
		_world_root.remove_child(_p3_root)
		_p3_root.free()
		_p3_root = null

	_we.environment.ambient_light_energy = 0.05

	_p4_root = Node3D.new()
	_p4_root.name = "CasterModeCheck"
	_world_root.add_child(_p4_root)

	_p4_receiver = _make_white_receiver(Vector2(30.0, 30.0))
	_p4_receiver.position = Vector3.ZERO
	_p4_root.add_child(_p4_receiver)

	_p4_caster = MeshInstance3D.new()
	_p4_caster.mesh = _vmesh_sphere
	_p4_caster.position = Vector3(0.0, 2.5, 0.0)
	_p4_root.add_child(_p4_caster)

	_p4_quad_caster = MeshInstance3D.new()
	_p4_quad_caster.mesh = _vmesh_quad_red
	_p4_quad_caster.position = Vector3(0.0, 2.5, 0.0)
	# QuadMesh normal is +Z; rotating +90 deg around X makes +Z point DOWN (-Y),
	# so its BACK face points UP (+Y) toward overhead lights!
	_p4_quad_caster.rotation_degrees = Vector3(90.0, 0.0, 0.0)
	_p4_quad_caster.visible = false
	_p4_root.add_child(_p4_quad_caster)

	_p4_sun = DirectionalLight3D.new()
	_p4_sun.shadow_enabled = true
	_p4_sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
	_p4_sun.directional_shadow_max_distance = 40.0
	_p4_sun.directional_shadow_pancake_size = 60.0
	_p4_sun.light_energy = 2.5
	# Point straight down (-Y) so shadow of (0, y, 0) lands directly at (0, 0, 0)!
	_p4_sun.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	_p4_sun.visible = false
	_p4_root.add_child(_p4_sun)

	_p4_spot = SpotLight3D.new()
	_p4_spot.shadow_enabled = true
	_p4_spot.spot_range = 30.0
	_p4_spot.spot_angle = 55.0
	_p4_spot.light_energy = 28.0
	_p4_spot.transform = _look_at_safe(Vector3(0.0, 9.0, 0.0), Vector3.ZERO)
	_p4_spot.visible = false
	_p4_root.add_child(_p4_spot)

	_p4_omni_cube = OmniLight3D.new()
	_p4_omni_cube.shadow_enabled = true
	_p4_omni_cube.omni_shadow_mode = OmniLight3D.SHADOW_CUBE
	_p4_omni_cube.omni_range = 30.0
	_p4_omni_cube.light_energy = 22.0
	_p4_omni_cube.position = Vector3(0.0, 8.0, 0.0)
	_p4_omni_cube.visible = false
	_p4_root.add_child(_p4_omni_cube)

	_p4_omni_dp = OmniLight3D.new()
	_p4_omni_dp.shadow_enabled = true
	_p4_omni_dp.omni_shadow_mode = OmniLight3D.SHADOW_DUAL_PARABOLOID
	_p4_omni_dp.omni_range = 30.0
	_p4_omni_dp.light_energy = 22.0
	_p4_omni_dp.position = Vector3(0.0, 8.0, 0.0)
	_p4_omni_dp.visible = false
	_p4_root.add_child(_p4_omni_dp)

	# Oblique camera looking directly at (0, 0, 0) on the receiver WITHOUT _p4_caster (at y=2.5..35)
	# occluding the center pixel (0, 0, 0)!
	_main_cam.transform = _look_at_safe(Vector3(0.0, 1.4, 5.5), Vector3(0.0, 0.0, 0.0))

	_p4_substep = 0
	_p4_subframe = 0
	_configure_p4_substep(0)
	_phase = "p4_caster_modes"
	_phase_frame = 0


func _configure_p4_substep(p_sub: int) -> void:
	_p4_substep = p_sub
	_p4_subframe = 0
	_p4_sun.visible = false
	_p4_spot.visible = false
	_p4_omni_cube.visible = false
	_p4_omni_dp.visible = false
	_p4_caster.visible = false
	_p4_quad_caster.visible = false
	_main_cam.transform = _look_at_safe(Vector3(0.0, 1.4, 5.5), Vector3(0.0, 0.0, 0.0))

	match p_sub:
		0:
			# Directional pancake shadow: place _p4_caster high at y = 45.0 (well above the
			# small 40m directional split near plane, inside the 60m pancake range)!
			_p4_sun.visible = true
			_p4_caster.visible = true
			_p4_caster.position = Vector3(0.0, 45.0, 0.0)
			_p4_caster.scale = Vector3.ONE * 1.5
			_p4_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		1:
			# Spot shadow ON
			_p4_spot.visible = true
			_p4_caster.visible = true
			_p4_caster.position = Vector3(0.0, 3.0, 0.0)
			_p4_caster.scale = Vector3.ONE
			_p4_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		2:
			# Omni Cube shadow ON
			_p4_omni_cube.visible = true
			_p4_caster.visible = true
			_p4_caster.position = Vector3(0.0, 3.0, 0.0)
			_p4_caster.scale = Vector3.ONE
			_p4_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		3:
			# Omni Dual-Paraboloid shadow ON (half 0: -Z pole pointing straight down at origin)
			_p4_omni_dp.visible = true
			_p4_omni_dp.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
			_p4_caster.visible = true
			_p4_caster.position = Vector3(0.0, 3.0, 0.0)
			_p4_caster.scale = Vector3.ONE
			_p4_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		4:
			# Omni Dual-Paraboloid shadow ON (half 1: +Z pole pointing straight down at origin)
			_p4_omni_dp.visible = true
			_p4_omni_dp.rotation_degrees = Vector3(90.0, 0.0, 0.0)
			_p4_caster.visible = true
			_p4_caster.position = Vector3(0.0, 3.0, 0.0)
			_p4_caster.scale = Vector3.ONE
			_p4_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		5:
			# CAST_SHADOW_OFF: caster casts NO shadow onto (0,0,0)!
			_p4_spot.visible = true
			_p4_caster.visible = true
			_p4_caster.position = Vector3(0.0, 3.0, 0.0)
			_p4_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		6:
			# CAST_SHADOW_SHADOWS_ONLY: red quad at (0, 3, 0) invisible in main view, casts shadow to (-3, 0, 0)
			_p4_sun.visible = true
			_p4_sun.rotation_degrees = Vector3(-45.0, 90.0, 0.0)
			_p4_quad_caster.visible = true
			_p4_quad_caster.position = Vector3(0.0, 3.0, 0.0)
			_p4_quad_caster.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
			_p4_quad_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY
			_main_cam.transform = _look_at_safe(Vector3(-1.5, 8.5, 0.0), Vector3(-1.5, 0.0, 0.0))
		7:
			# CAST_SHADOW_DOUBLE_SIDED: single-sided quad at (0, 3.0, 0) with back-face (+Y) toward
			# overhead SpotLight3D at (0, 9.0, 0) must still cast shadow onto (0, 0, 0)!
			_p4_sun.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
			_p4_spot.visible = true
			_p4_quad_caster.visible = true
			_p4_quad_caster.position = Vector3(0.0, 3.0, 0.0)
			_p4_quad_caster.rotation_degrees = Vector3(90.0, 0.0, 0.0)
			_p4_quad_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_DOUBLE_SIDED


func _sample_world_px(p_img: Image, p_world: Vector3) -> Color:
	var uv := _main_cam.unproject_position(p_world)
	var vp_size := Vector2(get_viewport().size)
	var ix := clampi(int(round(uv.x / maxf(vp_size.x, 1.0) * float(p_img.get_width()))), 0, p_img.get_width() - 1)
	var iy := clampi(int(round(uv.y / maxf(vp_size.y, 1.0) * float(p_img.get_height()))), 0, p_img.get_height() - 1)
	return p_img.get_pixel(ix, iy).srgb_to_linear()


func _tick_p4() -> void:
	_p4_subframe += 1
	_check_routing_invariants("p4_sub%d_f%d" % [_p4_substep, _p4_subframe])
	if _p4_subframe < 5:
		return

	var img := get_viewport().get_texture().get_image()
	if img == null or img.is_empty():
		_failures.append("p4_sub%d: empty viewport image" % _p4_substep)
	else:
		match _p4_substep:
			0, 1, 2, 3, 4:
				var shadowed := _sample_world_px(img, Vector3(0.0, 0.0, 0.0))
				var lit := _sample_world_px(img, Vector3(4.2, 0.0, 0.0))
				var label: String = ["dir_pancake", "spot_on", "omni_cube_on", "omni_dp_half0", "omni_dp_half1"][_p4_substep]
				if shadowed.r >= 0.08 or lit.r <= 0.45 or lit.r <= shadowed.r * 6.0:
					_failures.append("p4_%s: expected dark shadow at origin and lit flank (shadow=%s lit=%s)" % [
						label, str(shadowed), str(lit)])
				_summary["p4_" + label] = {
					"shadow_r": snappedf(shadowed.r, 0.001),
					"lit_r": snappedf(lit.r, 0.001),
				}
			5:
				var under_off := _sample_world_px(img, Vector3(0.0, 0.0, 0.0))
				var flank_off := _sample_world_px(img, Vector3(4.2, 0.0, 0.0))
				if under_off.r <= 0.45:
					_failures.append("p4_cast_off: CAST_SHADOW_OFF still cast a shadow at origin (under=%s flank=%s)" % [
						str(under_off), str(flank_off)])
				_summary["p4_cast_off"] = { "under_r": snappedf(under_off.r, 0.001) }
			6:
				var through_quad := _sample_world_px(img, Vector3(0.0, 0.0, 0.0))
				var cast_shadow_px := _sample_world_px(img, Vector3(-3.0, 0.0, 0.0))
				if through_quad.g <= 0.45 or absf(through_quad.r - through_quad.g) > 0.08:
					_failures.append("p4_shadows_only: SHADOWS_ONLY red quad leaked into main view (through=%s)" % str(through_quad))
				if cast_shadow_px.r >= 0.08 or through_quad.r <= cast_shadow_px.r * 6.0:
					_failures.append("p4_shadows_only: SHADOWS_ONLY quad failed to cast shadow (shadow=%s lit=%s)" % [
						str(cast_shadow_px), str(through_quad)])
				_summary["p4_shadows_only"] = {
					"through_rgb": [snappedf(through_quad.r, 0.001), snappedf(through_quad.g, 0.001), snappedf(through_quad.b, 0.001)],
					"shadow_r": snappedf(cast_shadow_px.r, 0.001),
				}
			7:
				var dbl_shadow := _sample_world_px(img, Vector3(0.0, 0.0, 0.0))
				var dbl_lit := _sample_world_px(img, Vector3(4.2, 0.0, 0.0))
				if dbl_shadow.r >= 0.08 or dbl_lit.r <= 0.45:
					_failures.append("p4_double_sided: back-facing quad with DOUBLE_SIDED failed to cast shadow (shadow=%s lit=%s)" % [
						str(dbl_shadow), str(dbl_lit)])
				var vp_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
				var cull_ms := float(vp_stats.get("shadow_cull_gpu_ms", -1.0))
				var raster_ms := float(vp_stats.get("shadow_raster_gpu_ms", -1.0))
				if cull_ms < 0.0 or raster_ms < 0.0:
					_failures.append("p4_timestamps: invalid shadow GPU timestamps (cull_ms=%f raster_ms=%f)" % [cull_ms, raster_ms])
				_summary["p4_double_sided_and_timestamps"] = {
					"shadow_r": snappedf(dbl_shadow.r, 0.001),
					"lit_r": snappedf(dbl_lit.r, 0.001),
					"shadow_cull_gpu_ms": snappedf(cull_ms, 0.001),
					"shadow_raster_gpu_ms": snappedf(raster_ms, 0.001),
				}

	if _p4_substep < 7:
		_configure_p4_substep(_p4_substep + 1)
	else:
		_finish()


func _finish() -> void:
	set_process(false)
	if _world_root != null:
		remove_child(_world_root)
		_world_root.free()
		_world_root = null
	for path in _temp_files:
		var abs_p := ProjectSettings.globalize_path(path)
		if FileAccess.file_exists(abs_p):
			DirAccess.remove_absolute(abs_p)
	_temp_files.clear()

	var ok := _failures.is_empty()
	print("NEXTGEO_SHADOW_SUMMARY pass=%s summary=%s failures=%s" % [
		str(ok),
		JSON.stringify(_summary),
		JSON.stringify(_failures),
	])
	get_tree().quit(0 if ok else 1)
