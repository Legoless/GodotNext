extends Node3D

## TSR async compute check for #193.
##
## Loads Upscaler Torture under TSR at scale 0.667 with the preset of --tsr-preset= (low, medium,
## high, epic or cinematic; default high), then steps rendering/anti_aliasing/tsr/async_compute
## through the levels of --levels= (comma-separated, default 0,1,2,3). At each level it waits
## SETTLE_FRAMES, then reads the main RenderingDevice's "async:<submitted>/<fell back>" frame
## counters (get_perf_report()) around WINDOW_FRAMES frames. TSR is the only engine user of the async
## queue, so the counters show which levels moved TSR passes there. It fails unless:
## - with async compute active (rendering/rendering_device/async_compute and the device's
##   SUPPORTS_ASYNC_COMPUTE), levels 0 and 1 submit no async frame, and levels 2 and 3 submit one
##   async frame for every rendered frame of the window (one frame of slack) with none falling back;
## - without it, no level submits or falls back;
## - every window renders a non-black image;
## - with --check-timestamps (run Godot with --gpu-profile, which captures the RenderingDevice
##   timestamps), the last captured frame of every window has, per TSR viewport, either one "TSR"
##   timestamp, or, where the passes are split (level 2 with async compute active, in the first TSR
##   viewport of the frame), one "TSR depth and velocity" and one "TSR critical path" after it and
##   no "TSR". "TSR" always covers all of TSR.
## The output itself is compared across levels by harness captures, not here.
##
## Variants (user arguments after "--"; they combine):
## - --thin: rendering/anti_aliasing/tsr/thin_geometry_detection on (the thin-geometry analysis).
## - --debug-view: Viewport.DEBUG_DRAW_TSR on the TSR viewport (the pass visualizer).
## - --resize: the TSR viewport's scaling_3d_scale switches between 0.667 and 0.5 every
##   RESIZE_PERIOD frames, so TSR's textures are freed, created and cleared inside the windows.
## - --two-viewports: a second TSR viewport (a SubViewport with its own camera) renders first, and
##   the main viewport samples its output on a quad in front of the camera. Only one TSR viewport of
##   an RD frame uses the async queue, so the frame still forks once and never falls back.
## - --xr: Upscaler Torture renders in a TSR SubViewport with use_xr through the MobileVR interface
##   (two views, multiview); the main viewport renders no 3D. The image check reads that viewport.
##   With slice-aware fork barriers (#300), multiview viewports follow the level setting like
##   single-view ones, so levels 2 and 3 fork one async frame per rendered frame with 0 fallbacks.
##   Needs xr/shaders/enabled=true in override.cfg (the multiview shader variants), as any XR project.
##
## Run it as a validation job, e.g. on the Linux L4 machine:
##   godot --path . --rendering-driver vulkan --gpu-validation --gpu-abort --resolution 1280x720 \
##     res://tests/tsr_async/tsr_async_check.tscn -- --tsr-preset=high --levels=0,1,2,3
## (add --gpu-profile before the scene and --check-timestamps after "--" for the timestamp check)
## with an override.cfg that sets rendering/rendering_device/async_compute=true for the async case.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const SceneArgs := preload("res://scenes/scene_args.gd")

const SCALE := 0.667
const RESIZE_SCALE := 0.5
const RESIZE_PERIOD := 5
const WARMUP_FRAMES := 20
const SETTLE_FRAMES := 4
const WINDOW_FRAMES := 30
const SECOND_VIEWPORT_SIZE := Vector2i(640, 360)
const QUAD_LAYER := 1 << 19
const LEVEL_SETTING := "rendering/anti_aliasing/tsr/async_compute"
const RD_SETTING := "rendering/rendering_device/async_compute"
const THIN_SETTING := "rendering/anti_aliasing/tsr/thin_geometry_detection"
const TSR_STAMP := "TSR"
const TSR_EARLY_STAMP := "TSR depth and velocity"
const TSR_CRITICAL_STAMP := "TSR critical path"

const PRESETS := {
	"low": Viewport.TSR_QUALITY_LOW,
	"medium": Viewport.TSR_QUALITY_MEDIUM,
	"high": Viewport.TSR_QUALITY_HIGH,
	"epic": Viewport.TSR_QUALITY_EPIC,
	"cinematic": Viewport.TSR_QUALITY_CINEMATIC,
}

var _failures := PackedStringArray()
var _check_timestamps := false
var _resize := false
var _two_viewports := false
var _xr := false
var _variants := PackedStringArray()
# The viewport whose TSR the variants change and whose image is checked.
var _tsr_viewport: Viewport
var _xr_interface: XRInterface


