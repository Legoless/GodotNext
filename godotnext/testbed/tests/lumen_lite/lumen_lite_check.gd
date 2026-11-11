extends Node3D
## #183: Lumen Lite (Medium with the irradiance-field gather) on the tier the folder override picks.
## - Lite records 0 gather and 0 reflection dispatches and refreshes probes every frame.
## - The dispatch counters are live: switching medium_gather to Screen Probes at runtime must
##   record gather dispatches, and switching back must return them to 0.
## - field_checks: the field is not empty. The "NextGI Lite Probes" view shows irradiance from
##   the surface cache alone (no sky, sun-lit cards), and turning the sky back on brightens it
##   (the WRC's misses read the sky). Environment.nextgi_read_sky_light off takes the sky out
##   of the field again.
## tests/lumen_lite runs the software tier on the kitbash (the PS smoke scene), without the field
## checks, and on the Roughness Corridor with them (lumen_lite_corridor_check.tscn): GodotNext:
## ps_swrt_trace (#186, #188), the software tier captures and relights the SRC, and its WRC probes
## shade their SDF hits from voxel lighting resampled from it.
## tests/lumen_lite_hwrt runs the hardware tier on the Roughness Corridor, whose few instances
## all fit the Lite radiance atlas; the kitbash's 2,400 rocks do not, which is #41's eviction.
## Under debug/deny_ray_query (a device without ray query) every dispatch check also asserts
## NO_RAY_QUERY, no RT arena, no ray-query ShaderRD and SWRT instances.

const WARMUP_FRAMES := 240
const SKY_FRAMES := 240
## The field decays by the WRC history (90% per 32-frame re-trace): about 0.3 of the sky term is
## left after 360 frames.
const SKY_OFF_FRAMES := 360
const TOGGLE_FRAMES := 12
## Share of sampled pixels with non-zero irradiance (the sky itself has no depth and stays black).
const MIN_CARD_LIT_FRACTION := 0.3
const MIN_SKY_LIT_FRACTION := 0.3

@export_file("*.tscn") var scene_path := "res://scenes/swrt_kitbash/swrt_kitbash.tscn"
## A camera in the scene to view from; empty places one over the kitbash.
@export var camera_name := ""

@export var expected_tier := "SWRT"
## The field phases (card radiance only, sky, sky light off). They need a scene whose instances fit
## the Lite radiance atlas.
@export var field_checks := false

var _frames := 0
var _phase := 0
var _phase_frame := 0
var _environment: Environment
var _sky_background := Environment.BG_SKY
var _sky: Sky
var _card_only_luma := 0.0
var _card_only_lit := 0.0
var _sky_luma := 0.0
var _summary := {}
var _done := false


func _ready() -> void:
	var packed: PackedScene = load(scene_path)
	var root := packed.instantiate()
	add_child(root)
	# The scene's own script may replace the Environment in _ready, so take it afterwards.
	var world := root.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		_environment = world.environment.duplicate()
		_environment.nextgi_enabled = true
		world.environment = _environment
		_sky_background = _environment.background_mode
		_sky = _environment.sky
		if field_checks:
			# Start with only the sun on the surface cache: no sky term reaches the field.
			_environment.background_mode = Environment.BG_COLOR
			_environment.background_color = Color.BLACK
			_environment.sky = null
	var scene_camera: Camera3D = null
	if not camera_name.is_empty():
		scene_camera = root.get_node_or_null(NodePath(camera_name)) as Camera3D
	if scene_camera != null:
		scene_camera.current = true
	else:
		var camera := Camera3D.new()
		add_child(camera)
		camera.position = Vector3(0.0, 2.0, 8.0)
		camera.look_at(Vector3(0.0, 1.0, 0.0))
		camera.current = true


func _write_manifest() -> void:
	var args := OS.get_cmdline_user_args()
	var out_dir := ""
	for index in args.size():
		var arg := str(args[index])
		if arg.begins_with("--out="):
			out_dir = arg.substr("--out=".length())
		elif arg == "--out" and index + 1 < args.size():
			out_dir = str(args[index + 1])
	if out_dir.is_empty():
		return
	DirAccess.make_dir_recursive_absolute(out_dir)
	var file := FileAccess.open(out_dir.path_join("manifest.json"), FileAccess.WRITE)
	if file == null:
		return
	file.store_string(JSON.stringify({"source": "lumen_lite_check", "summary": _summary}, "\t") + "\n")


func _fail(reason: String) -> void:
	if _done:
		return
	_done = true
	push_error(reason)
	print("LITE_DISPATCH_FAIL %s" % reason)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/medium_gather", 0)
	_write_manifest()
	get_tree().quit(1)


func _counters() -> Dictionary:
	var counters: Dictionary = RenderingServer.nextgi_get_counters()
	for key in ["gather_dispatches", "reflection_dispatches", "lite_probe_traces"]:
		if not counters.has(key):
			_fail("missing counter %s" % key)
			return {}
	return counters


# Share of pixels with non-zero irradiance and their mean luminance, from the Lite debug view.
# A missing field texture draws a uniform tint, which the spread test rejects.
func _field_stats() -> Dictionary:
	var image := get_viewport().get_texture().get_image()
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
			if luma > 0.002:
				lit += 1
				luma_sum += luma
			total += 1
	return {
		"lit_fraction": float(lit) / float(maxi(total, 1)),
		"mean_luma": luma_sum / float(maxi(lit, 1)),
		"spread_fraction": float(differing) / float(maxi(total, 1)),
	}


