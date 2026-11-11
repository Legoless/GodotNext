extends Node

## #186: the software tier renders GI through the frame block (plan §21.2.4).
##
## --check=smoke: the overlap-heavy kitbash (the PS smoke scene) in a 1920x1080 SubViewport, the
## camera at (0, 2, 8) looking at (0, 1, 0), NextGI on in its Environment. After WARMUP_FRAMES
## it samples RenderingServer.nextgi_get_tier_info() every frame for SAMPLE_FRAMES frames and
## keeps the frame with the most gather rays (the counters arrive asynchronously, one snapshot
## per frame) and the one with the most WRC rays.
##
## With --expected-tier=SWRT it asserts:
## - the tier is SWRT, the gather ran the software sibling (gather_tier SWRT) and traced rays, and
##   no ray-query ShaderRD was created since the tier was selected, nor in the whole process when
##   hardware_ray_tracing was Off at launch (the runner's software passes; Off skips the startup
##   self-test);
## - clean SDF hits (gather_sdf_hits minus the step-limit and near-origin ones) are at least
##   CLEAN_HIT_SHARE of the hardware tier's world hits on the same view (--hwrt-world-hits, from
##   the hardware pass that run_swrt_gi_check.sh runs first: rays - misses - screen hits);
## - step-limit (exhausted) hits are at most MAX_EXHAUSTED_SHARE of the SDF hits, and hits within
##   two voxels of the origin's level (self-intersection candidates) at most MAX_NEAR_SHARE. The
##   kitbash's overlapping rocks put real surfaces that close: on this view the hardware tier finds
##   30% of its world hits within 0.35 m of the gather's surface point (measured on the M5 Max with
##   a temporary counting build), so MAX_NEAR_SHARE is that hardware share, not a software one.
##   The shares are printed;
## - the Hi-Z screen trace found hits;
## - the software WRC producer ran (O2): WRC rays, gather samples from the WRC and clean WRC
##   SDF hits;
## - the denoised ambient (DEBUG_DRAW_NEXTGI_DENOISE_DENOISED) is not uniform: at least
##   MIN_SPREAD of its samples differ from the first.
## The WRC step-limit and near shares and the mean SDF hit distance are printed report-only.
## With --expected-tier=HWRT it only checks the tier and prints the hardware reference
## ("SWRT_GI_REFERENCE world_hits=N").
##
## --check=toggle (start on the hardware tier, hardware_ray_tracing Auto): the same view switches
## at runtime to the software tier (nextgi_set_hardware_ray_tracing(1)) and back (0), TOGGLE_FRAMES
## frames each. On the software tier the gather runs the software sibling and no ray-query
## ShaderRD is created since the switch (the hardware tier's shader states stay alive, so the
## census counts since the tier change); back on the hardware tier the gather runs the hardware
## shader again. No frame block changes tier inside its frame (mid_frame_tier_changes 0).
##
## --check=budget (start on the hardware tier, hardware_ray_tracing Auto): the tier is fixed for a
## render frame, and an AS-memory change applies at the next frame's start (#186). After
## BUDGET_WARMUP_FRAMES frames on the hardware tier it sets debug/as_budget_bytes to 1 byte, so the
## next AS-builds slot finds AS memory exceeded, and it samples nextgi_get_tier_info() every frame:
## - the first frame that reports AS memory exceeded (rt_as_memory_exceeded) ended on the hardware
##   tier, and its gather dispatched (gather_diffuse_dispatches grew). The slot timestamps are
##   recorded on either tier, so they cannot show the change;
## - the next frame is SWRT with reason AS_MEMORY, and the tier stays SWRT for BUDGET_FRAMES frames;
## - then the gather runs the software sibling, no ray-query ShaderRD was created since the switch,
##   and no frame block changed tier inside its frame (mid_frame_tier_changes 0).
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.

const KITBASH_SCENE := "res://scenes/swrt_kitbash/swrt_kitbash.tscn"
const VIEW_SIZE := Vector2i(1920, 1080)
const WARMUP_FRAMES := 240
const SAMPLE_FRAMES := 30
const CLEAN_HIT_SHARE := 0.25
const MAX_EXHAUSTED_SHARE := 0.10
const MAX_NEAR_SHARE := 0.30
const MIN_SPREAD := 0.05
const TOGGLE_FRAMES := 60
const AS_BUDGET_SETTING := "rendering/global_illumination/nextgi/debug/as_budget_bytes"
const BUDGET_WARMUP_FRAMES := 60
const BUDGET_WAIT_FRAMES := 30
const BUDGET_FRAMES := 60

