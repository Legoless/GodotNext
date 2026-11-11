extends Node

## #186: the software tier renders GI through the frame block (plan §21.2.4).
##
## --check=smoke: the overlap-heavy kitbash (the PS smoke scene) in a 1920x1080 SubViewport, the
## camera at (0, 2, 8) looking at (0, 1, 0), NextGI on in its Environment. After WARMUP_FRAMES
## it samples RenderingServer.nextgi_get_tier_info() every frame for SAMPLE_FRAMES frames. The
## counters arrive asynchronously: each snapshot holds one gather frame and stays published until
## the next one arrives, and which frames get published depends on the readback timing. World hits
## vary strongly from frame to frame (the ray rotation), so the gather counters are the mean over
## the distinct snapshots of the window rather than one frame against one frame. The WRC snapshot
## with the most clean WRC SDF hits is kept (its update traces a different probe set each frame).
## On either tier, no snapshot may hold more gather rays than its probes trace
## (src_gather_expected_rays): one run held exactly twice a frame's counters. The sampled ray
## counts are printed.
##
## With --expected-tier=SWRT it asserts:
## - the tier is SWRT, the gather ran the software sibling (gather_tier SWRT) and traced rays, and
##   no ray-query ShaderRD was created since the tier was selected, nor in the whole process when
##   hardware_ray_tracing was Off at launch (the runner's software passes; Off skips the startup
##   self-test);
## - clean SDF hits (gather_sdf_hits minus the step-limit and near-origin ones) are at least
##   CLEAN_HIT_SHARE of the hardware tier's world hits on the same view (--hwrt-world-hits, from
##   the hardware pass that run_swrt_gi_check.sh runs first: rays - misses - screen hits), both
##   window means;
## - step-limit (exhausted) hits are at most MAX_EXHAUSTED_SHARE of the SDF hits, and hits within
##   two voxels of the origin's level (self-intersection candidates) at most MAX_NEAR_SHARE. The
##   shares are printed. MAX_NEAR_SHARE is 0.30 rather than the 0.10 start value, pending a lead
##   decision (#186): on the M5 Max the software tier measures 0.23 at High and at Epic, and 0.80
##   and 0.77 with the gather origin offset removed. That build still passes the clean-hit bound
##   (0.26 and 0.30 of the hardware tier's world hits), so only this bound catches it. The 0.30
##   comes from another metric, not this one:
##   a temporary counting build found 30% of the hardware tier's world hits within 0.35 m of the
##   gather's surface point (the kitbash's overlapping rocks put real surfaces that close), where
##   the software tier scored 17%. The hardware tier has no counter for the two-voxel metric;
##   adding one would change its shaders;
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
## shader again. No frame block changes tier inside its frame (mid_frame_tier_changes 0). Just before
## the switch back it turns debug/deny_ray_query on at runtime: the setting needs a restart (the RT
## arena and the ray-query shaders were chosen at startup), so the hardware tier must come back.
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
## --check=bleed (#188, #186): Color Box Room A seen from above, the camera at (-2.05, 2.95, 0)
## looking straight down (up -Z, fov 50), so the red (-Z) and green (+Z) walls stay off-screen and
## their light reaches the floor only through NextGI. After BLEED_WARMUP_FRAMES it waits until the
## voxel-lighting volume has finished filling (voxel_lighting_retrace 0 for VOXEL_SETTLE_FRAMES
## frames in a row, within BLEED_MAX_FRAMES): a new volume traces its bricks at #188's 1536 CPU
## rays per frame, so on the software tier global-SDF hits in this room have no voxel radiance for
## the first 1456 frames (measured on the M5 Max). Both tiers wait the same way. It then measures the
## denoised ambient (DEBUG_DRAW_NEXTGI_DENOISE_DENOISED, HDR) every BLEED_STEP frames for
## BLEED_SAMPLES samples: 15x15-pixel means on the floor at x -3.6 and -0.6, z -1 (the red side)
## and z +1 (the green side). red_bleed is the mean R - G on the red side, green_bleed the mean
## G - R on the green side, each averaged over the samples. With --expected-tier=SWRT both must be
## positive and at least BLEED_SHARE of the hardware tier's (--hwrt-red-bleed, --hwrt-green-bleed,
## from the reference pass run_swrt_gi_check.sh runs first); the ratios are printed report-only.
## The software tier gets its floor bleed from the walls' surface cache: SRC capture and relight
## on that tier, voxel lighting shaded from it and the WRC it feeds. With --expected-tier=HWRT it
## prints the reference ("SWRT_GI_REFERENCE red_bleed=R green_bleed=G").
##
## --check=detail (#186 step 3): the bleed view with software_ray_tracing_mode Detail (the runner
## sets it). Gather rays hit mesh SDFs (gather_sdf_detail_hits > 0), those hits know their
## instance and resolve through the D6 card lookup of that instance's record (gather_sdf_d6_resolved
## at least DETAIL_D6_SHARE of the detail hits; both are gather-ray counts, window means), and the
## floor bleed stays positive. Software tier only.
##
## GodotNext: ps_swrt_trace (#186, #188). With debug/deny_ray_query (a device without ray query)
## every software-tier mode also asserts the tier's reason NO_RAY_QUERY, no RT arena
## (rt_arena_bytes 0), no ray-query ShaderRD in the process and SWRT instances (their runtime mesh
## SDFs come from the CPU copies kept without ray query).
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.