func _process(_delta: float) -> void:
	if _done:
		return
	_frames += 1
	_phase_frame += 1
	match _phase:
		0:
			if _phase_frame < WARMUP_FRAMES:
				return
			if not _check_lite_dispatches("lite"):
				return
			if field_checks:
				get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGI_LITE_PROBES
				_next_phase(1)
			else:
				_next_phase(4)
		1:
			# Card radiance only.
			if _phase_frame < TOGGLE_FRAMES:
				return
			var stats := _field_stats()
			_summary["card_only"] = stats
			print("LITE_FIELD card_only lit=%.3f luma=%.4f spread=%.3f" % [stats["lit_fraction"], stats["mean_luma"], stats["spread_fraction"]])
			if float(stats["lit_fraction"]) < MIN_CARD_LIT_FRACTION or float(stats["spread_fraction"]) < 0.05:
				_fail("the Lite field is empty without the sky: no surface-cache radiance reaches the probes")
				return
			_card_only_luma = float(stats["mean_luma"])
			_card_only_lit = float(stats["lit_fraction"])
			_environment.sky = _sky
			_environment.background_mode = _sky_background
			_next_phase(2)
		2:
			# The sky returns. Each WRC probe re-traces every 32 frames and keeps 90% history.
			if _phase_frame < SKY_FRAMES:
				return
			var stats := _field_stats()
			_summary["sky"] = stats
			print("LITE_FIELD sky lit=%.3f luma=%.4f spread=%.3f" % [stats["lit_fraction"], stats["mean_luma"], stats["spread_fraction"]])
			# Sky-lit surfaces the sun misses join the field, and the field brightens well past the
			# card-only level (about 2.7x on the corridor; WRC history alone reaches about 1.2x).
			if float(stats["lit_fraction"]) < MIN_SKY_LIT_FRACTION or float(stats["lit_fraction"]) < _card_only_lit + 0.1 or float(stats["mean_luma"]) < _card_only_luma * 1.6:
				_fail("the sky does not reach the Lite field (WRC misses)")
				return
			_sky_luma = float(stats["mean_luma"])
			_environment.nextgi_read_sky_light = false
			_next_phase(3)
		3:
			# Environment.nextgi_read_sky_light off: WRC misses read the (black) background color.
			if _phase_frame < SKY_OFF_FRAMES:
				return
			var stats := _field_stats()
			_summary["sky_light_off"] = stats
			print("LITE_FIELD sky_light_off lit=%.3f luma=%.4f spread=%.3f" % [stats["lit_fraction"], stats["mean_luma"], stats["spread_fraction"]])
			if float(stats["mean_luma"]) > _sky_luma * 0.8:
				_fail("Environment.nextgi_read_sky_light off still lets the sky into the Lite field")
				return
			_environment.nextgi_read_sky_light = true
			get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
			if not _check_lite_dispatches("lite_sky"):
				return
			_next_phase(4)
		4:
			ProjectSettings.set_setting("rendering/global_illumination/nextgi/medium_gather", 1)
			_next_phase(5)
		5:
			if _phase_frame < TOGGLE_FRAMES:
				return
			var counters := _counters()
			if counters.is_empty():
				return
			var gathers := int(counters["gather_dispatches"])
			print("LITE_DISPATCH screen_probes gather=%d reflection=%d" % [gathers, int(counters["reflection_dispatches"])])
			_summary["screen_probes_gather_dispatches"] = gathers
			if gathers <= 0:
				_fail("screen_probes recorded no gather dispatch: the counters do not see dispatches")
				return
			ProjectSettings.set_setting("rendering/global_illumination/nextgi/medium_gather", 0)
			_next_phase(6)
		6:
			if _phase_frame < TOGGLE_FRAMES:
				return
			if not _check_lite_dispatches("lite_again"):
				return
			_done = true
			_write_manifest()
			get_tree().quit(0)


func _next_phase(phase: int) -> void:
	_phase = phase
	_phase_frame = 0


func _check_lite_dispatches(label: String) -> bool:
	var counters := _counters()
	if counters.is_empty():
		return false
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	if str(info.get("tier", "")) != expected_tier:
		_fail("tier is %s, expected %s" % [str(info.get("tier", "")), expected_tier])
		return false
	if not bool(info.get("lite_beta", false)):
		_fail("Lite is not active (quality %s)" % str(info.get("quality", "")))
		return false
	var gather_rays := int(counters["gather_dispatches"])
	var reflection_rays := int(counters["reflection_dispatches"])
	var probe_traces := int(counters["lite_probe_traces"])
	print("LITE_DISPATCH %s tier=%s gather_rays=%d reflection_rays=%d probe_traces=%d" % [label, expected_tier, gather_rays, reflection_rays, probe_traces])
	_summary[label] = {"gather": gather_rays, "reflection": reflection_rays, "probe_traces": probe_traces}
	if gather_rays != 0 or reflection_rays != 0:
		_fail("lite dispatched screen or reflection rays")
		return false
	if probe_traces <= 0:
		_fail("lite probe traces are 0")
		return false
	# GodotNext: ps_swrt_trace (#186, #188). debug/deny_ray_query: a device without ray query.
	if bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/debug/deny_ray_query", false)):
		print("LITE_DENY reason=%s rt_arena_bytes=%d ray_query_shader_versions=%d swrt_trace_instances=%d" % [str(info.get("reason", "")), int(info.get("rt_arena_bytes", -1)), int(info.get("ray_query_shader_versions", -1)), int(info.get("swrt_trace_instances", 0))])
		if str(info.get("reason", "")) != "NO_RAY_QUERY" or int(info.get("rt_arena_bytes", -1)) != 0 or int(info.get("ray_query_shader_versions", -1)) != 0 or int(info.get("swrt_trace_instances", 0)) <= 0:
			_fail("deny_ray_query: expected NO_RAY_QUERY, no RT arena, no ray-query ShaderRD and SWRT instances")
			return false
	return true