var mode := "smoke"
var expected_tier := "SWRT"
var hwrt_world_hits := -1
var viewport: SubViewport
var frames := 0
var gather_best := {}
var wrc_best := {}
var wrc_clean_best := -1
var failures := PackedStringArray()
var done := false
var toggle_phase := 0
var toggle_frame := 0
var toggle_log := []
var budget_phase := 0
var budget_frame := 0
var budget_dispatches := -1
var hwrt_off_at_launch := false


func _ready() -> void:
	hwrt_off_at_launch = int(ProjectSettings.get_setting("rendering/global_illumination/nextgi/hardware_ray_tracing", 0)) == 1
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--check="):
			mode = arg.trim_prefix("--check=")
		elif arg.begins_with("--expected-tier="):
			expected_tier = arg.trim_prefix("--expected-tier=")
		elif arg.begins_with("--hwrt-world-hits="):
			hwrt_world_hits = int(arg.trim_prefix("--hwrt-world-hits="))
	if mode != "smoke" and mode != "toggle" and mode != "budget":
		_finish(["unknown --check=%s (smoke, toggle or budget)" % mode])
		return
	if expected_tier != "SWRT" and expected_tier != "HWRT":
		_finish(["--expected-tier must be SWRT or HWRT"])
		return
	if mode == "smoke" and expected_tier == "SWRT" and hwrt_world_hits <= 0:
		_finish(["--hwrt-world-hits=N (the hardware tier's world hits) is required with --expected-tier=SWRT"])
		return
	viewport = SubViewport.new()
	viewport.size = VIEW_SIZE
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.use_hdr_2d = true
	viewport.debug_draw = Viewport.DEBUG_DRAW_NEXTGI_DENOISE_DENOISED
	add_child(viewport)
	var root: Node3D = load(KITBASH_SCENE).instantiate()
	viewport.add_child(root)
	var world := root.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world == null or world.environment == null:
		_finish(["the kitbash has no WorldEnvironment"])
		return
	var environment: Environment = world.environment.duplicate()
	environment.nextgi_enabled = true
	world.environment = environment
	var camera := Camera3D.new()
	root.add_child(camera)
	camera.look_at_from_position(Vector3(0.0, 2.0, 8.0), Vector3(0.0, 1.0, 0.0))
	camera.current = true


func _process(_delta: float) -> void:
	if done:
		return
	frames += 1
	if mode == "toggle":
		_process_toggle()
		return
	if mode == "budget":
		_process_budget()
		return
	if frames <= WARMUP_FRAMES:
		return
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	if int(info.get("src_gather_rays", 0)) > int(gather_best.get("src_gather_rays", -1)):
		gather_best = info
	# The WRC frame with the most clean SDF hits (its update traces a different probe set each frame).
	var wrc_clean := int(info.get("wrc_sdf_hits", 0)) - int(info.get("wrc_sdf_exhausted", 0)) - int(info.get("wrc_sdf_near", 0))
	if wrc_clean > wrc_clean_best:
		wrc_clean_best = wrc_clean
		wrc_best = info
	if frames < WARMUP_FRAMES + SAMPLE_FRAMES:
		return
	_finish(_checks(info))


func _process_toggle() -> void:
	toggle_frame += 1
	if toggle_frame < TOGGLE_FRAMES:
		return
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var expected := "SWRT" if toggle_phase == 1 else "HWRT"
	var entry := {
		"phase": toggle_phase,
		"tier": info.get("tier", ""),
		"gather_tier": info.get("gather_tier", ""),
		"since_tier": info.get("ray_query_shader_versions_since_tier", -1),
		"total": info.get("ray_query_shader_versions", -1),
	}
	toggle_log.append(entry)
	print("SWRT_GI_TOGGLE %s" % [entry])
	if String(info.get("tier", "")) != expected:
		failures.append("phase %d: tier %s, expected %s" % [toggle_phase, String(info.get("tier", "")), expected])
	if String(info.get("gather_tier", "")) != expected:
		failures.append("phase %d: the gather ran the %s shader on the %s tier" % [toggle_phase, String(info.get("gather_tier", "")), expected])
	if toggle_phase == 1 and int(info.get("ray_query_shader_versions_since_tier", -1)) != 0:
		failures.append("%d ray-query ShaderRD versions were created after the switch to the software tier" % [int(info.get("ray_query_shader_versions_since_tier", -1))])
	if int(info.get("mid_frame_tier_changes", -1)) != 0:
		failures.append("phase %d: %d frame blocks changed tier inside their frame" % [toggle_phase, int(info.get("mid_frame_tier_changes", -1))])
	toggle_frame = 0
	toggle_phase += 1
	if toggle_phase == 1:
		RenderingServer.nextgi_set_hardware_ray_tracing(1)
	elif toggle_phase == 2:
		RenderingServer.nextgi_set_hardware_ray_tracing(0)
	else:
		_finish(failures)


