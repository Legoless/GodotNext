extends Node3D

## #183, #186: Lumen Lite with two views rendering at once. Each view integrates its own field in
## the frame block after its depth prepass, and reads the probes the scenario's WRC traced: the
## WRC updates once per frame, from the frame's first view, so every other view must read them on
## that view's grid. run_lite_two_views_check.sh runs both cases on one tier.
##
## --case=kitbash: the root view and a SubViewport, 12 m apart (inside WRC level 1 of each other,
## outside level 0), at yaw +30 and -30 degrees, over the kitbash (the PS smoke scene). After
## WARMUP_FRAMES, each view's denoised ambient (DEBUG_DRAW_NEXTGI_DENOISE_DENOISED, RB_TEX_AMBIENT)
## must be lit on at least MIN_LIT_FRACTION of its samples and not flat (MIN_SPREAD), Lite must
## record no gather or reflection dispatch, both views must pack their WRC probes every frame
## (lite_probe_traces = 2 x 512: the pack runs only while the WRC field is bound) and hold a field
## each (lite_view_fields 2). Then the SubViewport is freed: its field goes with it
## (lite_view_fields 1, lite_probe_traces 512).
##
## --case=rooms: two closed 6 x 3 x 6 m rooms share a 0.4 m wall, and an omni light (shadows on)
## lights only the west room (the lite_leak_check scene). One SubViewport per room looks down at
## its floor 0.5 m from the wall; the cameras are 1.4 m apart, so their WRC grids differ by a
## cell. --views=lit,dark or --views=dark,lit renders both, in that order; --views=lit or
## --views=dark renders one view alone. Each view's denoised ambient is averaged over the middle
## of its image (the floor patch) after the voxel-lighting volume has finished filling. Alone, it
## prints "LITE_TWO_VIEWS_SOLO view=<view> luma=<value>". With --lit-solo=L and --dark-solo=D
## (fresh processes of the solo runs) each view of a pair must be within ROOM_TOLERANCE x L of its
## own solo value. When the second view read the first view's grid (2e1b0f2e), the dark view read
## 0.87 of the lit room's solo value with the lit view first, and the lit view 0.077 of its own with
## the dark view first, on both tiers. The solo runs assert the scene separates the rooms:
## L >= MIN_ROOM_LIT and D <= MAX_DARK_RATIO x L.
##
## --expected-tier=HWRT, SWRT or device (HWRT when the rendering device reports
## SUPPORTS_RAY_QUERY_HARDWARE, else SWRT). Under debug/deny_ray_query (a device without ray
## query) the tier must also report NO_RAY_QUERY and no ray-query ShaderRD.
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes.

const KITBASH_SCENE := "res://scenes/swrt_kitbash/swrt_kitbash.tscn"
const SUB_VIEW_SIZE := Vector2i(960, 540)
const ROOM_VIEW_SIZE := Vector2i(960, 540)
const WARMUP_FRAMES := 240
const SETTLE_FRAMES := 30
const MAX_SETTLE_FRAMES := 5000
const VIEW_FRAMES := 12
const FREE_FRAMES := 12
const ROOM_MEASURE_FRAMES := 600
const ROOM_SAMPLES := 8
const ROOM_SAMPLE_STRIDE := 4
## Share of the kitbash samples with non-zero ambient (the sky has no depth and stays black).
const MIN_LIT_FRACTION := 0.3
## Share of the kitbash samples that differ from the first one: a uniform image is not a field.
const MIN_SPREAD := 0.05
const LIT_LUMA := 0.002
## A pair's view may differ from its solo value by this fraction of the lit room's solo value.
const ROOM_TOLERANCE := 0.1
const MIN_ROOM_LIT := 0.01
const MAX_DARK_RATIO := 0.05
const PROBES_PER_FRAME := 512
const WALL := 0.4
const FLOOR_OFFSET := 0.5

var case_name := ""
var expected_tier := ""
var room_views := PackedStringArray()
var lit_solo := -1.0
var dark_solo := -1.0
var deny_ray_query := false

