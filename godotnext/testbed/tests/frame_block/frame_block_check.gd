extends Node

## NextGI frame block check (#26; plan §4.1 and §6.1).
##
## Loads Color Box Room A into a 1920x1080 SubViewport, turns nextgi_enabled on in its Environment
## with SSAO, SSR and SSIL off, keeps the omni shadow on, adds a shadowed DirectionalLight3D and a
## ReflectionProbe that updates every frame. It then reads the RenderingDevice timestamps of every
## frame (run Godot with --gpu-profile) and, after the opaque pass, the render buffers.
##
## run_frame_block_check.sh writes res://override.cfg (git ignores it) with nextgi/enabled=true for
## the run and removes it afterwards:
##   godotnext/testbed/tests/frame_block/run_frame_block_check.sh [--half-resolution] [--hwrt=auto|off|force] \
##     [--quality=0|1|2|3] [--disabled] -- <godot> --path godotnext/testbed --rendering-driver <driver> \
##     --gpu-profile res://tests/frame_block/frame_block_check.tscn -- --expect=active|inactive [--frames=600] [--sdfgi]
##
## --expect=active (an active tier: HWRT, or SWRT since #186 feeds the block on the software
## tier, plan §21.2.4), on every checked frame:
## - exactly one "> NextGI" and the 12 NextGI timestamps right after it, in PASS_SLOTS order, and
##   no other NextGI timestamp (so no "NextGI SDF *" stamp: the software tier's work sits in the
##   slots, and those stamps belong to the verification kernels of readback runs);
## - no timestamp containing "Render GI" (process_gi skipped), and with --sdfgi, which turns the
##   Environment's own SDFGI on, no "Render SDFGI";
## - reflection-probe faces render in some frames and never add a NextGI block;
## after the run: the normal-roughness prepass texture exists (the forced prepass with SSAO, SSR,
## SSIL and SDFGI off) and RB_TEX_AMBIENT is 1920x1080, or 960x540 under gi/use_half_resolution.
## No frame block changed tier inside its frame (mid_frame_tier_changes 0). Per tier, from
## RenderingServer.nextgi_get_tier_info():
## - HWRT: the RT scene runs inside the block's AS slots (#243): by the end it holds TLAS instances
##   and has rebuilt the TLAS, and the result reports the widest GPU time each AS slot took
##   between its timestamp and the next one (tracked on warmup frames too, when the static
##   scene's BLASes build);
## - SWRT: the software tracer holds at least 3 global-SDF levels and the room's instances, its
##   global SDF was updated in the "NextGI: AS updates" slot (line 1a), no ray-query ShaderRD was
##   created since the tier was selected (and none in the whole process when hardware_ray_tracing
##   was Off at launch, which skips the startup self-test), and outside Lumen Lite the gather ran
##   the software sibling (gather_tier SWRT).
## The block's DEV_ASSERT traps a dev_build binary. Other builds print an error and skip the block,
## which fails the timestamp check.
##
## --expect=inactive (DISABLED): the frame block never runs. The tier comes from
## RenderingServer.nextgi_get_tier_info(); an active tier fails the run. On every checked frame no
## NextGI timestamp. After the run, from nextgi_get_tier_info() rather than the timestamps: the
## block never ran. Only the block creates the SRC relight, so src_relight_max_texels must be
## absent, and a run whose profile lost the block's stamps still fails. Without --sdfgi, nothing
## allocated RB_TEX_AMBIENT; with --sdfgi, SDFGI ran ("Render SDFGI"), as upstream.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.

const ProbeScript := preload("res://tests/frame_block/frame_block_probe.gd")
const ROOM_SCENE := "res://scenes/color_box_room/cbr_a.tscn"
const VIEW_SIZE := Vector2i(1920, 1080)
const WARMUP_FRAMES := 30
const SLOT_NAMES: Array[String] = [
	"> NextGI",
	"NextGI: SRC capture",
	"NextGI: AS updates",
	"NextGI: AS builds",
	"NextGI: SRC relight",
	"NextGI: WRC update",
	"NextGI: Screen traces",
	"NextGI: Diffuse gather",
	"NextGI: Reflection traces",
	"NextGI: Integration",
	"NextGI: Denoise",
	"< NextGI",
]