func _process_budget() -> void:
	budget_frame += 1
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var tier := String(info.get("tier", ""))
	var exceeded := bool(info.get("rt_as_memory_exceeded", false))
	var dispatches := int(info.get("gather_diffuse_dispatches", 0))
	if budget_phase == 0:
		# Steady frames on the hardware tier, then the 1-byte budget.
		if budget_frame < BUDGET_WARMUP_FRAMES:
			return
		if tier != "HWRT" or String(info.get("gather_tier", "")) != "HWRT" or exceeded:
			_finish(["the run did not settle on the hardware tier (tier %s, gather %s, AS memory exceeded %s)" % [tier, String(info.get("gather_tier", "")), exceeded]])
			return
		budget_dispatches = dispatches
		ProjectSettings.set_setting(AS_BUDGET_SETTING, 1)
		budget_phase = 1
		budget_frame = 0
		return
	var entry := {
		"phase": budget_phase,
		"frame": budget_frame,
		"tier": tier,
		"reason": info.get("reason", ""),
		"as_memory_exceeded": exceeded,
		"gather_tier": info.get("gather_tier", ""),
		"gather_diffuse_dispatches": dispatches,
		"mid_frame_tier_changes": info.get("mid_frame_tier_changes", -1),
	}
	if budget_phase == 1:
		# The frame that first exceeds the budget keeps its tier to the end.
		if tier == "HWRT" and not exceeded:
			if budget_frame >= BUDGET_WAIT_FRAMES:
				_finish(["AS memory was not reported exceeded within %d frames of a 1-byte budget" % BUDGET_WAIT_FRAMES])
				return
			budget_dispatches = dispatches
			return
		print("SWRT_GI_BUDGET %s" % [entry])
		if tier != "HWRT":
			failures.append("the frame that exceeded AS memory ended on the %s tier, not the HWRT tier it began on" % tier)
		if dispatches <= budget_dispatches:
			failures.append("the gather did not dispatch in the frame that exceeded AS memory (%d dispatches, %d before)" % [dispatches, budget_dispatches])
		budget_phase = 2
		budget_frame = 0
		return
	if budget_phase == 2:
		# The next frame starts on the software tier.
		print("SWRT_GI_BUDGET %s" % [entry])
		if tier != "SWRT" or String(info.get("reason", "")) != "AS_MEMORY":
			failures.append("the frame after AS memory was exceeded is %s/%s, expected SWRT/AS_MEMORY" % [tier, String(info.get("reason", ""))])
		budget_phase = 3
		budget_frame = 0
		return
	if tier != "SWRT":
		failures.append("the tier went back to %s %d frames after the budget switch" % [tier, budget_frame])
		_finish(failures)
		return
	if budget_frame < BUDGET_FRAMES:
		return
	print("SWRT_GI_BUDGET %s" % [entry])
	if String(info.get("gather_tier", "")) != "SWRT":
		failures.append("the gather ran the %s shader after the budget switch" % [String(info.get("gather_tier", ""))])
	if int(info.get("ray_query_shader_versions_since_tier", -1)) != 0:
		failures.append("%d ray-query ShaderRD versions were created after the budget switch" % [int(info.get("ray_query_shader_versions_since_tier", -1))])
	if int(info.get("mid_frame_tier_changes", -1)) != 0:
		failures.append("%d frame blocks changed tier inside their frame" % [int(info.get("mid_frame_tier_changes", -1))])
	_finish(failures)


func _spread() -> float:
	var image := viewport.get_texture().get_image()
	var first := -1.0
	var differing := 0
	var total := 0
	var step_x := maxi(image.get_width() / 64, 1)
	var step_y := maxi(image.get_height() / 36, 1)
	for y in range(step_y / 2, image.get_height(), step_y):
		for x in range(step_x / 2, image.get_width(), step_x):
			var luma := image.get_pixel(x, y).get_luminance()
			if first < 0.0:
				first = luma
			if absf(luma - first) > 0.01:
				differing += 1
			total += 1
	return float(differing) / float(maxi(total, 1))


