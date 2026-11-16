extends Node3D
## Vendor-seam check (#210, plan §21.5 U2, GU+-1) for the test-only C GDExtension in
## [code]../src/vendor_seam.c[/code]. When the engine starts, the library registers SeamCopy (a
## TemporalUpscalerExtension that copies the colour input with a compute shader) and
## SeamPassthrough (a FrameGenerationExtension that generates no frames). project.godot selects both:
## 3D scaling mode 8 (CUSTOM) with the custom upscaler "SeamCopy", and the frame-generation provider
## "SeamPassthrough".
##
## Without a RenderingDevice (--headless) nothing renders. The scene checks that both classes and both
## providers are registered, that CUSTOM is available and that frame generation reports
## API_UNSUPPORTED, then unloads the library and checks that the classes and the upscaler are gone.
##
## With a RenderingDevice it draws a still 3D scene under a HUD for 120 frames and checks that:
## - SeamCopy runs _process once per drawn frame, records every copy and CUSTOM stays active;
## - its frame equals the frame of the same scene with bilinear scaling at 1.0 (the expected copy);
## - SeamPassthrough gets one _on_frame_inputs per drawn frame with valid textures, and on D3D12 one
##   _on_present before/after pair per presented frame;
## - a second provider requested while SeamPassthrough holds the process is refused with
##   PROVIDER_ALREADY_ACTIVE, and SeamPassthrough keeps running;
## - a 3D scale outside SeamCopy's range (1.0 to 1.0) is clamped to it;
## - unloading the library unregisters both: CUSTOM falls back to TSR and frame generation reports
##   PROVIDER_UNAVAILABLE.
## run_vendor_seam_test.py checks the log: one line per fallback and no other engine error.
##
## User arguments (after [code]--[/code]): [code]--seam-out=<dir>[/code] writes the CUSTOM frame as
## [code]seam_frame.png[/code]. [code]--seam-reference[/code] draws that frame with the GDScript samples
## of #192 and #203 instead (SampleCopy with no jitter and no mip bias offset, and SamplePassthrough,
## loaded from godotnext/testbed), and [code]--seam-compare=<png>[/code] checks that the frame equals
## the given file, for example the frame of a run with the C library.

const EXTENSION_PATH := "res://vendor_seam_test.gdextension"
const COPY_CLASS := &"SeamCopy"
const PASSTHROUGH_CLASS := &"SeamPassthrough"
const UPSCALER_SETTING := "rendering/scaling_3d/custom_upscaler"
const PROVIDER_SETTING := "rendering/frame_generation/provider"
const SECOND_PROVIDER := "SeamSecond"
## The GDScript samples, relative to this project's folder.
const SAMPLES_DIR := "../../../godotnext/testbed/addons"
const REFERENCE_COPY := "SampleCopy"
const REFERENCE_PASSTHROUGH := "SamplePassthrough"

const MODE_TSR := 7
const MODE_CUSTOM := 8
const REASON_NONE := 0
const REASON_CUSTOM_UPSCALER_UNAVAILABLE := 12

## Frames (counted in _process) at which the steps run.
const WINDOW_START := 10
const WINDOW_END := 50
const BILINEAR_END := 60
const SECOND_START := 64
const SECOND_END := 70
const RESTORED := 76
const CLAMP_START := 78
const CLAMP_END := 82
const UNLOAD := 86
const UNLOADED := 94
const TOTAL_FRAMES := 120


## A second provider, requested while SeamPassthrough holds the process.
class SecondProvider extends FrameGenerationExtension:
	func _get_name() -> String:
		return SECOND_PROVIDER

	func _is_available() -> int:
		return REASON_OK


var _rendering := false
var _reference := false
var _out := ""
var _compare := ""
var _frame := 0
var _drawn := 0
var _failures := PackedStringArray()
var _window_drawn := 0
var _window_copy := {}
var _window_fg := {}
var _custom_image: Image
var _second: SecondProvider
var _second_inputs := 0
var _clamp_copies := 0
var _reference_copy: TemporalUpscalerExtension
var _reference_fg: FrameGenerationExtension