var expect := ""
var frames := 600
var sdfgi := false
var tier := ""
var probe: CompositorEffect
var process_frames := 0
var checked_frames := 0
var probe_frames := 0
var render_gi_frames := 0
var sdfgi_frames := 0
var nextgi_frames := 0
var as_updates_max_gpu_ns := 0
var as_builds_max_gpu_ns := 0
var seen_stamp_frames := {}
var failures := PackedStringArray()
var done := false
var hwrt_off_at_launch := false


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--expect="):
			expect = arg.trim_prefix("--expect=")
		elif arg.begins_with("--frames="):
			frames = int(arg.trim_prefix("--frames="))
		elif arg == "--sdfgi":
			sdfgi = true
	if expect != "active" and expect != "inactive":
		_finish(["--expect=active or --expect=inactive is required"])
		return
	# The tier is chosen at RenderingDevice init, before the scene loads.
	hwrt_off_at_launch = int(ProjectSettings.get_setting("rendering/global_illumination/nextgi/hardware_ray_tracing", 0)) == 1
	var tier_info: Dictionary = RenderingServer.nextgi_get_tier_info()
	tier = String(tier_info.get("tier", ""))
	_build_scene()
	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)


func _build_scene() -> void:
	var viewport := SubViewport.new()
	viewport.size = VIEW_SIZE
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(viewport)

	var room: Node3D = load(ROOM_SCENE).instantiate()
	viewport.add_child(room)

	var world_environment: WorldEnvironment = room.get_node("WorldEnvironment")
	var environment: Environment = world_environment.environment.duplicate()
	environment.nextgi_enabled = true
	environment.ssao_enabled = false
	environment.ssr_enabled = false
	environment.ssil_enabled = false
	environment.sdfgi_enabled = sdfgi
	world_environment.environment = environment

	var omni: OmniLight3D = room.get_node("RoomLight")
	omni.shadow_enabled = true
	var sun := DirectionalLight3D.new()
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-50.0, 30.0, 0.0)
	room.add_child(sun)

	var reflection_probe := ReflectionProbe.new()
	reflection_probe.update_mode = ReflectionProbe.UPDATE_ALWAYS
	reflection_probe.size = Vector3(4.0, 3.0, 4.0)
	reflection_probe.position = Vector3(-2.05, 1.5, 0.0)
	room.add_child(reflection_probe)

	probe = ProbeScript.new()
	var compositor := Compositor.new()
	compositor.compositor_effects = [probe]
	var camera: Camera3D = room.get_node("CaptureCam")
	camera.compositor = compositor
	camera.current = true


func _process(_delta: float) -> void:
	process_frames += 1
	# A safety cap: timestamps arrive a few frames late and only while --gpu-profile is on.
	if not done and process_frames > WARMUP_FRAMES + frames * 2 + 120:
		_finish(["only %d of %d frames had timestamps; run Godot with --gpu-profile" % [checked_frames, frames]])


func _on_frame_post_draw() -> void:
	if done:
		return
	var device := RenderingServer.get_rendering_device()
	var count := int(device.get_captured_timestamps_count())
	if count == 0:
		return
	var stamp_frame := int(device.get_captured_timestamps_frame())
	if seen_stamp_frames.has(stamp_frame):
		return
	seen_stamp_frames[stamp_frame] = true
	var names := PackedStringArray()
	var times := PackedInt64Array()
	for index in count:
		names.append(device.get_captured_timestamp_name(index))
		times.append(int(device.get_captured_timestamp_gpu_time(index)))
	_track_as_gpu_times(names, times)
	if process_frames <= WARMUP_FRAMES:
		return
	_check_frame(names)
	checked_frames += 1
	if checked_frames >= frames:
		_finish(_final_failures())