func _checks(p_last: Dictionary) -> PackedStringArray:
	var result := PackedStringArray()
	var tier := String(p_last.get("tier", ""))
	if tier != expected_tier:
		result.append("tier is %s, expected %s" % [tier, expected_tier])
		return result
	var rays := int(gather_best.get("src_gather_rays", 0))
	var misses := int(gather_best.get("src_gather_misses", 0))
	var screen_hits := int(gather_best.get("screen_trace_hits", 0))
	if expected_tier == "HWRT":
		print("SWRT_GI_REFERENCE world_hits=%d rays=%d" % [rays - misses - screen_hits, rays])
		return result
	var sdf_hits := int(gather_best.get("gather_sdf_hits", 0))
	var exhausted := int(gather_best.get("gather_sdf_exhausted", 0))
	var near := int(gather_best.get("gather_sdf_near", 0))
	var clean := sdf_hits - exhausted - near
	var wrc_hits := int(wrc_best.get("wrc_sdf_hits", 0))
	var spread := _spread()
	print("SWRT_GI_SMOKE quality=%s rays=%d sdf_hits=%d clean=%d (HWRT world hits %d, ratio %.3f, report-only) exhausted=%d (%.4f) near=%d (%.4f) screen_hits=%d mean_hit_m=%.3f wrc_rays=%d wrc_gather_samples=%d wrc_sdf_hits=%d wrc_clean=%d wrc_exhausted_share=%.4f wrc_near_share=%.4f spread=%.3f" % [
		String(p_last.get("quality", "")), rays, sdf_hits, clean, hwrt_world_hits, float(clean) / float(maxi(hwrt_world_hits, 1)),
		exhausted, float(exhausted) / float(maxi(sdf_hits, 1)), near, float(near) / float(maxi(sdf_hits, 1)), screen_hits,
		float(int(gather_best.get("gather_sdf_hit_cm_sum", 0))) / 100.0 / float(maxi(sdf_hits, 1)),
		int(wrc_best.get("wrc_rays", 0)), int(gather_best.get("wrc_gather_samples", 0)), wrc_hits, wrc_clean_best,
		float(int(wrc_best.get("wrc_sdf_exhausted", 0))) / float(maxi(wrc_hits, 1)), float(int(wrc_best.get("wrc_sdf_near", 0))) / float(maxi(wrc_hits, 1)), spread])
	if String(gather_best.get("gather_tier", "")) != "SWRT":
		result.append("the gather ran the %s shader, expected the software sibling" % [String(gather_best.get("gather_tier", ""))])
	if rays <= 0:
		result.append("the gather traced no rays")
	if int(p_last.get("ray_query_shader_versions_since_tier", -1)) != 0:
		result.append("%d ray-query ShaderRD versions were created on the SWRT tier" % [int(p_last.get("ray_query_shader_versions_since_tier", -1))])
	if hwrt_off_at_launch and int(p_last.get("ray_query_shader_versions", -1)) != 0:
		result.append("%d ray-query ShaderRD versions were created in a process started with hardware_ray_tracing Off" % [int(p_last.get("ray_query_shader_versions", -1))])
	if float(clean) < CLEAN_HIT_SHARE * float(hwrt_world_hits):
		result.append("%d clean SDF hits, expected at least %.2f x the hardware tier's %d world hits" % [clean, CLEAN_HIT_SHARE, hwrt_world_hits])
	if float(exhausted) > MAX_EXHAUSTED_SHARE * float(sdf_hits):
		result.append("%d of %d SDF hits ran out of steps (more than %.2f)" % [exhausted, sdf_hits, MAX_EXHAUSTED_SHARE])
	if float(near) > MAX_NEAR_SHARE * float(sdf_hits):
		result.append("%d of %d SDF hits are within two voxels of the origin (more than %.2f)" % [near, sdf_hits, MAX_NEAR_SHARE])
	if screen_hits <= 0:
		result.append("the screen trace found no hits")
	if int(wrc_best.get("wrc_rays", 0)) <= 0:
		result.append("the software WRC traced no probe rays")
	if int(gather_best.get("wrc_gather_samples", 0)) <= 0:
		result.append("the gather took no sample from the WRC")
	if wrc_clean_best <= 0:
		result.append("the software WRC found no clean SDF hit")
	if spread < MIN_SPREAD:
		result.append("the denoised ambient is uniform (%.3f of samples differ, expected at least %.2f)" % [spread, MIN_SPREAD])
	return result


func _finish(p_failures: PackedStringArray) -> void:
	done = true
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "swrt_gi",
		"mode": mode,
		"pass": p_failures.is_empty(),
		"expected_tier": expected_tier,
		"tier": info.get("tier", ""),
		"quality": info.get("quality", ""),
		"failures": p_failures,
	}))
	get_tree().quit(0 if p_failures.is_empty() else 1)
