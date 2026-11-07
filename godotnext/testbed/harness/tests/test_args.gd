extends SceneTree

## Parser, manifest schema, percentile, and CSV checks. No GPU and no window.

const ArgsScript = preload("res://harness/args.gd")
const ManifestScript = preload("res://harness/manifest.gd")

var _failures := 0


func _initialize() -> void:
	_test_parse()
	_test_manifest()
	_test_percentiles()
	_test_csv()
	_test_timing_fields()
	_test_animation_frame()
	_test_capture_environment()
	if _failures == 0:
		print("test_args: ok")
		quit(0)
	else:
		print("test_args: %d failure(s)" % _failures)
		quit(1)


func _process(_delta: float) -> bool:
	return true


func _check(condition: bool, message: String) -> void:
	if condition:
		return
	_failures += 1
	print("FAIL: " + message)


func _test_parse() -> void:
	var idle: Dictionary = ArgsScript.parse(PackedStringArray())
	_check(bool(idle["ok"]) and not bool(idle["present"]), "empty args stay idle")

	var foreign: Dictionary = ArgsScript.parse(PackedStringArray(["--frames=1"]))
	_check(bool(foreign["ok"]) and not bool(foreign["present"]), "flags without --mode stay idle")

	var captured: Dictionary = ArgsScript.parse(PackedStringArray([
		"--mode=capture",
		"--scene=res://harness/selftest/selftest.tscn",
		"--out=/tmp/out",
		"--frames=3",
	]))
	_check(bool(captured["ok"]) and bool(captured["present"]), "capture defaults parse")
	if bool(captured["ok"]):
		var args: Dictionary = captured["args"]
		_check(String(args["mode"]) == "capture", "mode capture")
		_check(int(args["warmup"]) == 0, "warmup defaults to 0")
		_check(int(args["seed"]) == 1, "seed defaults to 1")
		_check(String(args["camera"]) == "", "camera defaults to empty")
		_check(args["capture_frames"].is_empty(), "capture frames default to empty")

	var spaced: Dictionary = ArgsScript.parse(PackedStringArray([
		"--mode", "timing",
		"--scene", "res://a.tscn",
		"--camera", "CaptureCam",
		"--out", "/tmp/out",
		"--seed", "4",
		"--frames", "10",
		"--warmup", "2",
		"--capture-frames", "0, 4",
	]))
	_check(bool(spaced["ok"]), "spaced flags parse: %s" % spaced["error"])
	if bool(spaced["ok"]):
		var args: Dictionary = spaced["args"]
		_check(String(args["mode"]) == "timing", "spaced mode")
		_check(int(args["seed"]) == 4, "spaced seed")
		_check(int(args["frames"]) == 10, "spaced frames")
		_check(int(args["warmup"]) == 2, "spaced warmup")
		_check(String(args["camera"]) == "CaptureCam", "spaced camera")
		_check(args["capture_frames"] == [0, 4], "spaced capture frames")

	var windows_out: Dictionary = ArgsScript.parse(PackedStringArray([
		"--mode=capture",
		"--scene=res://a.tscn",
		"--out=C:/tmp/out",
		"--frames=1",
	]))
	_check(bool(windows_out["ok"]), "drive-letter --out is absolute")

	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=nope", "--scene=res://a.tscn", "--out=/tmp/out", "--frames=1",
	]))["ok"]), "bad mode fails")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=res://a.tscn", "--out=res://out", "--frames=1",
	]))["ok"]), "res:// --out fails")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=res://a.tscn", "--out=user://out", "--frames=1",
	]))["ok"]), "user:// --out fails")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=res://a.tscn", "--out=relative/out", "--frames=1",
	]))["ok"]), "relative --out fails")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=a.tscn", "--out=/tmp/out", "--frames=1",
	]))["ok"]), "scene must be res://")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--out=/tmp/out", "--frames=1",
	]))["ok"]), "missing scene fails")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=res://a.tscn", "--out=/tmp/out", "--frames=0",
	]))["ok"]), "zero frames fails")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=res://a.tscn", "--out=/tmp/out", "--frames=1", "--capture-frames=0,,2",
	]))["ok"]), "empty capture-frame token fails")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=res://a.tscn", "--out=/tmp/out", "--frames=1", "--extra=1",
	]))["ok"]), "unknown flag fails when --mode is set")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=res://a.tscn", "--out=/tmp/out", "--frames=1", "--foo",
	]))["ok"]), "unknown --foo fails when --mode is set")

	var scene_flags: Dictionary = ArgsScript.parse(PackedStringArray([
		"--mode=timing",
		"--scene=res://scenes/nextgi_perf/nextgi_perf.tscn",
		"--out=/tmp/out",
		"--frames=8",
		"--variant=door_open",
		"--hold-frame=60",
		"--instances=100000",
	]))
	_check(bool(scene_flags["ok"]), "scene flags parse: %s" % scene_flags["error"])
	if bool(scene_flags["ok"]):
		var args: Dictionary = scene_flags["args"]
		_check(String(args["variant"]) == "door_open", "variant parsed")
		_check(int(args["hold_frame"]) == 60, "hold-frame parsed")
		_check(int(args["instances"]) == 100000, "instances parsed")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=timing", "--scene=res://a.tscn", "--out=/tmp/out", "--frames=1", "--hold-frame=-3",
	]))["ok"]), "negative hold-frame fails")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=timing", "--scene=res://a.tscn", "--out=/tmp/out", "--frames=1", "--instances=0",
	]))["ok"]), "zero instances fails")

	var torture_flags: Dictionary = ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=res://scenes/upscaler_torture/upscaler_torture.tscn", "--out=/tmp/out", "--frames=410",
		"--velocity-dump", "--reference=ssaa", "--msaa=8", "--mask=alpha_particles", "--upscaler=fsr2", "--scale=0.5",
	]))
	_check(bool(torture_flags["ok"]), "upscaler capture flags parse: %s" % torture_flags["error"])
	if bool(torture_flags["ok"]):
		var args: Dictionary = torture_flags["args"]
		_check(String(args["velocity_dump"]) == "1", "bare velocity-dump flag")
		_check(String(args["reference"]) == "ssaa", "SSAA reference parsed")
		_check(String(args["msaa"]) == "8", "MSAA parsed")
		_check(String(args["mask"]) == "alpha_particles", "mask parsed")
		_check(String(args["upscaler"]) == "fsr2", "upscaler parsed")
		_check(is_equal_approx(float(args["scale"]), 0.5), "scale parsed")

	var engine_inferred: Dictionary = ArgsScript.parse(
		PackedStringArray(["--mode=capture", "--out=/tmp/out", "--seed=1", "--velocity-dump"]),
		PackedStringArray([
			"--path", "godotnext/testbed", "--resolution", "2560x1440", "--quit-after", "720",
			"res://scenes/upscaler_torture/upscaler_torture.tscn", "--",
			"--mode=capture", "--out=/tmp/out", "--seed=1", "--velocity-dump",
		])
	)
	_check(bool(engine_inferred["ok"]), "positional scene + quit-after inferred from engine args")
	if bool(engine_inferred["ok"]):
		var args: Dictionary = engine_inferred["args"]
		_check(String(args["scene"]) == "res://scenes/upscaler_torture/upscaler_torture.tscn", "engine scene inferred")
		_check(int(args["frames"]) == 600, "quit-after capture defaults to 600 frames")
		_check(args["output_resolution"] == [2560, 1440], "engine --resolution inferred")

	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=res://a.tscn", "--out=/tmp/out", "--frames=1", "--msaa=2",
	]))["ok"]), "invalid --msaa fails")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=res://a.tscn", "--out=/tmp/out", "--frames=1", "--mask=unknown",
	]))["ok"]), "invalid --mask fails")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=res://a.tscn", "--out=/tmp/out", "--frames=1", "--upscaler=bicubic",
	]))["ok"]), "invalid --upscaler fails")

	var ssaa_script = load("res://harness/ssaa_reference.gd")
	_check(not ssaa_script._has_upscaler_or_reference_flags(captured["args"]), "plain capture leaves SSAAReference idle")
	_check(ssaa_script._has_upscaler_or_reference_flags(torture_flags["args"]), "upscaler flags activate SSAAReference")