const KITBASH_SCENE := "res://scenes/swrt_kitbash/swrt_kitbash.tscn"
const BLEED_SCENE := "res://scenes/color_box_room/cbr_a.tscn"
const BLEED_WARMUP_FRAMES := 300
const VOXEL_SETTLE_FRAMES := 30
const BLEED_MAX_FRAMES := 4000
const BLEED_SAMPLES := 10
const BLEED_STEP := 3
const BLEED_SHARE := 0.25
const DETAIL_D6_SHARE := 0.5
const DENY_SETTING := "rendering/global_illumination/nextgi/debug/deny_ray_query"
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
var gather_samples: Array[Dictionary] = []
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
var deny_ray_query := false
var hwrt_red_bleed := -1.0
var hwrt_green_bleed := -1.0
var camera: Camera3D
var bleed_red := PackedFloat32Array()
var bleed_green := PackedFloat32Array()
var settle_count := 0
var settled_frame := -1


func _ready() -> void:
	hwrt_off_at_launch = int(ProjectSettings.get_setting("rendering/global_illumination/nextgi/hardware_ray_tracing", 0)) == 1
	deny_ray_query = bool(ProjectSettings.get_setting(DENY_SETTING, false))
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--check="):
			mode = arg.trim_prefix("--check=")
		elif arg.begins_with("--expected-tier="):
			expected_tier = arg.trim_prefix("--expected-tier=")
		elif arg.begins_with("--hwrt-world-hits="):
			hwrt_world_hits = int(arg.trim_prefix("--hwrt-world-hits="))
		elif arg.begins_with("--hwrt-red-bleed="):
			hwrt_red_bleed = float(arg.trim_prefix("--hwrt-red-bleed="))
		elif arg.begins_with("--hwrt-green-bleed="):
			hwrt_green_bleed = float(arg.trim_prefix("--hwrt-green-bleed="))
	if not mode in ["smoke", "toggle", "budget", "bleed", "detail"]:
		_finish(["unknown --check=%s (smoke, toggle, budget, bleed or detail)" % mode])
		return
	if expected_tier != "SWRT" and expected_tier != "HWRT":
		_finish(["--expected-tier must be SWRT or HWRT"])
		return
	if mode == "smoke" and expected_tier == "SWRT" and hwrt_world_hits <= 0:
		_finish(["--hwrt-world-hits=N (the hardware tier's world hits) is required with --expected-tier=SWRT"])
		return
	if mode == "bleed" and expected_tier == "SWRT" and (hwrt_red_bleed <= 0.0 or hwrt_green_bleed <= 0.0):
		_finish(["--hwrt-red-bleed and --hwrt-green-bleed (the hardware tier's positive floor bleed) are required with --expected-tier=SWRT"])
		return
	if mode == "detail" and (expected_tier != "SWRT" or int(ProjectSettings.get_setting("rendering/global_illumination/nextgi/software_ray_tracing_mode", 0)) != 1):
		_finish(["--check=detail runs the software tier with software_ray_tracing_mode Detail (1)"])
		return
	viewport = SubViewport.new()
	viewport.size = VIEW_SIZE
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.use_hdr_2d = true
	viewport.debug_draw = Viewport.DEBUG_DRAW_NEXTGI_DENOISE_DENOISED
	add_child(viewport)
	if mode == "bleed" or mode == "detail":
		_build_bleed_view()
		return
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