func _ready() -> void:
	_rendering = RenderingServer.get_rendering_device() != null
	_reference = OS.get_cmdline_user_args().has("--seam-reference")
	_out = _user_arg("--seam-out")
	_compare = _user_arg("--seam-compare")
	_build_scene()
	if _reference:
		_use_reference_samples()
	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)
	print("VENDOR_SEAM_SCENE driver=%s method=%s rendering=%s reference=%s" % [RenderingServer.get_current_rendering_driver_name(), RenderingServer.get_current_rendering_method(), _rendering, _reference])


func _user_arg(p_name: String) -> String:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with(p_name + "="):
			return arg.substr(p_name.length() + 1)
	return ""


func _build_scene() -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.2, 0.3, 0.45)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.4, 0.4, 0.4)
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50.0, -30.0, 0.0)
	add_child(sun)
	var camera := Camera3D.new()
	camera.transform = Transform3D(Basis(), Vector3(0.0, 1.6, 5.0)).looking_at(Vector3(0.0, 0.4, 0.0))
	camera.current = true
	add_child(camera)
	var ground := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(12.0, 12.0)
	ground.mesh = plane
	add_child(ground)
	for i in 5:
		var box := MeshInstance3D.new()
		var mesh := BoxMesh.new()
		var material := StandardMaterial3D.new()
		material.albedo_color = Color.from_hsv(0.17 * i, 0.7, 0.85)
		mesh.material = material
		box.mesh = mesh
		box.position = Vector3(-2.4 + 1.2 * i, 0.5 + 0.15 * i, -0.4 * (i % 2))
		box.rotation_degrees = Vector3(0.0, 20.0 * i, 0.0)
		add_child(box)
	# The HUD: a translucent panel and text, so the frame-generation UI layer has content.
	var hud := CanvasLayer.new()
	add_child(hud)
	var panel := ColorRect.new()
	panel.color = Color(0.05, 0.05, 0.1, 0.6)
	panel.position = Vector2(16.0, 16.0)
	panel.size = Vector2(260.0, 56.0)
	hud.add_child(panel)
	var label := Label.new()
	label.text = "Vendor seam check"
	label.position = Vector2(28.0, 32.0)
	hud.add_child(label)


func _use_reference_samples() -> void:
	var samples := ProjectSettings.globalize_path("res://").path_join(SAMPLES_DIR).simplify_path()
	var copy_script: GDScript = load(samples.path_join("sample_copy_upscaler/copy_upscaler.gd"))
	var fg_script: GDScript = load(samples.path_join("sample_passthrough_fg/passthrough_fg.gd"))
	if copy_script == null or fg_script == null:
		_expect(false, "the GDScript samples load from %s" % samples)
		return
	_reference_copy = copy_script.new()
	# Like SeamCopy: a scale of 1.0, no jitter and no mip bias offset.
	_reference_copy.set("scale_range", Vector2(1.0, 1.0))
	_reference_copy.set("jitter_phases", 0)
	_reference_copy.set("mip_bias_offset", 0.0)
	_reference_fg = fg_script.new()
	ProjectSettings.set_setting(UPSCALER_SETTING, REFERENCE_COPY)
	ProjectSettings.set_setting(PROVIDER_SETTING, REFERENCE_PASSTHROUGH)
	RenderingServer.register_temporal_upscaler_extension(_reference_copy)
	RenderingServer.register_frame_generation_extension(_reference_fg)


func _on_frame_post_draw() -> void:
	_drawn += 1


func _expect(p_ok: bool, p_what: String) -> void:
	if not p_ok:
		_failures.push_back(p_what)
		print("VENDOR_SEAM_FAIL %s" % p_what)


func _counters(p_class: StringName) -> Dictionary:
	if not ClassDB.class_exists(p_class):
		return {}
	return ClassDB.class_call_static(p_class, &"get_counters")


func _scaling_status() -> Dictionary:
	return RenderingServer.viewport_get_scaling_3d_status(get_viewport().get_viewport_rid())


func _expect_scaling(p_mode: int, p_reason: int, p_what: String) -> void:
	var status := _scaling_status()
	_expect(int(status.get("active_mode", -1)) == p_mode and int(status.get("reason", -1)) == p_reason, "%s (status %s)" % [p_what, status])