func _sample_manifest() -> Dictionary:
	return {
		"schema": 1,
		"host": "Apple M5 Max",
		"adapter": {"name": "adapter", "api_version": "api"},
		"commit": "abc",
		"command_line": {"engine": ["--path"], "user": ["--mode=capture"]},
		"seed": 1,
		"mode": "capture",
		"scene": "res://harness/selftest/selftest.tscn",
		"camera": "CaptureCam",
		"frames": 8,
		"warmup": 4,
		"capture_frames": [4, 7],
		"runner": {
			"clock": {"locked": false, "detail": "Clock locks belong to issue 237."},
			"power": {
				"on_ac": true,
				"powermode": 2,
				"powermode_name": "High Power",
				"thermal": "nominal",
				"before": {},
				"after": {},
			},
		},
		"scaling_3d_mode": 0,
		"scaling_3d_scale": 1.0,
		"output_size": [1280, 720],
	}


func _test_manifest() -> void:
	var sample := _sample_manifest()
	_check(ManifestScript.validate(sample).is_empty(), "manifest without dlss validates")

	var complete := sample.duplicate(true)
	complete["dlss"] = {
		"dll_path": "",
		"dll_version": "",
		"dll_sha256": "",
		"dll_sha_matches_manifest": false,
		"preset": "",
		"allow_ota_updates": false,
		"nvidia_driver": "",
		"override_checklist": {"screenshot": "", "state": "off"},
		"ngx_log": "",
	}
	_check(ManifestScript.validate(complete).is_empty(), "complete dlss object validates")

	var partial := sample.duplicate(true)
	partial["dlss"] = {"dll_path": ""}
	_check(not ManifestScript.validate(partial).is_empty(), "partial dlss object fails")

	var missing_scale := sample.duplicate(true)
	missing_scale.erase("scaling_3d_scale")
	_check(not ManifestScript.validate(missing_scale).is_empty(), "missing scaling_3d_scale fails")

	var bad_size := sample.duplicate(true)
	bad_size["output_size"] = [1280]
	_check(not ManifestScript.validate(bad_size).is_empty(), "short output_size fails")

	var extra := sample.duplicate(true)
	extra["note"] = "extra keys are allowed"
	_check(ManifestScript.validate(extra).is_empty(), "extra keys validate")

	var with_override := sample.duplicate(true)
	with_override["override"] = {
		"applied": true,
		"source": "res://scenes/feature_parity_room/override.cfg",
		"sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
	}
	_check(ManifestScript.validate(with_override).is_empty(), "applied override record validates")

	var without_override := sample.duplicate(true)
	without_override["override"] = {"applied": false, "source": "", "sha256": ""}
	_check(ManifestScript.validate(without_override).is_empty(), "unused override record validates")

	var partial_override := sample.duplicate(true)
	partial_override["override"] = {"applied": true}
	_check(not ManifestScript.validate(partial_override).is_empty(), "partial override record fails")

	var sourceless := sample.duplicate(true)
	sourceless["override"] = {"applied": true, "source": "", "sha256": ""}
	_check(not ManifestScript.validate(sourceless).is_empty(), "applied override without a source fails")

	_check(
		ManifestScript.override_source_for_scene("res://scenes/feature_parity_room/feature_parity_room.tscn")
		== "res://scenes/feature_parity_room/override.cfg",
		"override source sits next to the scene"
	)
	_check(ManifestScript.override_source_for_scene("res://a.tscn") == "res://override.cfg", "root scene override source")
	_check(ManifestScript.override_source_for_scene("not-a-scene") == "", "non-res scene has no override source")