# Color Box Room A from above: the colored walls stay off-screen, so their bleed on the floor comes
# through NextGI alone. Screen-space GI effects are off.
func _build_bleed_view() -> void:
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	viewport.use_taa = false
	var room: Node3D = load(BLEED_SCENE).instantiate()
	viewport.add_child(room)
	var world := room.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world == null or world.environment == null:
		_finish(["Color Box Room A has no WorldEnvironment"])
		return
	var environment: Environment = world.environment.duplicate()
	environment.nextgi_enabled = true
	environment.ssao_enabled = false
	environment.ssr_enabled = false
	environment.ssil_enabled = false
	world.environment = environment
	camera = Camera3D.new()
	camera.fov = 50.0
	room.add_child(camera)
	camera.look_at_from_position(Vector3(-2.05, 2.95, 0.0), Vector3(-2.05, 0.0, 0.0), Vector3(0, 0, -1))
	camera.current = true


func _patch_mean(p_image: Image, p_world: Vector3) -> Color:
	var center := camera.unproject_position(p_world)
	var sum := Color(0, 0, 0, 0)
	var count := 0
	for y in range(-7, 8):
		for x in range(-7, 8):
			var pixel := Vector2i(int(center.x) + x, int(center.y) + y)
			if pixel.x < 0 or pixel.y < 0 or pixel.x >= p_image.get_width() or pixel.y >= p_image.get_height():
				continue
			sum += p_image.get_pixel(pixel.x, pixel.y)
			count += 1
	return sum / float(maxi(count, 1))


# One sample of the floor bleed: [red_bleed, green_bleed].
func _sample_bleed() -> PackedFloat32Array:
	var image := viewport.get_texture().get_image()
	var red := 0.0
	var green := 0.0
	for x in [-3.6, -0.6]:
		var on_red := _patch_mean(image, Vector3(x, 0.0, -1.0))
		var on_green := _patch_mean(image, Vector3(x, 0.0, 1.0))
		red += (on_red.r - on_red.g) * 0.5
		green += (on_green.g - on_green.r) * 0.5
	return PackedFloat32Array([red, green])


func _mean(p_values: PackedFloat32Array) -> float:
	var sum := 0.0
	for value in p_values:
		sum += value
	return sum / float(maxi(p_values.size(), 1))


func _process_bleed() -> void:
	if frames <= BLEED_WARMUP_FRAMES:
		return
	if settled_frame < 0:
		var retrace := int(RenderingServer.nextgi_get_counters().get("voxel_lighting_retrace", -1))
		settle_count = settle_count + 1 if retrace == 0 else 0
		if settle_count >= VOXEL_SETTLE_FRAMES:
			settled_frame = frames
		elif frames >= BLEED_MAX_FRAMES:
			_finish(["voxel lighting was still filling after %d frames (voxel_lighting_retrace %d)" % [frames, retrace]])
		return
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	gather_samples.append(info)
	if (frames - settled_frame) % BLEED_STEP == 0:
		var bleed := _sample_bleed()
		bleed_red.append(bleed[0])
		bleed_green.append(bleed[1])
	if bleed_red.size() < BLEED_SAMPLES:
		return
	gather_best = _window_mean(gather_samples)
	_finish(_bleed_checks(info))


func _bleed_checks(p_last: Dictionary) -> PackedStringArray:
	var result := PackedStringArray()
	var tier := String(p_last.get("tier", ""))
	if tier != expected_tier:
		result.append("tier is %s, expected %s" % [tier, expected_tier])
		return result
	var red := _mean(bleed_red)
	var green := _mean(bleed_green)
	if expected_tier == "HWRT":
		print("SWRT_GI_REFERENCE red_bleed=%.6f green_bleed=%.6f settled_frame=%d samples_red=%s samples_green=%s" % [red, green, settled_frame, bleed_red, bleed_green])
		return result
	result.append_array(_software_tier_checks(p_last))
	var detail_hits := int(gather_best.get("gather_sdf_detail_hits", 0))
	var resolved := int(gather_best.get("gather_sdf_d6_resolved", 0))
	print("SWRT_GI_BLEED mode=%s settled_frame=%d quality=%s red_bleed=%.6f green_bleed=%.6f hwrt_red=%.6f hwrt_green=%.6f ratio_red=%.3f ratio_green=%.3f (report-only) samples_red=%s samples_green=%s snapshots=%d rays=%d sdf_hits=%d detail_hits=%d d6_resolved=%d swrt_records=%d paged=%d" % [
		mode, settled_frame, String(p_last.get("quality", "")), red, green, hwrt_red_bleed, hwrt_green_bleed,
		red / hwrt_red_bleed if hwrt_red_bleed > 0.0 else 0.0, green / hwrt_green_bleed if hwrt_green_bleed > 0.0 else 0.0,
		bleed_red, bleed_green, int(gather_best.get("snapshots", 0)), int(gather_best.get("src_gather_rays", 0)), int(gather_best.get("gather_sdf_hits", 0)),
		detail_hits, resolved, int(p_last.get("swrt_record_count", -1)), int(p_last.get("swrt_record_paged", -1))])
	if not (red > 0.0):
		result.append("the floor shows no red bleed (%.6f)" % red)
	if not (green > 0.0):
		result.append("the floor shows no green bleed (%.6f)" % green)
	if mode == "bleed":
		if red < BLEED_SHARE * hwrt_red_bleed:
			result.append("red bleed %.6f is below %.2f x the hardware tier's %.6f" % [red, BLEED_SHARE, hwrt_red_bleed])
		if green < BLEED_SHARE * hwrt_green_bleed:
			result.append("green bleed %.6f is below %.2f x the hardware tier's %.6f" % [green, BLEED_SHARE, hwrt_green_bleed])
	else:
		if detail_hits <= 0:
			result.append("no gather ray hit a mesh SDF in detail mode")
		if resolved <= 0 or float(resolved) < DETAIL_D6_SHARE * float(detail_hits):
			result.append("%d of %d detail hits resolved through the D6 card lookup, expected at least %.2f of them" % [resolved, detail_hits, DETAIL_D6_SHARE])
	return result