func _track_as_gpu_times(names: PackedStringArray, times: PackedInt64Array) -> void:
	# A slot's GPU time is the delta to the next timestamp. SLOT_NAMES offsets from
	# "> NextGI": AS updates at +2, AS builds at +3, the following slot at +4.
	var start := names.find("> NextGI")
	if start < 0 or start + 4 >= names.size():
		return
	if names[start + 2] != "NextGI: AS updates" or names[start + 3] != "NextGI: AS builds":
		return
	as_updates_max_gpu_ns = maxi(as_updates_max_gpu_ns, times[start + 3] - times[start + 2])
	as_builds_max_gpu_ns = maxi(as_builds_max_gpu_ns, times[start + 4] - times[start + 3])


func _check_frame(names: PackedStringArray) -> void:
	var begins := 0
	var nextgi_stamps := 0
	var probe_faces := 0
	var render_gi := false
	var render_sdfgi := false
	for name in names:
		if name == "> NextGI":
			begins += 1
		if name.contains("NextGI"):
			nextgi_stamps += 1
		if name.begins_with("Render ReflectionProbe, Face"):
			probe_faces += 1
		if name.contains("Render GI"):
			render_gi = true
		if name.begins_with("Render SDFGI"):
			render_sdfgi = true
	if probe_faces > 0:
		probe_frames += 1
	if render_gi:
		render_gi_frames += 1
	if render_sdfgi:
		sdfgi_frames += 1
	if nextgi_stamps > 0:
		nextgi_frames += 1

	if expect == "inactive":
		if nextgi_stamps > 0:
			_fail("NextGI timestamps on the %s tier: %s" % [tier, _nextgi_names(names)])
		return

	# One main view, so exactly one block, also in frames that render reflection-probe faces.
	if begins != 1:
		_fail("%d NextGI blocks in a frame (%d reflection-probe faces), expected 1" % [begins, probe_faces])
		return
	var start := names.find("> NextGI")
	for slot in SLOT_NAMES.size():
		var index := start + slot
		if index >= names.size() or names[index] != SLOT_NAMES[slot]:
			_fail("NextGI timestamps out of order: %s" % [_nextgi_names(names)])
			return
	if nextgi_stamps != SLOT_NAMES.size():
		_fail("%d NextGI timestamps in a frame, expected %d: %s" % [nextgi_stamps, SLOT_NAMES.size(), _nextgi_names(names)])
	for name in names:
		if name.begins_with("NextGI SDF"):
			_fail("a software-trace timestamp outside the slots in a %s frame: %s" % [tier, name])
			break
	if render_gi:
		_fail("\"Render GI\" is in the frame profile of a NextGI view")
	if render_sdfgi:
		_fail("SDFGI ran on the %s tier for an Environment with nextgi_enabled (\"Render SDFGI\")" % [tier])


func _nextgi_names(names: PackedStringArray) -> String:
	var found := PackedStringArray()
	for name in names:
		if name.contains("NextGI"):
			found.append(name)
	return ", ".join(found)


func _fail(message: String) -> void:
	# Keep the first occurrence of each failure, so a 600-frame run stays readable.
	if not failures.has(message):
		failures.append(message)


func _expected_ambient_size() -> Vector2i:
	var half: bool = ProjectSettings.get_setting("rendering/global_illumination/gi/use_half_resolution", false)
	return Vector2i(VIEW_SIZE.x >> 1, VIEW_SIZE.y >> 1) if half else VIEW_SIZE


