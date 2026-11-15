extends Node3D
## GT-6 load step (#190, plan §21.5): the Dense City flight with a scripted 2x GPU load step.
##
## At startup, with dynamic resolution off and the camera sweeping the 30 flight frames before
## frame 600 back and forth (so passes whose cost depends on motion, such as a temporal upscaler's,
## cost what they cost there), the script calibrates the burner
## ([code]gpu_load_step_burner.gd[/code]) on the viewport's GPU time.
## After a warm-up, each round measures the baseline (burner off) and then the loaded time (burner
## on) back to back, so a GPU clock that drifts between rounds does not skew the ratio. The rounds
## stop once loaded / baseline is 2 within 2% (it passes within 5%). Then the camera flies the
## Dense City spline by frame index and the burner switches on at flight frame 600. The target
## frame rate is 60 Hz. GT-6 timing runs lock the GPU clocks (#237), as every timing run does.
##
## Content: the Slow IO Flight stand-in boxes, or with [code]--with-city=1[/code] the generated
## Dense City ([code]res://generated/dense_city/dense_city.tscn[/code], built on demand).
## Arguments after [code]--[/code]: [code]--out=<absolute dir>[/code] writes
## [code]gpu_load_step.json[/code] and [code]gpu_load_step.csv[/code];
## [code]--flight-frames=<n>[/code] (default 720) ends a standalone run;
## [code]--dynamic-resolution=1[/code] turns dynamic resolution on once calibration ends (it is
## off during calibration; otherwise the project setting comes back).
## Under a harness timing run (#12) the harness owns the frame index and the exit, and the
## calibration has to finish inside its [code]--warmup[/code] (900 frames is enough). The harness
## rejects flags it does not know, so there the three flags above are not available: dynamic
## resolution comes from the project settings, and the upscaler from [code]--upscaler[/code].
## The burner's cost scales with the internal pixel count, so dynamic resolution can win it back.
##
## The doubling is measured in PQ (GT-6). [code]python3 -m godotnext_metrics.load_step[/code]
## reads the CSV or the harness timing.csv.

const Flight = preload("res://scenes/slow_io_flight/flight.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")
const BurnerScript = preload("res://tests/gpu_load_step_burner.gd")

const FLIGHT_SCENE := "res://scenes/slow_io_flight/slow_io_flight.tscn"
const SWITCH_FRAME := 600
const DEFAULT_FLIGHT_FRAMES := 720
const TARGET_RATIO := 2.0
const TOLERANCE := 0.05
const AIM := 0.02
const TARGET_FRAME_RATE := 60
const WARMUP_FRAMES := 240
const PROBE_DISCARD := 8
const SAMPLES := 30
const MAX_ROUNDS := 12
const FIRST_ITERATIONS := 64
## The flight frames before the switch that calibration sweeps, the window the GT-6 check
## (godotnext_metrics.load_step) compares against.
const CALIBRATION_WINDOW := 30
const DYNRES_SETTING := "rendering/scaling_3d/dynamic_resolution/enabled"

enum Phase { BASELINE, LOADED, FLIGHT }

var _phase := Phase.BASELINE
var _calibration_step := 0
var _discard := WARMUP_FRAMES
var _iterations := FIRST_ITERATIONS
var _samples: Array[float] = []
var _baseline_ms := 0.0
var _rounds: Array = []
var _calibration := {}
var _overlapped_window := false
var _flight_local := 0
var _flight_frames := DEFAULT_FLIGHT_FRAMES
var _dynres_after := false
var _dynres_before := false
var _rows: PackedStringArray = []
var _out := ""
var _camera: Camera3D
var _burner: BurnerScript
var _viewport_rid := RID()
var _content := "stand_in"