func _expect_fg(p_active: bool, p_provider: String, p_reason: int, p_what: String) -> void:
	var status := RenderingServer.get_frame_generation_status()
	var ok := bool(status.get("active", not p_active)) == p_active and int(status.get("reason", -1)) == p_reason
	if p_active:
		ok = ok and String(status.get("active_provider", "")) == p_provider
	_expect(ok, "%s (status %s)" % [p_what, status])


func _process(_delta: float) -> void:
	_frame += 1
	if not _rendering:
		if _frame == 2:
			_check_headless()
		return
	if _reference:
		_process_reference()
		return
	match _frame:
		WINDOW_START:
			_start_window()
		WINDOW_END:
			_end_window()
			_custom_image = _capture()
			get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
		BILINEAR_END:
			_compare_images(_custom_image, get_viewport().get_texture().get_image(), "SeamCopy at 1.0 vs bilinear at 1.0")
			get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_CUSTOM
		SECOND_START:
			print("VENDOR_SEAM_PHASE second_provider")
			_second = SecondProvider.new()
			RenderingServer.register_frame_generation_extension(_second)
			ProjectSettings.set_setting(PROVIDER_SETTING, SECOND_PROVIDER)
			_second_inputs = int(_counters(PASSTHROUGH_CLASS).get("inputs_calls", 0))
		SECOND_END:
			_expect_fg(true, String(PASSTHROUGH_CLASS), FrameGenerationExtension.REASON_PROVIDER_ALREADY_ACTIVE, "a second provider is refused and SeamPassthrough keeps the process")
			_expect(int(_counters(PASSTHROUGH_CLASS).get("inputs_calls", 0)) > _second_inputs, "SeamPassthrough keeps getting frame inputs")
			ProjectSettings.set_setting(PROVIDER_SETTING, String(PASSTHROUGH_CLASS))
			RenderingServer.unregister_frame_generation_extension(_second)
			_second = null
		RESTORED:
			_expect_fg(true, String(PASSTHROUGH_CLASS), FrameGenerationExtension.REASON_OK, "SeamPassthrough is the provider again")
			_expect_scaling(MODE_CUSTOM, REASON_NONE, "SeamCopy is active again after the bilinear frames")
		CLAMP_START:
			# SeamCopy's scale range is 1.0 to 1.0: a lower scale is clamped, with one warning.
			print("VENDOR_SEAM_PHASE clamp")
			_clamp_copies = int(_counters(COPY_CLASS).get("copies", 0))
			get_viewport().scaling_3d_scale = 0.5
		CLAMP_END:
			var status := _scaling_status()
			_expect(int(status.get("active_mode", -1)) == MODE_CUSTOM and is_equal_approx(float(status.get("effective_scale", -1.0)), 1.0), "SeamCopy's scale range clamps 0.5 to 1.0 (status %s)" % status)
			var copy := _counters(COPY_CLASS)
			_expect(int(copy.get("copies", 0)) > _clamp_copies and int(copy.get("copy_errors", -1)) == 0, "every copy was recorded, also while clamped (%s)" % copy)
			get_viewport().scaling_3d_scale = 1.0
		UNLOAD:
			print("VENDOR_SEAM_PHASE unload")
			_unload()
		UNLOADED:
			_expect_scaling(MODE_TSR, REASON_CUSTOM_UPSCALER_UNAVAILABLE, "CUSTOM falls back to TSR once SeamCopy is unregistered")
			_expect_fg(false, "", FrameGenerationExtension.REASON_PROVIDER_UNAVAILABLE, "frame generation reports PROVIDER_UNAVAILABLE once SeamPassthrough is unregistered")
			_expect(int(RenderingServer.get_frame_generation_status().get("split_textures", -1)) == 0, "the split holds no textures once frame generation is off")
		TOTAL_FRAMES:
			_finish()


