extends Node3D
## Custom upscaler check (#192, GT-1 subset, plan §21.5 U2). Registers the
## test-only SampleCopy extension, selects SCALING_3D_MODE_CUSTOM and checks
## the seam on a real renderer: dispatch, jitter phases and mip bias reaching
## the viewport, the scale clamp, the TSR fallback with
## CUSTOM_UPSCALER_UNAVAILABLE, one _reset_history per reset, and 100
## register/unregister cycles while rendering with no leaked shader RIDs.
## run_custom_upscaler.sh adds the log checks (one warning per fallback kind,
## no engine or Metal validation errors).

const SampleCopy := preload("res://addons/sample_copy_upscaler/copy_upscaler.gd")

const MODE_TSR := 7
const MODE_CUSTOM := 8
const REASON_NONE := 0
const REASON_CUSTOM_UPSCALER_UNAVAILABLE := 12
const CYCLES := 100
const SCALE := 0.75
const JITTER_PHASES := 13
const MIP_BIAS_OFFSET := -1.0

var _ext: TemporalUpscalerExtension
var _frame := 0
var _failures := PackedStringArray()
var _jitters := {}
var _cycles_done := 0
var _cycle_start_frame := -1
var _resets_before := 0
var _custom_image: Image


func _ready() -> void:
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 3.0)
	camera.current = true
	add_child(camera)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50.0, -30.0, 0.0)
	add_child(sun)
	var box := MeshInstance3D.new()
	box.mesh = BoxMesh.new()
	box.rotation_degrees = Vector3(0.0, 30.0, 0.0)
	add_child(box)

	ProjectSettings.set_setting("rendering/scaling_3d/custom_upscaler", SampleCopy.UPSCALER_NAME)
	_ext = SampleCopy.new()
	RenderingServer.register_temporal_upscaler_extension(_ext)
	get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_CUSTOM
	get_viewport().scaling_3d_scale = SCALE


func _status() -> Dictionary:
	return RenderingServer.viewport_get_scaling_3d_status(get_viewport().get_viewport_rid())


func _expect(p_ok: bool, p_what: String) -> void:
	if not p_ok:
		_failures.push_back(p_what)
		print("CUSTOM_UPSCALER_FAIL %s status=%s" % [p_what, _status()])


func _expect_active(p_mode: int, p_reason: int, p_what: String) -> void:
	var status := _status()
	_expect(int(status.get("active_mode", -1)) == p_mode and int(status.get("reason", -1)) == p_reason, p_what)


## Functional check only (FLIP vs bilinear runs in PQ): the copy wrote a lit
## frame that stays close to bilinear scaling of the same scene.
func _compare_with_bilinear(p_bilinear: Image) -> void:
	if _custom_image == null or p_bilinear == null or _custom_image.get_size() != p_bilinear.get_size():
		_expect(false, "custom and bilinear frames were captured at the same size")
		return
	var size := _custom_image.get_size()
	var brightest := 0.0
	var total_diff := 0.0
	var max_diff := 0.0
	var differing := 0
	var samples := 0
	for y in range(0, size.y, 4):
		for x in range(0, size.x, 4):
			var a := _custom_image.get_pixel(x, y)
			var b := p_bilinear.get_pixel(x, y)
			brightest = maxf(brightest, maxf(a.r, maxf(a.g, a.b)))
			var diff := (absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b)) / 3.0
			total_diff += diff
			max_diff = maxf(max_diff, diff)
			if diff > 0.002:
				differing += 1
			samples += 1
	var mean_diff := total_diff / maxf(samples, 1)
	print("CUSTOM_UPSCALER_IMAGE brightest=%.3f mean_abs_diff_vs_bilinear=%.5f max=%.4f differing=%d/%d" % [brightest, mean_diff, max_diff, differing, samples])
	_expect(brightest > 0.1, "the custom upscaler wrote a lit frame")
	_expect(mean_diff < 0.03, "the custom frame is close to bilinear scaling")


