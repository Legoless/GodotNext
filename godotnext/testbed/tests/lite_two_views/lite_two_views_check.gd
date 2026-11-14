extends Node3D

## #183, #186, #253: Lumen Lite with two views rendering at once. Each view integrates its own field
## in the frame block after its depth prepass, and reads the probes its own WRC traced: each view
## keeps a WRC in its render buffers (#253), on a grid that follows its own camera.
## run_lite_two_views_check.sh runs the cases on one tier.
##
## --case=kitbash: the root view and a SubViewport, 12 m apart (inside WRC level 1 of each other,
## outside level 0), at yaw +30 and -30 degrees, over the kitbash (the PS smoke scene). After
## WARMUP_FRAMES, each view's denoised ambient (DEBUG_DRAW_NEXTGI_DENOISE_DENOISED, RB_TEX_AMBIENT)
## must be lit on at least MIN_LIT_FRACTION of its samples and not flat (MIN_SPREAD), Lite must
## record no gather or reflection dispatch, both views must pack their WRC probes every frame
## (lite_probe_traces = 2 x 512: the pack runs only while the WRC field is bound) and hold a field
## each (lite_view_fields 2). Then the SubViewport is freed: its field goes with it
## (lite_view_fields 1, lite_probe_traces 512). Last, two views are added at the root camera's
## position: a SubViewport that renders every frame and a one-shot SubViewport (UPDATE_ONCE).
## Their first frame packs every probe of their new fields (lite_probe_traces 512 + 2 x 8192,
## lite_view_fields 3), and the image of that frame, the one-shot view's only one, must pass the
## same lit and spread bounds. The frame after, the one-shot view does not render (1024). When a
## new field packed only its first 512 probes (4448434a), that frame was lit on 0.07 and 0.05 of
## the samples, against 0.70 and 0.69 with one field for every view (2e1b0f2e) and with the fix.
##
## --case=rooms: two closed 6 x 3 x 6 m rooms share a 0.4 m wall, and an omni light (shadows on)
## lights only the west room (the lite_leak_check scene). One SubViewport per room looks down at
## its floor 0.5 m from the wall; the cameras are 1.4 m apart, so their WRC grids differ by a
## cell. --views=lit,dark or --views=dark,lit renders both, in that order; --views=lit or
## --views=dark renders one view alone. Each view's denoised ambient is averaged over the middle
## of its image (the floor patch) after the voxel-lighting volume has finished filling. Alone, it
## prints "LITE_TWO_VIEWS_SOLO view=<view> luma=<value>". With --lit-solo=L and --dark-solo=D
## (fresh processes of the solo runs) each view of a pair must be within ROOM_TOLERANCE x L of its
## own solo value. When the second view snapped the probe grid to its own camera (2e1b0f2e), the
## dark view read 0.87 of the lit room's solo value with the lit view first, and the lit view 0.077
## of its own with the dark view first, on both tiers. The solo runs assert the scene separates the rooms:
## L >= MIN_ROOM_LIT and D <= MAX_DARK_RATIO x L.
##
## --case=handoff (#254): the same rooms with one view, over the lit room. After the rooms settle,
## its patch is sampled and its camera moves to the dark room's camera position; its WRC's grid
## snaps a cell away. Within HANDOFF_FRAMES the patch must read at most MAX_DARK_RATIO of the lit
## one: the moved levels re-trace without the history their cells held at the old position. Before
## #254, when a second view over the dark room took the grid over, the dark room still read 0.26
## of the lit room 400 frames later; since #253 that second view keeps its own WRC, so the move
## comes from one camera.
##
## --case=far (#253): two views over the kitbash, FAR_DISTANCE apart: one where the kitbash case's
## root view stands, one far west of it looking back. --views=near or --views=far renders one alone
## and prints "LITE_TWO_VIEWS_FAR_SOLO view=<view> lit=<value>", the view's lit share of its
## denoised ambient averaged over FAR_SAMPLES frames (at least MIN_FAR_LIT). --views=near,far or
## far,near renders both with --near-solo and --far-solo, and each must stay within FAR_TOLERANCE
## of its solo value; the tier info must count one WRC per view (wrc_views). With one WRC for the
## scenario, the far view's grid left the near view's surroundings outside the probes.
##
## --case=switch (#256): the kitbash with its two views at Medium (Lite), then the project's
## quality switched to High: after SWITCH_FRAMES no view holds a Lite field (lite_view_fields 0).
## Back at Medium, both views hold a field again and the frame after the switch is lit and not
## flat, as the kitbash bounds require, because a new field packs every probe on its first frame.
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
## Frames after the camera move before the dark room is sampled: one fresh WRC cycle (32 frames) and
## the Lite fields' packing after it.
const HANDOFF_FRAMES := 64
## Frames a quality switch is given before lite_view_fields is read (#256).
const SWITCH_FRAMES := 8
const QUALITY_SETTING := "rendering/global_illumination/nextgi/quality"
## #253: the far case's views, 40 m apart over the kitbash, and how far a pair's view may move from
## its solo value (a fraction of that value).
const FAR_VIEW_SIZE := Vector2i(640, 360)
const FAR_DISTANCE := 40.0
const FAR_TOLERANCE := 0.1
const FAR_SAMPLES := 30
const MIN_FAR_LIT := 0.2
const PROBES_PER_FRAME := 512
const PROBE_COUNT := 8192
const WALL := 0.4
const FLOOR_OFFSET := 0.5