func _ready() -> void:
	process_priority = 2
	_out = SceneArgs.value("--out", "")
	_flight_frames = maxi(SceneArgs.int_value("--flight-frames", DEFAULT_FLIGHT_FRAMES), 1)
	_dynres_after = SceneArgs.value("--dynamic-resolution", "0") == "1"
	_dynres_before = bool(ProjectSettings.get_setting(DYNRES_SETTING, false))
	ProjectSettings.set_setting(DYNRES_SETTING, false)
	var packed := load(FLIGHT_SCENE) as PackedScene
	var flight := packed.instantiate()
	# The Slow IO Flight script stays idle: this script drives its camera.
	flight.set_meta("skip_runtime", true)
	add_child(flight)
	if SceneArgs.value("--with-city", "0") == "1":
		if ResourceLoader.exists(Flight.DENSE_CITY_SCENE):
			var city := (load(Flight.DENSE_CITY_SCENE) as PackedScene).instantiate()
			city.name = "DenseCity"
			add_child(city)
			_content = "dense_city"
		else:
			push_warning("gpu_load_step: %s is not generated; flying the stand-in." % Flight.DENSE_CITY_SCENE)
	_camera = flight.get_node("FlightCam") as Camera3D
	_camera.current = true
	_burner = BurnerScript.new()
	var compositor := Compositor.new()
	compositor.compositor_effects = [_burner]
	_camera.compositor = compositor
	_viewport_rid = get_viewport().get_viewport_rid()
	RenderingServer.viewport_set_measure_render_time(_viewport_rid, true)
	_camera.global_transform = Flight.camera_transform(calibration_pose(0, SWITCH_FRAME, CALIBRATION_WINDOW))
	print("gpu_load_step: content=%s switch_frame=%d target_ratio=%.1f calibrating" % [_content, SWITCH_FRAME, TARGET_RATIO])


func _process(_delta: float) -> void:
	var gpu_ms := RenderingServer.viewport_get_measured_render_time_gpu(_viewport_rid)
	var harness := get_node_or_null("/root/Harness")
	var driving := harness != null and harness.has_method("is_driving_animation") and bool(harness.call("is_driving_animation"))
	if _phase != Phase.FLIGHT:
		if driving and int(harness.call("get_animation_frame")) > 0:
			_overlapped_window = true
		_calibration_step += 1
		_camera.global_transform = Flight.camera_transform(calibration_pose(_calibration_step, SWITCH_FRAME, CALIBRATION_WINDOW))
		_calibrate(gpu_ms)
		return
	var frame := int(harness.call("get_animation_frame")) if driving else _flight_local
	_camera.global_transform = Flight.camera_transform(frame)
	_burner.enabled = burner_active(frame, SWITCH_FRAME)
	_rows.append("%d,%d,%.6f,%d,%d,%d" % [frame, 1 if _burner.enabled else 0, gpu_ms, _burner.iterations, _burner.last_internal_size.x, _burner.last_internal_size.y])
	_flight_local += 1
	if not driving and _flight_local >= _flight_frames:
		get_tree().quit(0 if bool(_calibration.get("pass", false)) else 1)


func _calibrate(gpu_ms: float) -> void:
	if _discard > 0:
		_discard -= 1
		return
	if gpu_ms > 0.0:
		_samples.append(gpu_ms)
	if _samples.size() < SAMPLES:
		return
	var measured := median(_samples)
	_samples.clear()
	_discard = PROBE_DISCARD
	if _phase == Phase.BASELINE:
		_baseline_ms = measured
		_phase = Phase.LOADED
		_burner.iterations = _iterations
		_burner.enabled = true
		return
	var ratio := measured / _baseline_ms
	_rounds.append({"iterations": _iterations, "baseline_ms": _baseline_ms, "loaded_ms": measured, "ratio": ratio})
	var at_cap := _iterations >= BurnerScript.MAX_ITERATIONS and ratio < TARGET_RATIO
	if within(ratio, TARGET_RATIO, AIM) or _rounds.size() >= MAX_ROUNDS or at_cap:
		_finish_calibration(measured, ratio)
		return
	_iterations = next_iterations(_iterations, _baseline_ms, measured, TARGET_RATIO)
	_phase = Phase.BASELINE
	_burner.enabled = false


