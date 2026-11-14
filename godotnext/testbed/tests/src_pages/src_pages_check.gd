extends Node3D

## #247: SRC page levels and solid-angle priority under a moving camera (the kitbash, Lumen Lite).
##
## --flight=move: the camera flies MOVE_FRAMES frames across the kitbash from START to END, then
## holds at END for SETTLE_FRAMES. Rocks change level as it passes (re-cuts and demotions) and
## nearer rocks take the pages of farther ones (priority evictions), so the run requires
## src_page_recuts, src_page_demotions and src_page_priority_evictions above 0.
## --flight=still: the camera starts at END and holds there for SETTLE_FRAMES.
## Both then average the view's denoised ambient (DEBUG_DRAW_NEXTGI_DENOISE_DENOISED) over SAMPLES
## frames and print "SRC_PAGES_AMBIENT flight=<flight> lit=<l> luma=<m>", so both runs' pages at
## END had the same frames to relight and converge. run_src_pages_check.sh runs still first and
## passes its luma to move as --still-luma: the moved run's luma must be within TOLERANCE of it,
## so no re-cut, demoted or reused block shows another card's radiance. On a still camera the
## allocator settles: no src_page_alloc_failures during the samples, and the page-table entry
## count stays within MAX_ENTRIES, the kitbash's cards (before #247 the entries grew past it on a
## still camera).
## --expected-tier=HWRT, SWRT or device. Prints one "GODOTNEXT_RESULT <json>" line and exits 0
## when every check passes.

const KITBASH_SCENE := "res://scenes/swrt_kitbash/swrt_kitbash.tscn"
const START := Vector3(-24.0, 3.0, 6.0)
const END := Vector3(18.0, 2.5, 8.0)
const TARGET_OFFSET := Vector3(0.0, -1.5, -8.0)
const MOVE_FRAMES := 480
const SETTLE_FRAMES := 240
const SAMPLES := 30
const MAX_ENTRIES := 2400 * 12 + 2
const TOLERANCE := 0.1
const LIT_LUMA := 0.002

var flight := ""
var expected_tier := ""
var still_luma := -1.0
var _camera: Camera3D
var _frames := 0
var _lit := PackedFloat32Array()
var _luma := PackedFloat32Array()
var _max_entries := 0
var _failures_before_samples := -1
var _failures := PackedStringArray()
var _done := false


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--flight="):
			flight = arg.trim_prefix("--flight=")
		elif arg.begins_with("--expected-tier="):
			expected_tier = arg.trim_prefix("--expected-tier=")
		elif arg.begins_with("--still-luma="):
			still_luma = float(arg.trim_prefix("--still-luma="))
	if expected_tier == "device":
		var device := RenderingServer.get_rendering_device()
		var hardware := device != null and device.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY_HARDWARE)
		var denied := bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/debug/deny_ray_query", false))
		expected_tier = "HWRT" if hardware and not denied else "SWRT"
	if flight != "move" and flight != "still":
		_finish_with("--flight must be move or still")
		return
	var root: Node = (load(KITBASH_SCENE) as PackedScene).instantiate()
	add_child(root)
	var world := root.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		var environment: Environment = world.environment.duplicate()
		environment.nextgi_enabled = true
		world.environment = environment
	_camera = Camera3D.new()
	_camera.fov = 70.0
	add_child(_camera)
	_place(START if flight == "move" else END)
	_camera.current = true
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGI_DENOISE_DENOISED


func _place(p_position: Vector3) -> void:
	_camera.look_at_from_position(p_position, p_position + TARGET_OFFSET)