func _final_failures() -> PackedStringArray:
	var result := failures.duplicate()
	if expect == "inactive":
		var tier_info: Dictionary = RenderingServer.nextgi_get_tier_info()
		if tier != "DISABLED":
			result.append("the tier is %s, expected DISABLED" % [tier])
		# NextGI::render_frame() is the only place that creates the SRC relight, which then reports
		# its keys for the rest of the run. The tier cannot reach the RT scene or ray-query shaders
		# here, so their counters would stay 0 even if the block ran.
		if tier_info.has("src_relight_max_texels"):
			result.append("the frame block ran on the %s tier (the SRC relight it creates reports src_relight_max_texels=%d)" % [tier, int(tier_info.get("src_relight_max_texels", 0))])
		if tier == "DISABLED" and not sdfgi and probe.ambient_size != Vector2i.ZERO:
			result.append("RB_TEX_AMBIENT is %s on the DISABLED tier, expected none" % [probe.ambient_size])
		# With the project setting off, SDFGI follows the Environment as upstream.
		if tier == "DISABLED" and sdfgi and sdfgi_frames == 0:
			result.append("SDFGI never ran on the DISABLED tier with --sdfgi (no \"Render SDFGI\" timestamp)")
	if expect == "active":
		if probe_frames == 0:
			result.append("no frame rendered a reflection-probe face")
		if probe.calls == 0:
			result.append("the compositor probe never ran")
		elif not probe.normal_roughness:
			result.append("no normal-roughness prepass texture")
		if probe.ambient_size != _expected_ambient_size():
			result.append("RB_TEX_AMBIENT is %s, expected %s" % [probe.ambient_size, _expected_ambient_size()])
		var tier_info: Dictionary = RenderingServer.nextgi_get_tier_info()
		if int(tier_info.get("mid_frame_tier_changes", -1)) != 0:
			result.append("%d frame blocks changed tier inside their frame" % [int(tier_info.get("mid_frame_tier_changes", -1))])
		if tier == "HWRT":
			# The RT scene's only caller is the frame block's AS slots (#243), so its
			# counters prove the AS work ran inside them.
			if int(tier_info.get("rt_instance_count", 0)) == 0:
				result.append("the RT scene held no TLAS instances inside the frame block")
			if int(tier_info.get("rt_tlas_build_count", 0)) == 0:
				result.append("the frame block never rebuilt the TLAS (AS updates slot)")
		elif tier == "SWRT":
			# The software tracer's only caller outside readback runs is the AS-updates slot.
			if int(tier_info.get("swrt_trace_levels", 0)) < 3:
				result.append("the software tracer holds %d global-SDF levels, expected at least 3" % [int(tier_info.get("swrt_trace_levels", 0))])
			if int(tier_info.get("swrt_trace_instances", 0)) == 0:
				result.append("the software tracer holds no instances")
			if String(tier_info.get("swrt_update_slot", "none")) != "NextGI: AS updates":
				result.append("the global SDF was updated in slot \"%s\", expected \"NextGI: AS updates\"" % [String(tier_info.get("swrt_update_slot", "none"))])
			if int(tier_info.get("ray_query_shader_versions_since_tier", -1)) != 0:
				result.append("%d ray-query ShaderRD versions were created on the SWRT tier" % [int(tier_info.get("ray_query_shader_versions_since_tier", -1))])
			if hwrt_off_at_launch and int(tier_info.get("ray_query_shader_versions", -1)) != 0:
				result.append("%d ray-query ShaderRD versions were created in a process started with hardware_ray_tracing Off" % [int(tier_info.get("ray_query_shader_versions", -1))])
			if not bool(tier_info.get("lite_beta", false)) and String(tier_info.get("gather_tier", "")) != "SWRT":
				result.append("the gather ran the %s shader on the SWRT tier" % [String(tier_info.get("gather_tier", ""))])
		else:
			result.append("the tier is %s, expected HWRT or SWRT" % [tier])
	return result


func _finish(final_failures: PackedStringArray) -> void:
	done = true
	var tier_info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var result := {
		"pass": final_failures.is_empty(),
		"expect": expect,
		"device": RenderingServer.get_video_adapter_name(),
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"tier_info": tier_info,
		"sdfgi": sdfgi,
		"half_resolution": ProjectSettings.get_setting("rendering/global_illumination/gi/use_half_resolution", false),
		"frames": checked_frames,
		"nextgi_frames": nextgi_frames,
		"as_updates_max_gpu_ns": as_updates_max_gpu_ns,
		"as_builds_max_gpu_ns": as_builds_max_gpu_ns,
		"reflection_probe_frames": probe_frames,
		"render_gi_frames": render_gi_frames,
		"sdfgi_frames": sdfgi_frames,
		"internal_size": probe.internal_size if probe != null else Vector2i.ZERO,
		"normal_roughness": probe.normal_roughness if probe != null else false,
		"ambient_size": probe.ambient_size if probe != null else Vector2i.ZERO,
		"failures": final_failures,
	}
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if final_failures.is_empty() else 1)