func _check_headless() -> void:
	print("VENDOR_SEAM_PHASE headless")
	_expect(ClassDB.class_exists(COPY_CLASS) and ClassDB.get_parent_class(COPY_CLASS) == &"TemporalUpscalerExtension", "SeamCopy is registered as a TemporalUpscalerExtension")
	_expect(ClassDB.class_exists(PASSTHROUGH_CLASS) and ClassDB.get_parent_class(PASSTHROUGH_CLASS) == &"FrameGenerationExtension", "SeamPassthrough is registered as a FrameGenerationExtension")
	var copy := _counters(COPY_CLASS)
	var fg := _counters(PASSTHROUGH_CLASS)
	print("VENDOR_SEAM_COUNTERS copy=%s fg=%s" % [copy, fg])
	_expect(int(copy.get("registered", 0)) == 1 and int(fg.get("registered", 0)) == 1, "the library registered one provider of each kind")
	_expect(int(copy.get("live_instances", -1)) == 2, "the library made exactly two instances")
	_expect(int(copy.get("name_calls", 0)) >= 1 and int(fg.get("name_calls", 0)) >= 1, "the RenderingServer read both providers' names")
	_expect(RenderingServer.scaling_3d_mode_get_availability(RenderingServer.VIEWPORT_SCALING_3D_MODE_CUSTOM) == REASON_NONE, "CUSTOM is available with SeamCopy")
	_expect(int(_counters(COPY_CLASS).get("available_calls", 0)) >= 1, "SeamCopy._is_available ran")
	var status := RenderingServer.get_frame_generation_status()
	_expect(String(status.get("provider", "")) == String(PASSTHROUGH_CLASS) and bool(status.get("requested", false)), "frame generation requests SeamPassthrough (status %s)" % status)
	# Nothing renders without a RenderingDevice, so frame generation falls back before it asks the provider.
	_expect_fg(false, "", FrameGenerationExtension.REASON_API_UNSUPPORTED, "frame generation reports API_UNSUPPORTED headless")
	_unload()
	_expect(RenderingServer.scaling_3d_mode_get_availability(RenderingServer.VIEWPORT_SCALING_3D_MODE_CUSTOM) == REASON_CUSTOM_UPSCALER_UNAVAILABLE, "CUSTOM is unavailable once SeamCopy is unregistered")
	_finish()


func _start_window() -> void:
	_expect_scaling(MODE_CUSTOM, REASON_NONE, "SeamCopy is the active upscaler")
	_expect_fg(true, String(PASSTHROUGH_CLASS), FrameGenerationExtension.REASON_OK, "SeamPassthrough is the active provider")
	_window_drawn = _drawn
	_window_copy = _counters(COPY_CLASS)
	_window_fg = _counters(PASSTHROUGH_CLASS)
	_expect(int(_window_copy.get("reset_history_calls", 0)) >= 1, "SeamCopy._reset_history ran for the first frame")
	_expect(int(_window_fg.get("reset_calls", 0)) >= 1 and int(_window_fg.get("mode_calls", 0)) >= 1, "SeamPassthrough got _set_mode and the activation _reset")


func _end_window() -> void:
	var drawn := _drawn - _window_drawn
	var copy := _counters(COPY_CLASS)
	var fg := _counters(PASSTHROUGH_CLASS)
	print("VENDOR_SEAM_COUNTERS drawn=%d copy=%s fg=%s" % [drawn, copy, fg])
	_expect_scaling(MODE_CUSTOM, REASON_NONE, "SeamCopy is still the active upscaler")
	_expect(drawn > 0, "frames were drawn")
	_expect(int(copy.get("process_calls", 0)) - int(_window_copy.get("process_calls", 0)) == drawn, "one SeamCopy._process per drawn frame")
	_expect(int(copy.get("copies", 0)) - int(_window_copy.get("copies", 0)) == drawn and int(copy.get("copy_errors", -1)) == 0, "every copy was recorded")
	_expect(Vector2i(int(copy.get("last_width", 0)), int(copy.get("last_height", 0))) == get_viewport().size, "the output is the viewport size")
	_expect(int(fg.get("inputs_calls", 0)) - int(_window_fg.get("inputs_calls", 0)) == drawn, "one SeamPassthrough._on_frame_inputs per drawn frame")
	for key in ["invalid_hudless", "invalid_ui", "invalid_depth", "invalid_velocity"]:
		_expect(int(fg.get(key, -1)) == 0, "every frame input is a valid texture (%s)" % key)
	var present_hook := bool(RenderingServer.get_frame_generation_status().get("present_hook", false))
	var before := int(fg.get("present_before", 0)) - int(_window_fg.get("present_before", 0))
	var after := int(fg.get("present_after", 0)) - int(_window_fg.get("present_after", 0))
	if RenderingServer.get_current_rendering_driver_name() == "d3d12":
		_expect(present_hook, "D3D12 has the present hook")
		_expect(before == drawn and after == drawn and int(fg.get("present_order_errors", -1)) == 0, "one _on_present pair per presented frame (%d before, %d after, %d frames)" % [before, after, drawn])
	else:
		_expect(not present_hook and int(fg.get("present_before", -1)) == 0, "no present hook without D3D12")


