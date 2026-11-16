extends Node3D

## Virtual Shadow Map page marking and allocation on the GPU (#219; plan §21.3 contract S3).
##
## A lit set with every light kind casting shadows: a sun (100 m max distance, 7 clipmap levels),
## a spot, an area light and a ring of omni lights, over a floor with boxes, a transparent box and
## volumetric fog. The camera visits POSES fixed poses. At each one it captures two views: the
## first after the move (new pages, scrolled clipmap pages, stolen cached pages: the dirty list is
## full) and one after SETTLE_FRAMES frames (cached pages kept). Then it holds the first pose until
## pages unrequested for the cache frames are dropped (HOLD_CAPTURES consecutive views), and turns the
## shadows of the sun and one omni light off (their maps go, their pages are orphaned, and their maps
## must keep no overflow bias for the next light that gets them). For each capture,
## RenderingServer.virtual_shadow_maps_request_check() arms the next view and
## virtual_shadow_maps_get_check_result() compares it with the CPU models:
## - marking: the pages the GPU flagged requested (and coarse) against the CPU reference over every
##   pixel, fog froxel vertex and transparent box. "exact" is a 100% match; any other difference
##   must be explained by float rounding (a page, mip, face or light edge within 1e-5 of the input);
## - the request list, the allocation counters against the CPU model, the pages handed out by
##   free-list bucket against the model (free pages first, then cached pages least recently used
##   first), the page table and pool header invariants after allocation (no page mapped twice, no
##   dangling entry, every request served or flagged OVERFLOW), the dirty-page list and its indirect
##   arguments, the page clear (the pool is filled with a non-zero value before allocation: the
##   layers each dirty page names must read 0 after the clear pass, every other texel the fill
##   value), the overflow bias, and sampling (every pixel request must find a mapped page, at its
##   requested mip or at an always-resident coarse fallback page). Pose 7 turns volumetric fog and
##   the transparent box off so that only the always-resident pass marks coarse pages there.
## Across the captures, pages must be newly allocated, put in the dirty list (directional ones too),
## cleared and orphaned; with the default pool, dropped as well.
## With --expect-overflow=true (run it with GODOT_DEV_VSM=25%, a quarter of the default pool) the
## pool must overflow (the OVERFLOW counter is non-zero), some map must take an overflow bias,
## overflowed pixel requests must fall back to coarse pages with 0 sampled-but-unmapped pages, and
## every invariant must still hold: no corrupted page.
## With --expect-active=false (GODOT_DEV_VSM unset) VSM must mark nothing.
##
## It prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.
## User arguments: --expect-active=true|false (default: GODOT_DEV_VSM is set),
## --expect-overflow=true|false (default false).

const SceneArgs = preload("res://scenes/scene_args.gd")

const SETTLE_FRAMES := 4
const RESULT_WAIT_FRAMES := 8
const HOLD_CAPTURES := 6
const HOLD_LEAD := 3 # Hold captures start this many frames before the cache frames run out.
const OMNI_RING := 8
# Camera position, then the point it looks at.
const POSES: Array[Array] = [
	[Vector3(0.0, 3.0, 9.0), Vector3(0.0, 0.5, 0.0)],
	[Vector3(7.0, 1.6, 6.0), Vector3(-1.0, 0.8, -1.0)],
	[Vector3(-8.0, 6.0, -2.0), Vector3(1.0, 0.0, 0.0)],
	[Vector3(0.5, 12.0, 0.5), Vector3(0.0, 0.0, 0.0)],
	[Vector3(-2.0, 0.6, 4.0), Vector3(3.0, 1.2, -6.0)],
	[Vector3(3.0, 2.0, -9.0), Vector3(0.0, 1.0, 2.0)],
	[Vector3(25.0, 8.0, 25.0), Vector3(0.0, 0.0, 0.0)],
	[Vector3(-0.3, 1.2, 1.4), Vector3(-0.3, 0.6, -1.0)],
]