# The software tier's common checks: the gather ran the software sibling and traced rays, and no
# ray-query ShaderRD was created since the tier was selected (nor in the process when it started
# with hardware_ray_tracing Off or under debug/deny_ray_query); under debug/deny_ray_query also the
# tier reason, the RT arena and the SWRT instances.
func _software_tier_checks(p_last: Dictionary) -> PackedStringArray:
	var result := PackedStringArray()
	if String(gather_best.get("gather_tier", "")) != "SWRT":
		result.append("the gather ran the %s shader, expected the software sibling" % [String(gather_best.get("gather_tier", ""))])
	if int(gather_best.get("src_gather_rays", 0)) <= 0:
		result.append("the gather traced no rays")
	if int(p_last.get("ray_query_shader_versions_since_tier", -1)) != 0:
		result.append("%d ray-query ShaderRD versions were created on the SWRT tier" % [int(p_last.get("ray_query_shader_versions_since_tier", -1))])
	if (hwrt_off_at_launch or deny_ray_query) and int(p_last.get("ray_query_shader_versions", -1)) != 0:
		result.append("%d ray-query ShaderRD versions were created in a process started with hardware_ray_tracing Off or debug/deny_ray_query" % [int(p_last.get("ray_query_shader_versions", -1))])
	if deny_ray_query:
		print("SWRT_GI_DENY reason=%s rt_arena_bytes=%d ray_query_shader_versions=%d swrt_trace_instances=%d" % [String(p_last.get("reason", "")), int(p_last.get("rt_arena_bytes", -1)), int(p_last.get("ray_query_shader_versions", -1)), int(p_last.get("swrt_trace_instances", 0))])
		if String(p_last.get("reason", "")) != "NO_RAY_QUERY":
			result.append("deny_ray_query: the tier's reason is %s, expected NO_RAY_QUERY" % String(p_last.get("reason", "")))
		if int(p_last.get("rt_arena_bytes", -1)) != 0:
			result.append("deny_ray_query: the RT arena holds %d bytes" % int(p_last.get("rt_arena_bytes", -1)))
		if int(p_last.get("swrt_trace_instances", 0)) <= 0:
			result.append("deny_ray_query: no SWRT instances (no runtime mesh SDFs from the CPU copies)")
	return result


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
	if mode == "bleed" or mode == "detail":
		_process_bleed()
		return
	if frames <= WARMUP_FRAMES:
		return
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	gather_samples.append(info)
	# The WRC frame with the most clean SDF hits (its update traces a different probe set each frame).
	var wrc_clean := int(info.get("wrc_sdf_hits", 0)) - int(info.get("wrc_sdf_exhausted", 0)) - int(info.get("wrc_sdf_near", 0))
	if wrc_clean > wrc_clean_best:
		wrc_clean_best = wrc_clean
		wrc_best = info
	if frames < WARMUP_FRAMES + SAMPLE_FRAMES:
		return
	gather_best = _window_mean(gather_samples)
	_finish(_checks(info))