func _finish_calibration(loaded_ms: float, ratio: float) -> void:
	_burner.enabled = false
	_calibration = {
		"baseline_ms": _baseline_ms,
		"loaded_ms": loaded_ms,
		"ratio": ratio,
		"iterations": _iterations,
		"rounds": _rounds,
		"internal_size": [_burner.last_internal_size.x, _burner.last_internal_size.y],
		"pass": within(ratio, TARGET_RATIO, TOLERANCE),
	}
	ProjectSettings.set_setting(DYNRES_SETTING, _dynres_after or _dynres_before)
	_phase = Phase.FLIGHT
	print("GPU_LOAD_STEP_CALIBRATION " + JSON.stringify(_calibration))


func _exit_tree() -> void:
	ProjectSettings.set_setting(DYNRES_SETTING, _dynres_before)
	var report := {
		"schema": 1,
		"content": _content,
		"switch_frame": SWITCH_FRAME,
		"target_ratio": TARGET_RATIO,
		"tolerance": TOLERANCE,
		"target_frame_rate": TARGET_FRAME_RATE,
		"budget_ms": 1000.0 / float(TARGET_FRAME_RATE),
		"calibrated": not _calibration.is_empty(),
		"calibration": _calibration,
		"calibration_overlapped_window": _overlapped_window,
		"dynamic_resolution_after_calibration": _dynres_after or _dynres_before,
		"flight_frames_recorded": _rows.size(),
		"adapter": RenderingServer.get_video_adapter_name(),
	}
	print("GPU_LOAD_STEP " + JSON.stringify(report))
	if _overlapped_window:
		push_error("gpu_load_step: calibration ran into the harness's measured window; raise --warmup.")
	if _out.is_empty() or not _out.is_absolute_path():
		return
	DirAccess.make_dir_recursive_absolute(_out)
	var json_file := FileAccess.open(_out.path_join("gpu_load_step.json"), FileAccess.WRITE)
	if json_file != null:
		json_file.store_string(JSON.stringify(report, "\t") + "\n")
		json_file.close()
	var csv_file := FileAccess.open(_out.path_join("gpu_load_step.csv"), FileAccess.WRITE)
	if csv_file != null:
		csv_file.store_line("frame,burner,gpu_ms,iterations,internal_width,internal_height")
		for row in _rows:
			csv_file.store_line(row)
		csv_file.close()


## The burner runs from the switch frame on.
static func burner_active(frame: int, switch_frame: int) -> bool:
	return frame >= switch_frame


## The flight frame shown on calibration step [param step]: it sweeps the [param window] frames
## before [param switch_frame] forward and back, one frame per step, so the camera never cuts.
static func calibration_pose(step: int, switch_frame: int, window: int) -> int:
	var period := 2 * (window - 1)
	var phase := step % period
	return switch_frame - window + (phase if phase < window else period - phase)


## Within [param tolerance] of [param target], as a fraction of it. The 1e-9 keeps the band's
## edges (1.9x and 2.1x for 2x at 5%) inside it despite rounding.
static func within(ratio: float, target: float, tolerance: float) -> bool:
	return absf(ratio / target - 1.0) <= tolerance + 1e-9


static func median(values: Array[float]) -> float:
	if values.is_empty():
		return 0.0
	var sorted := values.duplicate()
	sorted.sort()
	var middle := sorted.size() >> 1
	if sorted.size() % 2 == 1:
		return sorted[middle]
	return 0.5 * (sorted[middle - 1] + sorted[middle])


## The next iteration count: linear in the added GPU time, damped to at most 8x either way, and
## growing 8x while the added time is still under 2% of the baseline.
static func next_iterations(current: int, baseline_ms: float, loaded_ms: float, target_ratio: float) -> int:
	var added := loaded_ms - baseline_ms
	var wanted := baseline_ms * (target_ratio - 1.0)
	var next := float(current) * 8.0
	if added > baseline_ms * 0.02:
		next = clampf(float(current) * wanted / added, float(current) * 0.125, float(current) * 8.0)
	return clampi(roundi(next), 1, BurnerScript.MAX_ITERATIONS)