var _expect_active := false
var _expect_overflow := false
var _frame := 0
var _pose := 0
var _pose_frame := 0
var _step := "poses"
var _step_frame := 0
var _hold_start := 0
var _armed_frame := -1
var _armed_kind := ""
var _captures := 0
var _camera: Camera3D
var _environment: Environment
var _glass: MeshInstance3D
var _far_glass: MeshInstance3D
var _sun: DirectionalLight3D
var _omni: Array[OmniLight3D] = []
var _totals := {"new": 0, "dirty": 0, "dirty_directional": 0, "evicted": 0, "orphaned": 0, "stolen": 0, "kept": 0, "handed_out_cached": 0}
var _cleared_pages := 0
var _clear_checks := 0
var _sampled_pages := 0
var _sampled_fallback := 0
var _sampled_unmapped := 0
var _coarse_sampled := 0
var _coarse_receiver_unmapped := 0
var _coarse_unmapped := 0
var _without_coarse_unmapped := 0
var _failures := PackedStringArray()
var _result := {}
var _poses: Array = []
var _exact_captures := 0
var _max_biased := 0
var _max_overflow := 0


func _ready() -> void:
	_expect_active = SceneArgs.value("--expect-active", "true" if OS.get_environment("GODOT_DEV_VSM") != "" else "false") == "true"
	_expect_overflow = SceneArgs.value("--expect-overflow", "false") == "true"
	_result["expect_active"] = _expect_active
	_result["expect_overflow"] = _expect_overflow
	_result["driver"] = RenderingServer.get_current_rendering_driver_name()
	_result["device"] = RenderingServer.get_video_adapter_name()
	_build_scene()


func _build_scene() -> void:
	_environment = Environment.new()
	_environment.background_mode = Environment.BG_COLOR
	_environment.background_color = Color(0.2, 0.25, 0.3)
	_environment.volumetric_fog_enabled = true
	_environment.volumetric_fog_density = 0.02
	_environment.volumetric_fog_length = 40.0
	var world := WorldEnvironment.new()
	world.environment = _environment
	add_child(world)

	var floor_mesh := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(80.0, 80.0)
	floor_mesh.mesh = plane
	add_child(floor_mesh)
	var rng := RandomNumberGenerator.new()
	rng.seed = 219
	for i in 24:
		var box := MeshInstance3D.new()
		var mesh := BoxMesh.new()
		mesh.size = Vector3(rng.randf_range(0.3, 2.0), rng.randf_range(0.3, 3.0), rng.randf_range(0.3, 2.0))
		box.mesh = mesh
		box.position = Vector3(rng.randf_range(-12.0, 12.0), mesh.size.y * 0.5, rng.randf_range(-12.0, 12.0))
		box.rotation.y = rng.randf_range(0.0, TAU)
		add_child(box)
	# A transparent box: its world box requests coarse pages.
	_glass = MeshInstance3D.new()
	var glass_mesh := BoxMesh.new()
	glass_mesh.size = Vector3(1.5, 2.0, 0.2)
	_glass.mesh = glass_mesh
	var glass_material := StandardMaterial3D.new()
	glass_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	glass_material.albedo_color = Color(0.6, 0.8, 1.0, 0.35)
	_glass.material_override = glass_material
	_glass.position = Vector3(-1.5, 1.0, 1.5)
	add_child(_glass)

	_camera = Camera3D.new()
	add_child(_camera)
	_camera.current = true

	_sun = DirectionalLight3D.new()
	_sun.shadow_enabled = true
	_sun.rotation_degrees = Vector3(-55.0, 35.0, 0.0)
	add_child(_sun)
	# A second transparent box in the sun's coarsest clipmap level's far strip against the sky (no opaque
	# pixel behind it): only MODE_BOX's far-strip coarse request covers it.
	_far_glass = MeshInstance3D.new()
	var far_glass_mesh := BoxMesh.new()
	far_glass_mesh.size = Vector3(1.0, 1.0, 1.0)
	_far_glass.mesh = far_glass_mesh
	_far_glass.material_override = glass_material
	_far_glass.position = _sun.transform.basis.x.normalized() * 114.0 + Vector3(0.0, 12.0, 0.0)
	add_child(_far_glass)
	var spot := SpotLight3D.new()
	spot.shadow_enabled = true
	spot.position = Vector3(-3.0, 5.0, -2.0)
	spot.rotation_degrees = Vector3(-70.0, 20.0, 0.0)
	spot.spot_range = 14.0
	spot.spot_angle = 35.0
	add_child(spot)
	var area := AreaLight3D.new()
	area.shadow_enabled = true
	area.position = Vector3(2.0, 3.5, -3.0)
	area.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	add_child(area)
	for i in OMNI_RING:
		var omni := OmniLight3D.new()
		omni.shadow_enabled = true
		var angle := TAU * float(i) / float(OMNI_RING)
		omni.position = Vector3(cos(angle) * 7.0, 2.0 + 0.5 * float(i % 3), sin(angle) * 7.0)
		omni.omni_range = 11.0
		add_child(omni)
		_omni.append(omni)