func _ready() -> void:
	var preset_name := SceneArgs.value("--tsr-preset", "high").to_lower()
	var user_args := OS.get_cmdline_user_args()
	_check_timestamps = user_args.has("--check-timestamps")
	_resize = user_args.has("--resize")
	_two_viewports = user_args.has("--two-viewports")
	_xr = user_args.has("--xr")
	for variant in ["--thin", "--debug-view", "--resize", "--two-viewports", "--xr"]:
		if user_args.has(variant):
			_variants.append(variant.trim_prefix("--"))
	var levels := PackedInt32Array()
	for token in SceneArgs.value("--levels", "0,1,2,3").split(","):
		if token.strip_edges().is_valid_int():
			levels.append(int(token))
	if not PRESETS.has(preset_name):
		_finish(preset_name, levels, false, {}, ["unknown --tsr-preset '%s'" % preset_name])
		return
	if levels.is_empty():
		_finish(preset_name, levels, false, {}, ["--levels lists no level"])
		return
	if _xr and _two_viewports:
		_finish(preset_name, levels, false, {}, ["--xr and --two-viewports do not combine"])
		return

	var packed: PackedScene = load("res://scenes/upscaler_torture/upscaler_torture.tscn")
	var scene := packed.instantiate()
	var error := _xr_setup(scene) if _xr else _flat_setup(scene)
	if not error.is_empty():
		_finish(preset_name, levels, false, {}, [error])
		return
	_set_tsr(_tsr_viewport, PRESETS[preset_name])
	if user_args.has("--thin"):
		ProjectSettings.set_setting(THIN_SETTING, true)
	if user_args.has("--debug-view"):
		_tsr_viewport.debug_draw = Viewport.DEBUG_DRAW_TSR
	_run(preset_name, levels)


# Upscaler Torture in the main viewport; with --two-viewports, a second TSR viewport whose output the
# main viewport samples.
func _flat_setup(scene: Node) -> String:
	add_child(scene)
	var camera := scene.get_node_or_null("CameraPath/CameraFollow/Camera3D") as Camera3D
	if camera == null:
		return "Upscaler Torture has no CameraPath/CameraFollow/Camera3D"
	camera.current = true
	_tsr_viewport = get_viewport()
	if not _two_viewports:
		return ""

	var second := SubViewport.new()
	second.size = SECOND_VIEWPORT_SIZE
	second.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(second)
	var second_camera := Camera3D.new()
	second_camera.cull_mask = camera.cull_mask & ~QUAD_LAYER
	second.add_child(second_camera)
	second_camera.global_transform = camera.global_transform
	second_camera.current = true
	_set_tsr(second, PRESETS[SceneArgs.value("--tsr-preset", "high").to_lower()])

	# The main viewport's opaque pass samples the second viewport's output, so the main viewport's
	# frame depends on the second one's. The second camera does not see the quad.
	var quad := MeshInstance3D.new()
	var mesh := QuadMesh.new()
	mesh.size = Vector2(0.32, 0.18)
	quad.mesh = mesh
	quad.layers = QUAD_LAYER
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_texture = second.get_texture()
	quad.material_override = material
	camera.add_child(quad)
	quad.position = Vector3(0.3, -0.18, -1.0)
	return ""


# Upscaler Torture in a TSR SubViewport with use_xr (MobileVR, two views); no 3D in the main viewport.
func _xr_setup(scene: Node) -> String:
	if not bool(ProjectSettings.get_setting("xr/shaders/enabled", false)):
		return "--xr needs xr/shaders/enabled=true in override.cfg"
	_xr_interface = XRServer.find_interface("Native mobile")
	if _xr_interface == null or not _xr_interface.initialize():
		return "the MobileVR interface is not available"
	var viewport := SubViewport.new()
	viewport.size = Vector2i(1280, 720)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.use_xr = true
	add_child(viewport)
	viewport.add_child(scene)
	var camera := scene.get_node_or_null("CameraPath/CameraFollow/Camera3D") as Camera3D
	if camera == null:
		return "Upscaler Torture has no CameraPath/CameraFollow/Camera3D"
	var origin := XROrigin3D.new()
	viewport.add_child(origin)
	origin.global_transform = camera.global_transform
	origin.position.y -= 1.85 # MobileVR's default eye height.
	var xr_camera := XRCamera3D.new()
	origin.add_child(xr_camera)
	xr_camera.current = true
	_tsr_viewport = viewport
	return ""


func _set_tsr(viewport: Viewport, quality: int) -> void:
	viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_TSR
	viewport.scaling_3d_scale = SCALE
	viewport.tsr_quality = quality