var _views := {}
var _sub_view: SubViewport
var _environment: Environment
var _frames := 0
var _phase := 0
var _phase_frame := 0
var _settle := 0
var _samples := {}
var _summary := {}
var _failures := PackedStringArray()
var _done := false


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--case="):
			case_name = arg.trim_prefix("--case=")
		elif arg.begins_with("--expected-tier="):
			expected_tier = arg.trim_prefix("--expected-tier=")
		elif arg.begins_with("--views="):
			room_views = arg.trim_prefix("--views=").split(",", false)
		elif arg.begins_with("--lit-solo="):
			lit_solo = float(arg.trim_prefix("--lit-solo="))
		elif arg.begins_with("--dark-solo="):
			dark_solo = float(arg.trim_prefix("--dark-solo="))
	deny_ray_query = bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/debug/deny_ray_query", false))
	if expected_tier == "device":
		var device := RenderingServer.get_rendering_device()
		var hardware := device != null and device.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY_HARDWARE)
		expected_tier = "HWRT" if hardware and not deny_ray_query else "SWRT"
	if expected_tier != "HWRT" and expected_tier != "SWRT":
		_finish_with("--expected-tier must be HWRT, SWRT or device")
		return
	if case_name == "kitbash":
		_build_kitbash()
	elif case_name == "rooms":
		if room_views.is_empty() or room_views.size() > 2:
			_finish_with("--views must list one or two of lit and dark")
			return
		for view in room_views:
			if view != "lit" and view != "dark":
				_finish_with("--views must list one or two of lit and dark, got %s" % view)
				return
		if room_views.size() == 2 and (room_views[0] == room_views[1] or lit_solo < 0.0 or dark_solo < 0.0):
			_finish_with("a pair needs two different views and --lit-solo and --dark-solo")
			return
		_build_rooms()
	else:
		_finish_with("--case must be kitbash or rooms")


func _make_sub_view(p_size: Vector2i) -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = p_size
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.use_hdr_2d = true
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	viewport.use_taa = false
	viewport.debug_draw = Viewport.DEBUG_DRAW_NEXTGI_DENOISE_DENOISED
	add_child(viewport)
	return viewport


func _build_kitbash() -> void:
	var packed: PackedScene = load(KITBASH_SCENE)
	var root := packed.instantiate()
	add_child(root)
	# The scene's own script may replace the Environment in _ready, so take it afterwards.
	var world := root.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		_environment = world.environment.duplicate()
		_environment.nextgi_enabled = true
		world.environment = _environment
	# The SubViewport shares the root's World3D. SubViewports render before the root viewport,
	# so it is the frame's first view and the root view reads the probes on its grid.
	_sub_view = _make_sub_view(SUB_VIEW_SIZE)
	var sub_camera := Camera3D.new()
	sub_camera.fov = 70.0
	_sub_view.add_child(sub_camera)
	sub_camera.position = Vector3(-6.0, 2.5, 10.0)
	sub_camera.rotation_degrees = Vector3(-12.0, -30.0, 0.0)
	sub_camera.current = true
	var camera := Camera3D.new()
	camera.fov = 70.0
	add_child(camera)
	camera.position = Vector3(6.0, 2.5, 10.0)
	camera.rotation_degrees = Vector3(-12.0, 30.0, 0.0)
	camera.current = true
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGI_DENOISE_DENOISED
	_views["root"] = get_viewport()
	_views["sub"] = _sub_view


func _box(p_root: Node3D, p_center: Vector3, p_size: Vector3) -> void:
	var mesh := BoxMesh.new()
	mesh.size = p_size
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = p_center
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	p_root.add_child(instance)


func _build_rooms() -> void:
	# Created in render order: the first view renders first and updates the WRC.
	for view in room_views:
		var viewport := _make_sub_view(ROOM_VIEW_SIZE)
		var camera := Camera3D.new()
		camera.fov = 60.0
		viewport.add_child(camera)
		var side := -1.0 if view == "lit" else 1.0
		var target := Vector3(side * (WALL * 0.5 + FLOOR_OFFSET), 0.0, 0.0)
		camera.look_at_from_position(target + Vector3(0.0, 1.2, 0.0), target, Vector3(0.0, 0.0, -1.0))
		camera.current = true
		_views[view] = viewport
	# Both SubViewports share the root's World3D; the root viewport has no camera.
	var root := Node3D.new()
	add_child(root)
	_environment = Environment.new()
	_environment.background_mode = Environment.BG_COLOR
	_environment.background_color = Color.BLACK
	_environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	_environment.ambient_light_color = Color.BLACK
	_environment.ambient_light_energy = 0.0
	_environment.nextgi_enabled = true
	var world := WorldEnvironment.new()
	world.environment = _environment
	root.add_child(world)
	_box(root, Vector3(0, -0.1, 0), Vector3(12.4, 0.2, 6.4))
	_box(root, Vector3(0, 3.1, 0), Vector3(12.4, 0.2, 6.4))
	_box(root, Vector3(-6.1, 1.5, 0), Vector3(0.2, 3.0, 6.4))
	_box(root, Vector3(6.1, 1.5, 0), Vector3(0.2, 3.0, 6.4))
	_box(root, Vector3(0, 1.5, -3.1), Vector3(12.4, 3.0, 0.2))
	_box(root, Vector3(0, 1.5, 3.1), Vector3(12.4, 3.0, 0.2))
	_box(root, Vector3(0, 1.5, 0), Vector3(WALL, 3.0, 6.0))
	var light := OmniLight3D.new()
	light.position = Vector3(-2.0, 2.0, 0.0)
	light.omni_range = 8.0
	light.light_energy = 2.0
	light.shadow_enabled = true
	root.add_child(light)