func _process(_delta: float) -> void:
	_frame += 1
	if not _expect_active:
		if _frame >= SETTLE_FRAMES * 3:
			_check_inactive()
			_finish()
		return
	# A capture in flight: read it once the armed view has rendered.
	if _armed_frame >= 0:
		var check := RenderingServer.virtual_shadow_maps_get_check_result()
		if bool(check.get("captured", false)) or check.has("failures"):
			_record(check, _armed_kind)
			_armed_frame = -1
		elif _frame - _armed_frame > RESULT_WAIT_FRAMES:
			_failures.append("%s: no view captured the check in %d frames" % [_armed_kind, RESULT_WAIT_FRAMES])
			_armed_frame = -1
		else:
			return
	_step_frame += 1
	match _step:
		"poses":
			_pose_frame += 1
			if _pose_frame == 1:
				var fog_and_alpha := _pose + 1 < POSES.size()
				_environment.volumetric_fog_enabled = fog_and_alpha
				_glass.visible = fog_and_alpha
				_far_glass.visible = fog_and_alpha
				_camera.position = POSES[_pose][0]
				_camera.look_at(POSES[_pose][1])
				_arm("pose %d move" % _pose)
			elif _pose_frame == SETTLE_FRAMES:
				_arm("pose %d steady" % _pose)
			elif _pose_frame > SETTLE_FRAMES:
				_pose += 1
				_pose_frame = 0
				if _pose >= POSES.size():
					_environment.volumetric_fog_enabled = true
					_glass.visible = true
					_far_glass.visible = true
					_camera.position = POSES[0][0]
					_camera.look_at(POSES[0][1])
					_next("hold")
		"hold":
			# Pages last requested at the other poses pass the cache frames around now.
			var cache_frames := int(RenderingServer.virtual_shadow_maps_get_debug_info().get("cache_frames", 60))
			if _step_frame >= cache_frames - HOLD_LEAD:
				if _hold_start == 0:
					_hold_start = _captures
				if _captures - _hold_start >= HOLD_CAPTURES:
					_next("orphan")
				else:
					_arm("hold %d" % (_captures - _hold_start))
		"orphan":
			if _step_frame == 1:
				_omni[0].shadow_enabled = false
				_sun.shadow_enabled = false
				_arm("orphan")
			else:
				_check_end()
				_finish()


func _arm(p_kind: String) -> void:
	RenderingServer.virtual_shadow_maps_request_check()
	_armed_frame = _frame
	_armed_kind = p_kind


func _next(p_step: String) -> void:
	_step = p_step
	_step_frame = 0


func _record(p_check: Dictionary, p_kind: String) -> void:
	var mark: Dictionary = p_check.get("mark", {})
	var allocation: Dictionary = p_check.get("allocation", {})
	var invariants: Dictionary = p_check.get("invariants", {})
	var dirty: Dictionary = p_check.get("dirty", {})
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	_captures += 1
	var pose := {
		"capture": p_kind,
		"pass": bool(p_check.get("pass", false)),
		"size": p_check.get("size", Vector2i()),
		"mark": mark,
		"request_list": p_check.get("request_list", {}),
		"allocation": allocation,
		"invariants": invariants,
		"dirty": dirty,
		"bias": p_check.get("bias", {}),
		"clear": p_check.get("clear", {}),
		"resident": p_check.get("resident", {}),
		"sample": p_check.get("sample", {}),
	}
	_poses.append(pose)
	for failure in p_check.get("failures", PackedStringArray()):
		_failures.append("%s: %s" % [p_kind, failure])
	if bool(mark.get("exact", false)):
		_exact_captures += 1
	if int(mark.get("gpu_requested", 0)) == 0:
		_failures.append("%s: the view requested no page" % p_kind)
	for key in _totals:
		_totals[key] = int(_totals[key]) + int(allocation.get(key, 0))
	var clear: Dictionary = p_check.get("clear", {})
	if bool(clear.get("checked", false)):
		_clear_checks += 1
		_cleared_pages += int(clear.get("cleared_pages", 0))
	var sample: Dictionary = p_check.get("sample", {})
	_sampled_pages += int(sample.get("sampled", 0))
	_sampled_fallback += int(sample.get("fallback", 0))
	_sampled_unmapped += int(sample.get("unmapped", 0))
	_coarse_sampled += int(sample.get("coarse_sampled", 0))
	_coarse_receiver_unmapped += int(sample.get("coarse_receiver_unmapped", 0))
	_coarse_unmapped += int(sample.get("coarse_unmapped", 0))
	_without_coarse_unmapped += int(sample.get("without_coarse_unmapped", 0))
	var bias: Dictionary = p_check.get("bias", {})
	_max_biased = maxi(_max_biased, int(bias.get("biased_maps", 0)))
	var pages: Dictionary = info.get("pages", {})
	_max_overflow = maxi(_max_overflow, int(pages.get("total_overflow", 0)))