var case_name := ""
var expected_tier := ""
var room_views := PackedStringArray()
var lit_solo := -1.0
var dark_solo := -1.0
var near_solo := -1.0
var far_solo := -1.0
var _far_lit := {}
var deny_ray_query := false

var _views := {}
var _sub_view: SubViewport
var _camera_position := Vector3.ZERO
var _camera_rotation := Vector3.ZERO
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
		elif arg.begins_with("--near-solo="):
			near_solo = float(arg.trim_prefix("--near-solo="))
		elif arg.begins_with("--far-solo="):
			far_solo = float(arg.trim_prefix("--far-solo="))
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
	elif case_name == "switch":
		_build_kitbash()
	elif case_name == "handoff":
		room_views = PackedStringArray(["lit"])
		_build_rooms()
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
	elif case_name == "far":
		if room_views.is_empty() or room_views.size() > 2:
			_finish_with("--views must list one or two of near and far")
			return
		for view in room_views:
			if view != "near" and view != "far":
				_finish_with("--views must list one or two of near and far, got %s" % view)
				return
		if room_views.size() == 2 and (room_views[0] == room_views[1] or near_solo < 0.0 or far_solo < 0.0):
			_finish_with("a pair needs two different views and --near-solo and --far-solo")
			return
		_build_far()
	else:
		_finish_with("--case must be kitbash, rooms, handoff, switch or far")


func _make_sub_view(p_size: Vector2i, p_update_mode := SubViewport.UPDATE_ALWAYS) -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = p_size
	viewport.render_target_update_mode = p_update_mode
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
	# The SubViewport shares the root's World3D; each view keeps its own WRC (#253).
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
	_camera_position = camera.position
	_camera_rotation = camera.rotation_degrees
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGI_DENOISE_DENOISED
	_views["root"] = get_viewport()
	_views["sub"] = _sub_view


# #253: the near view where the kitbash case's root view stands, the far one FAR_DISTANCE west of it
# looking back at the kitbash. Each is a SubViewport; the root viewport renders no 3D (the kitbash
# scene brings a camera), so the only views are the ones under test.
func _build_far() -> void:
	get_viewport().disable_3d = true
	var packed: PackedScene = load(KITBASH_SCENE)
	var root := packed.instantiate()
	add_child(root)
	var world := root.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		_environment = world.environment.duplicate()
		_environment.nextgi_enabled = true
		world.environment = _environment
	for view in room_views:
		var viewport := _make_sub_view(FAR_VIEW_SIZE)
		var camera := Camera3D.new()
		camera.fov = 70.0
		viewport.add_child(camera)
		if view == "near":
			camera.position = Vector3(6.0, 2.5, 10.0)
			camera.rotation_degrees = Vector3(-12.0, 30.0, 0.0)
		else:
			camera.position = Vector3(6.0 - FAR_DISTANCE, 4.0, 10.0)
			camera.look_at(Vector3(0.0, 1.0, 0.0))
		camera.current = true
		_views[view] = viewport
		_far_lit[view] = PackedFloat32Array()