func _run(preset_name: String, levels: PackedInt32Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		_finish(preset_name, levels, false, {}, ["no RenderingDevice"])
		return
	var active := bool(ProjectSettings.get_setting(RD_SETTING, false)) and rd.has_feature(RenderingDevice.SUPPORTS_ASYNC_COMPUTE)
	# GodotNext: px_rd_async_multiview (#300). Multiview (XR) viewports now use the async queue like single-view ones.
	var tsr_async := active
	ProjectSettings.set_setting(LEVEL_SETTING, levels[0])
	for i in WARMUP_FRAMES:
		await RenderingServer.frame_post_draw

	var results := {}
	for level in levels:
		ProjectSettings.set_setting(LEVEL_SETTING, level)
		for i in SETTLE_FRAMES:
			await RenderingServer.frame_post_draw
		var before := _counters(rd)
		var frames_before := Engine.get_frames_drawn()
		var resizes := 0
		for i in WINDOW_FRAMES:
			if _resize and i % RESIZE_PERIOD == 0:
				_tsr_viewport.scaling_3d_scale = RESIZE_SCALE if is_equal_approx(_tsr_viewport.scaling_3d_scale, SCALE) else SCALE
				resizes += 1
			await RenderingServer.frame_post_draw
		var after := _counters(rd)
		# Before get_image(): reading the texture back stalls for every frame in flight, which
		# begins frames with no timestamps.
		var names := _timestamp_names(rd)
		var frames := Engine.get_frames_drawn() - frames_before
		var submitted := after[0] - before[0]
		var fell_back := after[1] - before[1]
		var lit := _lit_share(_viewport_image(_tsr_viewport))
		results[str(level)] = { "frames": frames, "submitted": submitted, "fell_back": fell_back, "lit": lit }
		if _resize:
			results[str(level)]["resizes"] = resizes

		var label := "level %d" % level
		if fell_back != 0:
			_failures.append("%s: %d frames fell back to the main queue" % [label, fell_back])
		if tsr_async and level >= 2:
			if submitted < frames - 1 or submitted > frames + 1:
				_failures.append("%s: %d async frames in %d rendered frames, expected one per frame" % [label, submitted, frames])
		elif submitted != 0:
			_failures.append("%s: %d async frames, expected none (async compute %s%s)" % [label, submitted, "active" if active else "inactive", ", multiview" if _xr else ""])
		if lit < 0.05:
			_failures.append("%s: the image is black (%.3f of the sampled pixels lit)" % [label, lit])
		if _check_timestamps:
			results[str(level)]["timestamps"] = [names.count(TSR_STAMP), names.count(TSR_EARLY_STAMP), names.count(TSR_CRITICAL_STAMP)]
			_check_tsr_timestamps(label, names, 2 if _two_viewports else 1, 1 if tsr_async and level == 2 else 0)

	_finish(preset_name, levels, active, results, [])


# The names of the last frame whose RenderingDevice timestamps were captured (--gpu-profile).
func _timestamp_names(rd: RenderingDevice) -> PackedStringArray:
	var names := PackedStringArray()
	for index in int(rd.get_captured_timestamps_count()):
		names.append(rd.get_captured_timestamp_name(index))
	return names


# Per TSR viewport one "TSR"; in a viewport whose passes are split (level 2 with async compute active,
# only the first TSR viewport of the frame), "TSR depth and velocity" and then "TSR critical path"
# instead.
func _check_tsr_timestamps(label: String, names: PackedStringArray, viewports: int, split: int) -> void:
	if names.is_empty():
		_failures.append("%s: no captured timestamps; run Godot with --gpu-profile" % label)
		return
	var expected := { TSR_STAMP: viewports - split, TSR_EARLY_STAMP: split, TSR_CRITICAL_STAMP: split }
	for stamp in expected:
		var count := names.count(stamp)
		if count != int(expected[stamp]):
			_failures.append("%s: %d \"%s\" timestamps in a frame, expected %d" % [label, count, stamp, expected[stamp]])
	if split > 0 and names.find(TSR_CRITICAL_STAMP) < names.find(TSR_EARLY_STAMP):
		_failures.append("%s: \"%s\" comes before \"%s\"" % [label, TSR_CRITICAL_STAMP, TSR_EARLY_STAMP])


# The device's running "async:<submitted>/<fell back>" frame counters, as async_compute_check.gd reads them.
func _counters(rd: RenderingDevice) -> Array[int]:
	var report := rd.get_perf_report()
	var at := report.find(" async:")
	if at < 0:
		_failures.append("get_perf_report() has no async: counter")
		return [0, 0]
	var parts := report.substr(at + 7).get_slice(" ", 0).split("/")
	return [int(parts[0]), int(parts[1])]


# The viewport's image; a multiview (XR) target gives its first view.
func _viewport_image(viewport: Viewport) -> Image:
	var texture := viewport.get_texture()
	var image := texture.get_image() if texture != null else null
	if (image == null or image.is_empty()) and texture != null:
		image = RenderingServer.texture_2d_layer_get(texture.get_rid(), 0)
	return image


# The share of a coarse pixel grid with any channel above 1/255.
func _lit_share(image: Image) -> float:
	if image == null or image.is_empty():
		return 0.0
	var size := image.get_size()
	var lit := 0
	var total := 0
	for y in range(0, size.y, 16):
		for x in range(0, size.x, 16):
			var color := image.get_pixel(x, y)
			total += 1
			if maxf(color.r, maxf(color.g, color.b)) > 1.0 / 255.0:
				lit += 1
	return float(lit) / float(maxi(total, 1))


func _finish(preset_name: String, levels: PackedInt32Array, active: bool, results: Dictionary, extra_failures: Array) -> void:
	for failure in extra_failures:
		_failures.append(String(failure))
	if _xr_interface != null and _xr_interface.is_initialized():
		_xr_interface.uninitialize()
	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "tsr_async",
		"pass": ok,
		"preset": preset_name,
		"scale": SCALE,
		"variants": Array(_variants),
		"async_active": active,
		"levels": Array(levels),
		"results": results,
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)