func _capture() -> Image:
	var image := get_viewport().get_texture().get_image()
	# A lit frame with content, not a cleared or uniform one.
	var lowest := 1.0
	var highest := 0.0
	for y in range(0, image.get_height(), 8):
		for x in range(0, image.get_width(), 8):
			var luminance := image.get_pixel(x, y).get_luminance()
			lowest = minf(lowest, luminance)
			highest = maxf(highest, luminance)
	_expect(highest - lowest > 0.2, "the frame has content (luminance %.3f to %.3f)" % [lowest, highest])
	if not _out.is_empty():
		DirAccess.make_dir_recursive_absolute(_out)
		var path := _out.path_join("seam_frame.png")
		_expect(image.save_png(path) == OK, "write %s" % path)
	return image


func _compare_images(p_a: Image, p_b: Image, p_what: String) -> void:
	if p_a == null or p_b == null or p_a.get_size() != p_b.get_size():
		_expect(false, "%s: two frames of the same size" % p_what)
		return
	if p_b.get_format() != p_a.get_format():
		p_b.convert(p_a.get_format())
	var differing := 0
	var largest := 0.0
	if p_a.get_data() != p_b.get_data():
		for y in p_a.get_height():
			for x in p_a.get_width():
				var a := p_a.get_pixel(x, y)
				var b := p_b.get_pixel(x, y)
				var difference := maxf(maxf(absf(a.r - b.r), absf(a.g - b.g)), maxf(absf(a.b - b.b), absf(a.a - b.a)))
				if difference > 0.0:
					differing += 1
					largest = maxf(largest, difference)
	print("VENDOR_SEAM_COMPARE %s: %d of %d pixels differ, largest difference %.4f" % [p_what, differing, p_a.get_width() * p_a.get_height(), largest])
	_expect(differing == 0, "%s: the frames are identical" % p_what)


func _unload() -> void:
	var loaded := GDExtensionManager.is_extension_loaded(EXTENSION_PATH)
	var status := GDExtensionManager.unload_extension(EXTENSION_PATH)
	_expect(loaded and status == GDExtensionManager.LOAD_STATUS_OK, "the library unloads (status %d)" % status)
	_expect(not ClassDB.class_exists(COPY_CLASS) and not ClassDB.class_exists(PASSTHROUGH_CLASS), "unloading removes both classes")


func _process_reference() -> void:
	if _frame == WINDOW_END:
		_expect_scaling(MODE_CUSTOM, REASON_NONE, "SampleCopy is the active upscaler")
		_expect_fg(true, REFERENCE_PASSTHROUGH, FrameGenerationExtension.REASON_OK, "SamplePassthrough is the active provider")
		_expect(int(_reference_copy.get("process_count")) > 0, "SampleCopy._process ran")
		var image := _capture()
		if not _compare.is_empty():
			_compare_images(Image.load_from_file(_compare), image, "the C library's frame vs the GDScript samples' frame")
	elif _frame == BILINEAR_END:
		# The samples stay registered: the engine releases them when it shuts down, and unregistering
		# them here would log the fallbacks of the frame drawn before the quit.
		_finish()


func _finish() -> void:
	var ok := _failures.is_empty()
	var mode := "headless" if not _rendering else ("reference" if _reference else "rendered")
	print("GODOTNEXT_RESULT " + JSON.stringify({"test": "vendor_seam", "mode": mode, "pass": ok, "frames": _frame, "drawn": _drawn, "failures": _failures}))
	get_tree().quit(0 if ok else 1)