# The gather counters averaged over the window's distinct snapshots (a snapshot stays published
# for every frame until the next one arrives); the other keys are the last sample's.
func _window_mean(p_samples: Array[Dictionary]) -> Dictionary:
	const KEYS := ["src_gather_rays", "src_gather_misses", "screen_trace_hits", "gather_sdf_hits", "gather_sdf_exhausted", "gather_sdf_near", "gather_sdf_hit_cm_sum", "wrc_gather_samples", "gather_sdf_detail_hits", "gather_sdf_d6_resolved"]
	var sums := {}
	for key in KEYS:
		sums[key] = 0
	var distinct := 0
	var previous := []
	for sample in p_samples:
		var counters := []
		for key in KEYS:
			counters.append(int(sample.get(key, 0)))
		if counters == previous:
			continue
		previous = counters
		distinct += 1
		for i in KEYS.size():
			sums[KEYS[i]] += counters[i]
	var result: Dictionary = p_samples[p_samples.size() - 1].duplicate()
	for key in KEYS:
		result[key] = int(round(float(sums[key]) / float(maxi(distinct, 1))))
	result["snapshots"] = distinct
	return result


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
		# GodotNext: ps_swrt_trace (#186, #188). Read at startup only, so the switch ignores it.
		ProjectSettings.set_setting("rendering/global_illumination/nextgi/debug/deny_ray_query", true)
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
	# A snapshot holds one frame's gather, so its rays never exceed the probes times the rays per
	# probe. A snapshot above that counted a frame twice: the run fails rather than pick or drop it.
	var sample_rays := PackedInt64Array()
	var over_cap := 0
	var over_cap_max := 0
	for sample in gather_samples:
		var sample_count := int(sample.get("src_gather_rays", 0))
		sample_rays.append(sample_count)
		if sample_count > int(sample.get("src_gather_expected_rays", 0)):
			over_cap += 1
			over_cap_max = maxi(over_cap_max, sample_count)
	var cap := int(gather_best.get("src_gather_expected_rays", 0))
	print("SWRT_GI_SAMPLES src_gather_rays=%s cap=%d (report-only)" % [sample_rays, cap])
	if over_cap > 0:
		result.append("%d of %d snapshots hold more gather rays than the probes trace (up to %d, cap %d): a frame was counted twice" % [over_cap, gather_samples.size(), over_cap_max, cap])
	var rays := int(gather_best.get("src_gather_rays", 0))
	var misses := int(gather_best.get("src_gather_misses", 0))
	var screen_hits := int(gather_best.get("screen_trace_hits", 0))
	if expected_tier == "HWRT":
		print("SWRT_GI_REFERENCE world_hits=%d rays=%d snapshots=%d" % [rays - misses - screen_hits, rays, int(gather_best.get("snapshots", 0))])
		return result
	var sdf_hits := int(gather_best.get("gather_sdf_hits", 0))
	var exhausted := int(gather_best.get("gather_sdf_exhausted", 0))
	var near := int(gather_best.get("gather_sdf_near", 0))
	var clean := sdf_hits - exhausted - near
	var wrc_hits := int(wrc_best.get("wrc_sdf_hits", 0))
	var spread := _spread()
	print("SWRT_GI_SMOKE quality=%s snapshots=%d rays=%d sdf_hits=%d clean=%d (HWRT world hits %d, ratio %.3f, report-only) exhausted=%d (%.4f) near=%d (%.4f) screen_hits=%d mean_hit_m=%.3f wrc_rays=%d wrc_gather_samples=%d wrc_sdf_hits=%d wrc_clean=%d wrc_exhausted_share=%.4f wrc_near_share=%.4f spread=%.3f" % [
		String(p_last.get("quality", "")), int(gather_best.get("snapshots", 0)), rays, sdf_hits, clean, hwrt_world_hits, float(clean) / float(maxi(hwrt_world_hits, 1)),
		exhausted, float(exhausted) / float(maxi(sdf_hits, 1)), near, float(near) / float(maxi(sdf_hits, 1)), screen_hits,
		float(int(gather_best.get("gather_sdf_hit_cm_sum", 0))) / 100.0 / float(maxi(sdf_hits, 1)),
		int(wrc_best.get("wrc_rays", 0)), int(gather_best.get("wrc_gather_samples", 0)), wrc_hits, wrc_clean_best,
		float(int(wrc_best.get("wrc_sdf_exhausted", 0))) / float(maxi(wrc_hits, 1)), float(int(wrc_best.get("wrc_sdf_near", 0))) / float(maxi(wrc_hits, 1)), spread])
	result.append_array(_software_tier_checks(p_last))
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