func _test_percentiles() -> void:
	_check(ManifestScript.percentile(PackedFloat64Array(), 50.0) == 0.0, "empty percentile is 0")
	_check(ManifestScript.percentile(PackedFloat64Array([4.0]), 95.0) == 4.0, "single percentile is that value")
	var five := PackedFloat64Array([1.0, 2.0, 3.0, 4.0, 5.0])
	_check(is_equal_approx(ManifestScript.percentile(five, 50.0), 3.0), "p50 of 1..5 is 3")
	var hundred := PackedFloat64Array()
	for value in range(1, 101):
		hundred.append(float(value))
	_check(is_equal_approx(ManifestScript.percentile(hundred, 95.0), 95.05), "p95 of 1..100 is 95.05")

	var rows: Array = []
	for value in five:
		rows.append({"wall_ms": value, "passes": []})
	var summary: Dictionary = ManifestScript.summarize(rows, 2)
	_check(int(summary["frames"]) == 5, "summary frame count")
	_check(int(summary["warmup"]) == 2, "summary warmup")
	_check(is_equal_approx(float(summary["columns"]["wall_ms"]["p50"]), 3.0), "summary p50")
	_check(int(summary["columns"]["wall_ms"]["n"]) == 5, "summary n")


func _test_csv() -> void:
	var columns := ManifestScript.columns_for_passes(PackedStringArray(["Opaque Pass", "Opaque Pass", ""]))
	_check(columns == PackedStringArray(["opaque_pass_ms", "opaque_pass_2_ms", "pass_ms"]), "pass column names")
	var rows: Array = [
		{"wall_ms": 1.5, "passes": [{"column": "opaque_pass_ms", "ms": 0.25}]},
		{"wall_ms": 2.0, "passes": []},
	]
	var csv := ManifestScript.render_csv(rows)
	var lines := csv.split("\n")
	_check(lines[0] == "frame,wall_ms,opaque_pass_ms", "csv header")
	_check(lines[1] == "0,1.500000,0.250000", "csv first row")
	_check(lines[2] == "1,2.000000,", "csv blank gpu cell")