func _process(_delta: float) -> void:
	_frame += 1
	match _frame:
		10:
			print("CUSTOM_UPSCALER_PHASE select")
			_expect_active(MODE_CUSTOM, REASON_NONE, "custom upscaler is active")
			_expect(int(_status().get("requested_mode", -1)) == MODE_CUSTOM, "requested mode is CUSTOM")
			_expect(is_equal_approx(float(_status().get("effective_scale", -1.0)), SCALE), "scale is kept inside the range")
			_expect(RenderingServer.scaling_3d_mode_get_availability(RenderingServer.VIEWPORT_SCALING_3D_MODE_CUSTOM) == REASON_NONE, "availability reports NONE")
			_expect(_ext.process_count > 0, "_process ran")
			_expect(_ext.reset_count >= 1, "_reset_history ran for the first frame")
			var last: Dictionary = _ext.last
			_expect(last.get("output_size", Vector2i()) == get_viewport().size, "output size is the viewport size")
			_expect(int(last.get("view_index", -1)) == 0, "view index is 0")
			_expect(bool(last.get("has_depth_and_velocity", false)), "depth and velocity are bound")
			_expect(bool(last.get("has_render_buffers", false)), "the viewport's render buffers are passed")
			_expect(Vector2i(last.get("max_internal_size", Vector2i())).x >= Vector2i(last.get("internal_size", Vector2i(1, 1))).x, "max internal size covers the internal size")
			_custom_image = get_viewport().get_texture().get_image()
			# Jitter and mip bias from the extension reach the viewport.
			_ext.jitter_phases = JITTER_PHASES
			_ext.mip_bias_offset = MIP_BIAS_OFFSET
			RenderingServer.register_temporal_upscaler_extension(_ext)
		14:
			var status := _status()
			_expect(int(status.get("jitter_phase_count", -1)) == JITTER_PHASES, "jitter phase count reaches the viewport")
			var expected_bias := log(SCALE) / log(2.0) + MIP_BIAS_OFFSET
			_expect(is_equal_approx(float(status.get("mipmap_bias", 0.0)), expected_bias), "mip bias offset reaches the viewport")
			_jitters.clear()
		41:
			_expect(_jitters.size() == JITTER_PHASES, "jitter cycles through %d phases (saw %d)" % [JITTER_PHASES, _jitters.size()])
			print("CUSTOM_UPSCALER_PHASE clamp")
			get_viewport().scaling_3d_scale = 0.1
		45:
			_expect_active(MODE_CUSTOM, REASON_NONE, "custom stays active when clamped")
			_expect(is_equal_approx(float(_status().get("effective_scale", -1.0)), 0.25), "scale clamps to the extension range")
			get_viewport().scaling_3d_scale = SCALE
		50:
			print("CUSTOM_UPSCALER_PHASE unavailable")
			_ext.available = false
		55:
			_expect_active(MODE_TSR, REASON_CUSTOM_UPSCALER_UNAVAILABLE, "unavailable extension falls back to TSR")
			_expect(RenderingServer.scaling_3d_mode_get_availability(RenderingServer.VIEWPORT_SCALING_3D_MODE_CUSTOM) == REASON_CUSTOM_UPSCALER_UNAVAILABLE, "availability reports CUSTOM_UPSCALER_UNAVAILABLE")
			_ext.available = true
		60:
			_expect_active(MODE_CUSTOM, REASON_NONE, "custom upscaler is active again")
			_resets_before = _ext.reset_count
			RenderingServer.viewport_reset_temporal_history(get_viewport().get_viewport_rid())
		64:
			_expect(_ext.reset_count == _resets_before + 1, "_reset_history runs once per reset (%d -> %d)" % [_resets_before, _ext.reset_count])
			print("CUSTOM_UPSCALER_PHASE cycles")
			_cycle_start_frame = _frame
	if _frame >= 15 and _frame < 41 and _ext.last.has("jitter"):
		_jitters[_ext.last["jitter"]] = true
	if _cycle_start_frame > 0 and _cycles_done < CYCLES:
		# One cycle per frame with a fresh instance, so each retired one is
		# freed through the deferred path while frames are in flight.
		RenderingServer.unregister_temporal_upscaler_extension(_ext)
		_ext = SampleCopy.new()
		RenderingServer.register_temporal_upscaler_extension(_ext)
		_cycles_done += 1
		if _cycles_done == CYCLES:
			_cycle_start_frame = _frame
		return
	if _cycles_done == CYCLES and _cycle_start_frame > 0:
		var since := _frame - _cycle_start_frame
		if since == 5:
			_expect_active(MODE_CUSTOM, REASON_NONE, "custom upscaler is active after %d cycles" % CYCLES)
			_expect(_ext.process_count > 0, "the last instance ran _process")
			_expect(SampleCopy.live_shaders == 1, "retired instances freed their shaders (%d live)" % SampleCopy.live_shaders)
			print("CUSTOM_UPSCALER_PHASE unregister")
			RenderingServer.unregister_temporal_upscaler_extension(_ext)
			_ext = null
		elif since == 10:
			_expect_active(MODE_TSR, REASON_CUSTOM_UPSCALER_UNAVAILABLE, "unregistered extension falls back to TSR")
			_expect(SampleCopy.live_shaders == 0, "all shaders are freed (%d live)" % SampleCopy.live_shaders)
			get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
		elif since == 15:
			_compare_with_bilinear(get_viewport().get_texture().get_image())
		elif since == 16:
			var ok := _failures.is_empty()
			print("GODOTNEXT_RESULT " + JSON.stringify({"test": "custom_upscaler", "pass": ok, "cycles": _cycles_done, "failures": _failures}))
			get_tree().quit(0 if ok else 1)