func _process(_delta: float) -> void:
	if _done:
		return
	_frames += 1
	if flight == "move" and _frames <= MOVE_FRAMES:
		_place(START.lerp(END, float(_frames) / float(MOVE_FRAMES)))
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	_max_entries = maxi(_max_entries, int(info.get("src_resident_pages", 0)))
	if _frames <= (MOVE_FRAMES if flight == "move" else 0) + SETTLE_FRAMES:
		return
	if _failures_before_samples < 0:
		_failures_before_samples = int(info.get("src_page_alloc_failures", -1))
	var stats := _ambient()
	_lit.append(stats.x)
	_luma.append(stats.y)
	if _lit.size() < SAMPLES:
		return
	var lit := _mean(_lit)
	var luma := _mean(_luma)
	print("SRC_PAGES_AMBIENT flight=%s lit=%.6f luma=%.6f" % [flight, lit, luma])
	_check(String(info.get("tier", "")) == expected_tier, "tier is %s, expected %s" % [String(info.get("tier", "")), expected_tier])
	_check(String(info.get("quality", "")) == "MEDIUM", "quality is %s, expected MEDIUM (Lumen Lite)" % String(info.get("quality", "")))
	_check(lit > 0.2, "the view is lit on %.3f of its samples: the comparison would see nothing" % lit)
	var failures := int(info.get("src_page_alloc_failures", -1))
	print("SRC_PAGES_FAILURES total=%d during_samples=%d" % [failures, failures - _failures_before_samples])
	_check(failures == _failures_before_samples, "%d page sets failed to fit while the camera held still" % (failures - _failures_before_samples))
	_check(_max_entries <= MAX_ENTRIES, "the page table reached %d entries, more than the kitbash's %d cards" % [_max_entries, MAX_ENTRIES])
	if flight == "move":
		for key in ["src_page_recuts", "src_page_demotions", "src_page_priority_evictions"]:
			print("SRC_PAGES_%s=%d" % [key.trim_prefix("src_page_").to_upper(), int(info.get(key, -1))])
			_check(int(info.get(key, -1)) > 0, "%s is %d: the flight did not move any page" % [key, int(info.get(key, -1))])
		if still_luma >= 0.0:
			_check(absf(luma - still_luma) <= TOLERANCE * still_luma, "after the flight the ambient luma is %.5f, against %.5f from a still camera there (more than %.0f%% apart)" % [luma, still_luma, TOLERANCE * 100.0])
		else:
			_check(false, "--flight=move needs --still-luma from a --flight=still run")
	_finish()


# Lit share and mean luma of lit samples over a 64 x 36 grid of the denoised ambient.
func _ambient() -> Vector2:
	var image := get_viewport().get_texture().get_image()
	var lit := 0
	var total := 0
	var luma_sum := 0.0
	var step_x := maxi(image.get_width() / 64, 1)
	var step_y := maxi(image.get_height() / 36, 1)
	for y in range(step_y / 2, image.get_height(), step_y):
		for x in range(step_x / 2, image.get_width(), step_x):
			var luma := image.get_pixel(x, y).get_luminance()
			if luma > LIT_LUMA:
				lit += 1
				luma_sum += luma
			total += 1
	return Vector2(float(lit) / float(maxi(total, 1)), luma_sum / float(maxi(lit, 1)))


func _mean(p_values: PackedFloat32Array) -> float:
	var sum := 0.0
	for value in p_values:
		sum += value
	return sum / float(maxi(p_values.size(), 1))


func _check(p_condition: bool, p_failure: String) -> void:
	if not p_condition:
		_failures.append(p_failure)


func _finish_with(p_failure: String) -> void:
	_failures.append(p_failure)
	_finish()


func _finish() -> void:
	_done = true
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "src_pages",
		"flight": flight,
		"pass": _failures.is_empty(),
		"expected_tier": expected_tier,
		"tier": info.get("tier", ""),
		"lit": _mean(_lit),
		"luma": _mean(_luma),
		"max_entries": _max_entries,
		"sets": info.get("src_page_sets_current", -1),
		"levels": info.get("src_page_level_sets", []),
		"failures": _failures,
	}))
	get_tree().quit(0 if _failures.is_empty() else 1)