# Share of non-zero samples, their mean luminance and the share that differs from the first.
func _field_stats(p_viewport: Viewport) -> Dictionary:
	var image := p_viewport.get_texture().get_image()
	var lit := 0
	var total := 0
	var luma_sum := 0.0
	var first := -1.0
	var differing := 0
	var step_x := maxi(image.get_width() / 64, 1)
	var step_y := maxi(image.get_height() / 36, 1)
	for y in range(step_y / 2, image.get_height(), step_y):
		for x in range(step_x / 2, image.get_width(), step_x):
			var luma := image.get_pixel(x, y).get_luminance()
			if first < 0.0:
				first = luma
			if absf(luma - first) > 0.01:
				differing += 1
			if luma > LIT_LUMA:
				lit += 1
				luma_sum += luma
			total += 1
	return {
		"lit_fraction": float(lit) / float(maxi(total, 1)),
		"mean_luma": luma_sum / float(maxi(lit, 1)),
		"spread_fraction": float(differing) / float(maxi(total, 1)),
	}


# The mean luminance over the middle quarter of the view (the floor patch).
func _patch_luma(p_viewport: Viewport) -> float:
	var image := p_viewport.get_texture().get_image()
	var sum := 0.0
	var count := 0
	for y in range(image.get_height() * 3 / 8, image.get_height() * 5 / 8, 4):
		for x in range(image.get_width() * 3 / 8, image.get_width() * 5 / 8, 4):
			sum += image.get_pixel(x, y).get_luminance()
			count += 1
	return sum / float(maxi(count, 1))


func _mean(p_values: PackedFloat32Array) -> float:
	var sum := 0.0
	for value in p_values:
		sum += value
	return sum / float(maxi(p_values.size(), 1))


func _check(p_condition: bool, p_message: String) -> void:
	if not p_condition:
		_failures.append(p_message)


# The tier, Lite and no screen or reflection rays, and the per-frame probe packs of p_views views.
func _check_lite(p_label: String, p_views: int) -> void:
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var counters: Dictionary = RenderingServer.nextgi_get_counters()
	var tier := str(info.get("tier", ""))
	var fields := int(info.get("lite_view_fields", -1))
	var traces := int(counters.get("lite_probe_traces", -1))
	print("LITE_TWO_VIEWS %s tier=%s reason=%s lite=%s lite_view_fields=%d lite_probe_traces=%d gather=%d reflection=%d" % [p_label, tier, str(info.get("reason", "")), str(info.get("lite_beta", "")), fields, traces, int(counters.get("gather_dispatches", -1)), int(counters.get("reflection_dispatches", -1))])
	_summary[p_label] = {"tier": tier, "lite_view_fields": fields, "lite_probe_traces": traces}
	_check(tier == expected_tier, "%s: tier is %s, expected %s" % [p_label, tier, expected_tier])
	_check(bool(info.get("lite_beta", false)), "%s: Lumen Lite is not active (quality %s)" % [p_label, str(info.get("quality", ""))])
	_check(int(counters.get("gather_dispatches", -1)) == 0 and int(counters.get("reflection_dispatches", -1)) == 0, "%s: Lite dispatched screen-probe or reflection rays" % p_label)
	_check(fields == p_views, "%s: %d views hold a Lite field, expected %d" % [p_label, fields, p_views])
	_check(traces == p_views * PROBES_PER_FRAME, "%s: %d Lite probes packed in the last frame, expected %d (%d per view, packed only while the WRC field is bound)" % [p_label, traces, p_views * PROBES_PER_FRAME, PROBES_PER_FRAME])
	if deny_ray_query:
		_check(str(info.get("reason", "")) == "NO_RAY_QUERY", "%s: deny_ray_query: the reason is %s, expected NO_RAY_QUERY" % [p_label, str(info.get("reason", ""))])
		_check(int(info.get("ray_query_shader_versions", -1)) == 0, "%s: deny_ray_query: %d ray-query ShaderRD versions were created" % [p_label, int(info.get("ray_query_shader_versions", -1))])


func _next_phase() -> void:
	_phase += 1
	_phase_frame = 0


func _process(_delta: float) -> void:
	if _done:
		return
	_frames += 1
	_phase_frame += 1
	if case_name == "kitbash":
		_process_kitbash()
	else:
		_process_rooms()