# Each view's lit share of its denoised ambient, averaged over FAR_SAMPLES frames after the warmup.
# Alone, it prints its value; in a pair, each view must stay within FAR_TOLERANCE of its solo value.
func _process_far() -> void:
	if _frames <= WARMUP_FRAMES:
		return
	# Packed arrays are values: append to a copy and store it back.
	for view in room_views:
		var values: PackedFloat32Array = _far_lit[view]
		values.append(float(_field_stats(_views[view])["lit_fraction"]))
		_far_lit[view] = values
	if (_far_lit[room_views[0]] as PackedFloat32Array).size() < FAR_SAMPLES:
		return
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	_check(String(info.get("tier", "")) == expected_tier, "tier is %s, expected %s" % [String(info.get("tier", "")), expected_tier])
	_summary["wrc_views"] = int(info.get("wrc_views", -1))
	_check(int(info.get("wrc_views", -1)) == room_views.size(), "%d views keep %d WRCs, expected one each" % [room_views.size(), int(info.get("wrc_views", -1))])
	for view in room_views:
		var lit := _mean(_far_lit[view])
		_summary[view] = lit
		if room_views.size() == 1:
			print("LITE_TWO_VIEWS_FAR_SOLO view=%s lit=%.6f quality=%s" % [view, lit, String(info.get("quality", ""))])
			_check(lit >= MIN_FAR_LIT, "the %s view alone is lit on %.3f of its samples, below %.2f: the comparison would see nothing" % [view, lit, MIN_FAR_LIT])
		else:
			var solo := near_solo if view == "near" else far_solo
			print("LITE_TWO_VIEWS_FAR_PAIR view=%s lit=%.6f solo=%.6f order=%s" % [view, lit, solo, ",".join(room_views)])
			_check(absf(lit - solo) <= FAR_TOLERANCE * solo, "the %s view is lit on %.3f of its samples beside the other view, against %.3f alone (more than %.0f%% apart)" % [view, lit, solo, FAR_TOLERANCE * 100.0])
	_finish()


func _box(p_root: Node3D, p_center: Vector3, p_size: Vector3) -> void:
	var mesh := BoxMesh.new()
	mesh.size = p_size
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = p_center
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	p_root.add_child(instance)


func _build_rooms() -> void:
	# Created in render order.
	for view in room_views:
		var viewport := _make_sub_view(ROOM_VIEW_SIZE)
		var camera := Camera3D.new()
		camera.fov = 60.0
		viewport.add_child(camera)
		_place_room_camera(camera, view)
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


# The tier, Lite and no screen or reflection rays, the fields of p_views views, and the probes
# packed in the last frame: p_traces, or PROBES_PER_FRAME per view when it is negative.
func _check_lite(p_label: String, p_views: int, p_traces := -1) -> void:
	var expected_traces := p_traces if p_traces >= 0 else p_views * PROBES_PER_FRAME
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
	_check(traces == expected_traces, "%s: %d Lite probes packed in the last frame, expected %d (%d per view a frame, all %d on a new field's first frame, packed only while the WRC field is bound)" % [p_label, traces, expected_traces, PROBES_PER_FRAME, PROBE_COUNT])
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
	elif case_name == "switch":
		_process_switch()
	elif case_name == "far":
		_process_far()
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
			# At the root camera's position: the new views render before the root view, so the WRC
			# updates from the first one's camera that frame, and the same position keeps its grid.
			_views["new"] = _make_kitbash_view(SubViewport.UPDATE_ALWAYS, -15.0)
			_views["one_shot"] = _make_kitbash_view(SubViewport.UPDATE_ONCE, -30.0)
			_next_phase()
		2:
			# The first _process after the new views were added reads the frame they first drew.
			if _phase_frame == 1:
				_check_lite("new_views_first_frame", 3, PROBES_PER_FRAME + 2 * PROBE_COUNT)
				for key in ["new", "one_shot"]:
					var stats := _field_stats(_views[key])
					_summary["ambient_first_frame_" + key] = stats
					print("LITE_TWO_VIEWS ambient_first_frame view=%s lit=%.3f luma=%.4f spread=%.3f" % [key, stats["lit_fraction"], stats["mean_luma"], stats["spread_fraction"]])
					_check(float(stats["lit_fraction"]) >= MIN_LIT_FRACTION, "the %s view's first frame is lit on %.3f of its samples, below %.2f: its first pack did not fill its field" % [key, stats["lit_fraction"], MIN_LIT_FRACTION])
					_check(float(stats["spread_fraction"]) >= MIN_SPREAD, "the %s view's first frame is flat (spread %.3f, below %.2f)" % [key, stats["spread_fraction"], MIN_SPREAD])
				return
			# The one-shot view keeps its field but no longer renders.
			_check_lite("new_views_next_frame", 3, 2 * PROBES_PER_FRAME)
			_finish()