func _check_inactive() -> void:
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	_result["info"] = info
	if bool(info.get("active", true)):
		_failures.append("GODOT_DEV_VSM is unset but virtual shadow maps are active")
	if int(info.get("marked_views", -1)) != 0:
		_failures.append("VSM is off but %d views marked pages" % int(info.get("marked_views", -1)))


func _check_end() -> void:
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	var pages: Dictionary = info.get("pages", {})
	_result["info"] = info
	_result["captures"] = _captures
	_result["exact_captures"] = _exact_captures
	_result["poses"] = _poses
	_result["totals"] = _totals
	if _captures != POSES.size() * 2 + HOLD_CAPTURES + 1:
		_failures.append("%d captures, expected %d" % [_captures, POSES.size() * 2 + HOLD_CAPTURES + 1])
	# Cached pages are handed out again (testing the free-list order) only once the free pages run out.
	# Under D3D12 GPU-based validation the light cluster is empty (#277) and they never do, so the count
	# is reported, not required.
	_result["handed_out_cached"] = int(_totals["handed_out_cached"])
	for key in ["new", "dirty", "dirty_directional", "orphaned"]:
		if int(_totals[key]) == 0:
			_failures.append("no capture saw %s pages" % key)
	_result["clear_checks"] = _clear_checks
	_result["cleared_pages"] = _cleared_pages
	if _clear_checks != _captures or _cleared_pages == 0:
		_failures.append("the page clear was checked in %d of %d captures, %d pages cleared" % [_clear_checks, _captures, _cleared_pages])
	if not _expect_overflow and int(_totals["evicted"]) == 0:
		_failures.append("no capture saw a page dropped after the cache frames")
	_result["max_biased_maps"] = _max_biased
	_result["total_overflow"] = int(pages.get("total_overflow", 0))
	_result["overflow_views"] = int(pages.get("overflow_views", 0))
	_result["total_resident_overflow"] = int(pages.get("total_resident_overflow", 0))
	_result["sampled_pages"] = _sampled_pages
	_result["sampled_fallback"] = _sampled_fallback
	_result["sampled_unmapped"] = _sampled_unmapped
	_result["coarse_sampled"] = _coarse_sampled
	_result["coarse_receiver_unmapped"] = _coarse_receiver_unmapped
	_result["coarse_unmapped"] = _coarse_unmapped
	_result["without_coarse_unmapped"] = _without_coarse_unmapped
	if _sampled_pages == 0:
		_failures.append("no pixel shadow request was sampled")
	if _sampled_unmapped != 0:
		_failures.append("%d of %d sampled pixel requests found no mapped page" % [_sampled_unmapped, _sampled_pages])
	if _coarse_sampled == 0 or _coarse_receiver_unmapped != 0:
		_failures.append("%d of %d coarse froxel/box lookups found no mapped page" % [_coarse_receiver_unmapped, _coarse_sampled])
	if _coarse_unmapped != 0 or int(pages.get("total_resident_overflow", 0)) != 0:
		_failures.append("%d coarse and %d resident requests went without a page" % [_coarse_unmapped, int(pages.get("total_resident_overflow", 0))])
	if _expect_overflow:
		if int(pages.get("total_overflow", 0)) == 0:
			_failures.append("the pool of %d pages never overflowed" % int(info.get("pool_pages", 0)))
		if _max_biased == 0:
			_failures.append("no map took an overflow bias")
		if _sampled_fallback == 0 or _without_coarse_unmapped == 0:
			_failures.append("overflowed views saw %d fallback samples and %d unmapped without coarse pages" % [_sampled_fallback, _without_coarse_unmapped])
	if int(pages.get("total_dropped", 0)) != 0:
		_failures.append("%d requests found the request list full" % int(pages.get("total_dropped", 0)))


func _finish() -> void:
	_result["failures"] = _failures
	_result["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