## Keep this fixture in sync with metrics/tests/test_timing_summary.py.
func _test_timing_fields() -> void:
	var rows: Array = [
		{"wall_ms": 1.0, "passes": [{"column": "as_updates_ms", "ms": 1.0}]},
		{"wall_ms": 1.0, "passes": [
			{"column": "as_updates_ms", "ms": 10.0},
			{"column": "blas_build_ms", "ms": 4.0},
		]},
		{"wall_ms": 1.0, "passes": [{"column": "as_updates_ms", "ms": 3.0}]},
	]
	var summary: Dictionary = ManifestScript.summarize(rows, 0, "Apple M5 Max", "Apple M5 Max", false)
	_check(int(summary["schema"]) == 1, "timing summary keeps schema 1")
	_check(is_equal_approx(float(summary["lines"]["1a"]["p95"]), 2.9), "steady 1a p95 is 2.9")
	_check(int(summary["lines"]["1a"]["n"]) == 2, "steady 1a excludes the 1b frame")
	_check(is_equal_approx(float(summary["lines"]["1b"]["p95"]), 4.0), "line 1b p95 is 4")
	_check(is_equal_approx(float(summary["diffuse_path"]["p95"]), 2.9), "diffuse path p95 is 2.9")
	var tagged: Dictionary = summary["line_1b_frames"]
	_check(tagged["frames"] == [1], "1b frame index")
	_check(is_equal_approx(float(tagged["p99"]), 14.0), "1b frame gpu p99 is 14")
	_check(String(tagged["device"]) == "Apple M5 Max", "M5 device")
	_check(is_equal_approx(float(tagged["limit_ms"]), 4.25), "M5 limit is 4.25")
	_check(tagged["within_limit"] == false, "14 ms is outside the M5 limit")
	var counters: Dictionary = summary["counters"]
	_check(String(counters["source"]) == "absent", "counters absent")
	for key in ["relight_texels", "shadow_rays", "q18", "tlas_fill_ms", "capture_cpu_ms"]:
		_check(counters[key] == null, "counter %s is null" % key)

	var ada: Dictionary = ManifestScript.timing_fields(rows, "", "RTX 4080 SUPER", false)
	_check(String(ada["line_1b_frames"]["device"]) == "RTX 4080 SUPER", "4080 device")
	_check(is_equal_approx(float(ada["line_1b_frames"]["limit_ms"]), 1.7), "4080 limit is 1.70")
	var blackwell: Dictionary = ManifestScript.timing_fields(rows, "RTX 5090", "RTX 4080 SUPER", false)
	_check(String(blackwell["line_1b_frames"]["device"]) == "RTX 5090", "5090 device")
	_check(blackwell["line_1b_frames"]["limit_ms"] == null, "5090 has no ms limit")
	_check(blackwell["line_1b_frames"]["within_limit"] == null, "5090 within_limit is null")


func _test_animation_frame() -> void:
	var node = load("res://harness/harness.gd").new()
	_check(not node.is_driving_animation(), "idle harness does not drive")
	node._active = true
	node._driving = true
	node._animation_frame = 12
	_check(node.is_driving_animation(), "driving harness publishes a frame")
	_check(int(node.get_animation_frame()) == 12, "animation frame is 12")
	node._active = false
	node.free()


## Capture mode keeps the scene's fog (#240). Volumetric fog is made
## deterministic by pinning its temporal reprojection off, not by
## turning the fog itself off.
func _test_capture_environment() -> void:
	var harness_script = load("res://harness/harness.gd")
	var environment := Environment.new()
	environment.fog_enabled = true
	environment.volumetric_fog_enabled = true
	environment.volumetric_fog_temporal_reprojection_enabled = true
	environment.glow_enabled = true
	harness_script.apply_capture_environment(environment)
	_check(environment.fog_enabled, "capture keeps depth fog on")
	_check(environment.volumetric_fog_enabled, "capture keeps volumetric fog on")
	_check(not environment.volumetric_fog_temporal_reprojection_enabled, "capture pins volumetric temporal reprojection off")
	_check(environment.tonemap_mode == Environment.TONE_MAPPER_LINEAR, "capture tonemap is linear")
	_check(is_equal_approx(environment.tonemap_exposure, 1.0), "capture exposure is 1")
	_check(not environment.glow_enabled, "capture glow is off")
	_check(not environment.adjustment_enabled, "capture adjustments are off")

	var fogless := Environment.new()
	harness_script.apply_capture_environment(fogless)
	_check(not fogless.fog_enabled, "capture leaves fogless scenes fogless")
	_check(not fogless.volumetric_fog_enabled, "capture leaves volumetric fogless scenes fogless")