func _process_kitbash() -> void:
	match _phase:
		0:
			if _phase_frame < WARMUP_FRAMES:
				return
			_check_lite("two_views", 2)
			for key in ["root", "sub"]:
				var stats := _field_stats(_views[key])
				_summary["ambient_" + key] = stats
				print("LITE_TWO_VIEWS ambient view=%s lit=%.3f luma=%.4f spread=%.3f" % [key, stats["lit_fraction"], stats["mean_luma"], stats["spread_fraction"]])
				_check(float(stats["lit_fraction"]) >= MIN_LIT_FRACTION, "the %s view's ambient is lit on %.3f of its samples, below %.2f" % [key, stats["lit_fraction"], MIN_LIT_FRACTION])
				_check(float(stats["spread_fraction"]) >= MIN_SPREAD, "the %s view's ambient is flat (spread %.3f, below %.2f)" % [key, stats["spread_fraction"], MIN_SPREAD])
			_sub_view.queue_free()
			_sub_view = null
			_views.erase("sub")
			_next_phase()
		1:
			if _phase_frame < FREE_FRAMES:
				return
			_check_lite("one_view", 1)
			_finish()


func _process_rooms() -> void:
	match _phase:
		0:
			# The rooms' light reaches the field through the voxel-lighting volume; wait for it.
			if _frames < WARMUP_FRAMES:
				return
			var retrace := int(RenderingServer.nextgi_get_counters().get("voxel_lighting_retrace", -1))
			_settle = _settle + 1 if retrace == 0 else 0
			if _settle >= SETTLE_FRAMES:
				_next_phase()
			elif _frames >= MAX_SETTLE_FRAMES:
				_finish_with("voxel lighting was still filling after %d frames" % _frames)
		1:
			if _phase_frame <= ROOM_MEASURE_FRAMES:
				return
			if _phase_frame == ROOM_MEASURE_FRAMES + 1:
				_check_lite("rooms", room_views.size())
			if (_phase_frame - ROOM_MEASURE_FRAMES) % ROOM_SAMPLE_STRIDE != 0:
				return
			for view in room_views:
				if not _samples.has(view):
					_samples[view] = PackedFloat32Array()
				var values: PackedFloat32Array = _samples[view]
				values.append(_patch_luma(_views[view]))
				_samples[view] = values
			var taken: PackedFloat32Array = _samples[room_views[0]]
			if taken.size() >= ROOM_SAMPLES:
				_room_checks()
				_finish()


func _room_checks() -> void:
	var means := {}
	for view in room_views:
		means[view] = _mean(_samples[view])
	_summary["room_luma"] = means
	if room_views.size() == 1:
		var view: String = room_views[0]
		print("LITE_TWO_VIEWS_SOLO view=%s luma=%.6f frames=%d" % [view, float(means[view]), _frames])
		return
	_check(lit_solo >= MIN_ROOM_LIT, "the lit room's solo ambient (%.6f) is below %.3f: the comparison would see nothing" % [lit_solo, MIN_ROOM_LIT])
	_check(dark_solo <= MAX_DARK_RATIO * lit_solo, "the dark room's solo ambient (%.6f) is above %.2f of the lit room's (%.6f): the rooms are not separate" % [dark_solo, MAX_DARK_RATIO, lit_solo])
	var tolerance := ROOM_TOLERANCE * lit_solo
	var solo := {"lit": lit_solo, "dark": dark_solo}
	var line := "LITE_TWO_VIEWS_PAIR order=%s tolerance=%.6f" % [",".join(room_views), tolerance]
	for view in room_views:
		var difference := absf(float(means[view]) - float(solo[view]))
		line += " %s=%.6f %s_solo=%.6f" % [view, float(means[view]), view, float(solo[view])]
		_check(difference <= tolerance, "with the %s view rendered first, the %s view's ambient is %.6f, %.6f from its solo %.6f (tolerance %.6f, %.0f%% of the lit room's solo)" % [room_views[0], view, float(means[view]), difference, float(solo[view]), tolerance, ROOM_TOLERANCE * 100.0])
	print(line + " frames=%d" % _frames)


func _finish_with(p_failure: String) -> void:
	_failures.append(p_failure)
	_finish()


func _finish() -> void:
	_done = true
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "lite_two_views",
		"case": case_name,
		"views": room_views,
		"pass": _failures.is_empty(),
		"expected_tier": expected_tier,
		"tier": info.get("tier", ""),
		"quality": info.get("quality", ""),
		"summary": _summary,
		"failures": _failures,
	}))
	get_tree().quit(0 if _failures.is_empty() else 1)