# A SubViewport at the root camera's position, turned p_yaw degrees from it.
# GodotNext: #256. Medium -> High -> Medium on both kitbash views.
func _process_switch() -> void:
	match _phase:
		0:
			if _phase_frame < WARMUP_FRAMES:
				return
			_check_lite("switch_medium", 2)
			ProjectSettings.set_setting(QUALITY_SETTING, 2)
			_next_phase()
		1:
			if _phase_frame < SWITCH_FRAMES:
				return
			var info: Dictionary = RenderingServer.nextgi_get_tier_info()
			var fields := int(info.get("lite_view_fields", -1))
			print("LITE_TWO_VIEWS switch_high quality=%s lite=%s lite_view_fields=%d" % [str(info.get("quality", "")), str(info.get("lite_beta", "")), fields])
			_summary["switch_high_fields"] = fields
			_check(not bool(info.get("lite_beta", true)), "after the switch to High, Lite is still active")
			_check(fields == 0, "after the switch to High, %d views still hold a Lite field" % fields)
			ProjectSettings.set_setting(QUALITY_SETTING, 1)
			_next_phase()
		2:
			# Two frames after the switch back: each view's new field packed every probe on its first
			# Lite frame, and the steady 512 a frame since.
			if _phase_frame < 2:
				return
			_check_lite("switch_back", 2)
			for key in ["root", "sub"]:
				var stats := _field_stats(_views[key])
				print("LITE_TWO_VIEWS switch_back view=%s lit=%.3f spread=%.3f" % [key, stats["lit_fraction"], stats["spread_fraction"]])
				_check(float(stats["lit_fraction"]) >= MIN_LIT_FRACTION, "back in Lite, the %s view is lit on %.3f of its samples, below %.2f" % [key, stats["lit_fraction"], MIN_LIT_FRACTION])
				_check(float(stats["spread_fraction"]) >= MIN_SPREAD, "back in Lite, the %s view is flat (spread %.3f)" % [key, stats["spread_fraction"]])
			_finish()


func _make_kitbash_view(p_update_mode: SubViewport.UpdateMode, p_yaw: float) -> SubViewport:
	var viewport := _make_sub_view(SUB_VIEW_SIZE, p_update_mode)
	var camera := Camera3D.new()
	camera.fov = 70.0
	viewport.add_child(camera)
	camera.position = _camera_position
	camera.rotation_degrees = _camera_rotation + Vector3(0.0, p_yaw, 0.0)
	camera.current = true
	return viewport


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
			if case_name == "handoff":
				_process_handoff_lit()
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
		2:
			_process_handoff_dark()


# A room view's camera looks down at its room's floor, FLOOR_OFFSET from the wall.
func _place_room_camera(p_camera: Camera3D, p_view: String) -> void:
	var side := -1.0 if p_view == "lit" else 1.0
	var target := Vector3(side * (WALL * 0.5 + FLOOR_OFFSET), 0.0, 0.0)
	p_camera.look_at_from_position(target + Vector3(0.0, 1.2, 0.0), target, Vector3(0.0, 0.0, -1.0))


# GodotNext: #254. The lit room's patch, then the view's camera moves over the dark room.
func _process_handoff_lit() -> void:
	if (_phase_frame - ROOM_MEASURE_FRAMES) % ROOM_SAMPLE_STRIDE != 0:
		return
	var values: PackedFloat32Array = _samples.get("lit", PackedFloat32Array())
	values.append(_patch_luma(_views["lit"]))
	_samples["lit"] = values
	if values.size() < ROOM_SAMPLES:
		return
	_summary["lit_luma"] = _mean(values)
	var viewport := _views["lit"] as SubViewport
	_place_room_camera(viewport.get_camera_3d(), "dark")
	_views["dark"] = viewport
	_views.erase("lit")
	_next_phase()


func _process_handoff_dark() -> void:
	if _phase_frame <= HANDOFF_FRAMES or (_phase_frame - HANDOFF_FRAMES) % ROOM_SAMPLE_STRIDE != 0:
		return
	var values: PackedFloat32Array = _samples.get("dark", PackedFloat32Array())
	values.append(_patch_luma(_views["dark"]))
	_samples["dark"] = values
	if values.size() < ROOM_SAMPLES:
		return
	var lit := float(_summary["lit_luma"])
	var dark := _mean(values)
	_summary["dark_luma_after_handoff"] = dark
	print("LITE_TWO_VIEWS_HANDOFF lit=%.6f dark=%.6f ratio=%.4f frames_after=%d" % [lit, dark, dark / maxf(lit, 1e-9), _phase_frame])
	_check(lit >= MIN_ROOM_LIT, "the lit room's ambient (%.6f) is below %.3f: the comparison would see nothing" % [lit, MIN_ROOM_LIT])
	_check(dark <= MAX_DARK_RATIO * lit, "%d frames after the camera moved, the dark room reads %.6f, %.3f of the lit room's %.6f (at most %.2f)" % [HANDOFF_FRAMES, dark, dark / maxf(lit, 1e-9), lit, MAX_DARK_RATIO])
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
