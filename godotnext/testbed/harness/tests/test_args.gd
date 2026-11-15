extends SceneTree

## Parser, manifest schema, percentile, and CSV checks. No GPU and no window.

const ArgsScript = preload("res://harness/args.gd")
const ManifestScript = preload("res://harness/manifest.gd")
const ReferencePTScript = preload("res://harness/reference_pt.gd")

var _failures := 0


func _initialize() -> void:
	_test_parse()
	_test_manifest()
	_test_percentiles()
	_test_csv()
	_test_timing_fields()
	_test_animation_frame()
	_test_capture_environment()
	_test_output_size()
	_test_reference_pt_args()
	_test_reference_pt_statistics()
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
	var tsr_flags: Dictionary = ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=res://scenes/upscaler_torture/upscaler_torture.tscn", "--out=/tmp/out", "--frames=8",
		"--upscaler=tsr", "--scale=0.667",
	]))
	_check(bool(tsr_flags["ok"]) and String(tsr_flags["args"]["upscaler"]) == "tsr", "--upscaler=tsr parses")
	_test_gt_mask_args()

	var ssaa_script = load("res://harness/ssaa_reference.gd")
	_check(not ssaa_script._has_upscaler_or_reference_flags(captured["args"]), "plain capture leaves SSAAReference idle")
	_check(ssaa_script._has_upscaler_or_reference_flags(torture_flags["args"]), "upscaler flags activate SSAAReference")


## The GT-2 content masks and the TSR preset flag (#190).
func _test_gt_mask_args() -> void:
	var base := ["--mode=capture", "--scene=res://scenes/upscaler_torture/upscaler_torture.tscn", "--out=/tmp/out", "--frames=8"]
	for mask in ["moire", "thin_wire"]:
		var parsed: Dictionary = ArgsScript.parse(PackedStringArray(base + ["--mask=" + mask, "--variant=static"]))
		_check(bool(parsed["ok"]) and String(parsed["args"]["mask"]) == mask, "--mask=%s parses" % mask)
		if bool(parsed["ok"]):
			_check(String(parsed["args"]["variant"]) == "static", "--variant=static rides along with --mask=%s" % mask)
	var preset: Dictionary = ArgsScript.parse(PackedStringArray(base + ["--upscaler=tsr", "--scale=0.667", "--tsr-quality=high"]))
	_check(bool(preset["ok"]) and String(preset["args"]["tsr_quality"]) == "high", "--tsr-quality=high parses")
	_check(not bool(ArgsScript.parse(PackedStringArray(base + ["--upscaler=tsr", "--tsr-quality=ultra"]))["ok"]), "unknown --tsr-quality fails")
	_check(not bool(ArgsScript.parse(PackedStringArray(base + ["--upscaler=fsr2", "--tsr-quality=high"]))["ok"]), "--tsr-quality without --upscaler=tsr fails")
	_check(not bool(ArgsScript.parse(PackedStringArray([
		"--mode=capture", "--scene=res://a.tscn", "--out=/tmp/out", "--reference=pt", "--tsr-quality=high",
	]))["ok"]), "--tsr-quality does not apply to --reference=pt")
	var ssaa_script = load("res://harness/ssaa_reference.gd")
	_check(ssaa_script.LAYERS.has("moire") and ssaa_script.LAYERS.has("thin_wire"), "SSAAReference knows the GT-2 mask layers")
	var generator = load("res://generators/upscaler_torture.gd")
	_check(int(ssaa_script.LAYERS["moire"]) == int(generator.MOIRE_LAYER), "moire mask layer matches the generator")
	_check(int(ssaa_script.LAYERS["thin_wire"]) == int(generator.THIN_WIRE_LAYER), "thin_wire mask layer matches the generator")
	_check(ssaa_script.OCCLUDED_MASKS.has("moire") and ssaa_script.OCCLUDED_MASKS.has("thin_wire"), "GT-2 masks are drawn with occluders")
	_check(not ssaa_script.OCCLUDED_MASKS.has("alpha_particles"), "the #95 masks keep their layer-only form")


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


## The output size check (#270): where the requested size comes from, and the failure message.
func _test_output_size() -> void:
	var harness_script = load("res://harness/harness.gd")
	_check(harness_script.requested_size_from_job({"resolution": [3840.0, 2160.0]}) == Vector2i(3840, 2160), "runner record resolution is the requested size")
	_check(harness_script.requested_size_from_job({}) == Vector2i.ZERO, "no runner record resolution requests nothing")
	_check(harness_script.requested_size_from_job({"resolution": [3840]}) == Vector2i.ZERO, "a short resolution requests nothing")
	_check(harness_script.requested_size_from_job({"resolution": [0, 2160]}) == Vector2i.ZERO, "a zero width requests nothing")
	_check(harness_script.requested_size_from_job({"resolution": "3840x2160"}) == Vector2i.ZERO, "a string resolution requests nothing")

	var clamped: String = harness_script.output_size_error(Vector2i(3840, 2160), Vector2i(3840, 2135))
	_check(not clamped.is_empty(), "a clamped viewport fails")
	_check(clamped.contains("3840x2135") and clamped.contains("3840x2160"), "the mismatch message names both sizes: " + clamped)
	_check(String(harness_script.output_size_error(Vector2i(2560, 1440), Vector2i(2560, 1440))).is_empty(), "a matching viewport passes")
	_check(not String(harness_script.output_size_error(Vector2i(1920, 1080), Vector2i(1080, 1920))).is_empty(), "a transposed viewport fails")

	var folder := ProjectSettings.globalize_path("user://test_args_output_size")
	DirAccess.make_dir_recursive_absolute(folder)
	var stem := folder.path_join("frame")
	var frame_path := "%s%08d.png" % [stem, 0]
	DirAccess.remove_absolute(frame_path)
	_check(harness_script.movie_frame_size(stem) == Vector2i.ZERO, "a missing first movie frame has no size")
	var frame := Image.create_empty(48, 27, false, Image.FORMAT_RGBA8)
	_check(frame.save_png(frame_path) == OK, "the test movie frame is written")
	_check(harness_script.movie_frame_size(stem) == Vector2i(48, 27), "the first movie frame's size is read")
	DirAccess.remove_absolute(frame_path)
	DirAccess.remove_absolute(folder)


## --reference=pt (#263): the path-traced scene reference.
func _test_reference_pt_args() -> void:
	var base := ["--mode=capture", "--scene=res://scenes/color_box_room/cbr_a.tscn", "--out=/tmp/out", "--camera=CovCam0"]
	var reference: Dictionary = ArgsScript.parse(PackedStringArray(base + ["--reference=pt"]))
	_check(bool(reference["ok"]), "--reference=pt parses without --frames: %s" % reference["error"])
	if bool(reference["ok"]):
		var args: Dictionary = reference["args"]
		_check(String(args["reference"]) == "pt", "pt reference parsed")
		_check(int(args["spp"]) == 4096, "--spp defaults to 4096")
		_check(int(args["frames"]) == 1, "pt frames default to 1")
	var short: Dictionary = ArgsScript.parse(PackedStringArray(base + ["--reference=pt", "--spp=512", "--hold-frame=32"]))
	_check(bool(short["ok"]) and int(short["args"]["spp"]) == 512, "--spp=512 parses")
	if bool(short["ok"]):
		_check(int(short["args"]["hold_frame"]) == 32, "pt keeps --hold-frame")
	for bad_spp in ["--spp=0", "--spp=100", "--spp=4000", "--spp=-512", "--spp=many"]:
		var parsed: Dictionary = ArgsScript.parse(PackedStringArray(base + ["--reference=pt", bad_spp]))
		_check(not bool(parsed["ok"]) and String(parsed["error"]) == "--spp must be a positive multiple of 512", "%s fails" % bad_spp)
	var stray: Dictionary = ArgsScript.parse(PackedStringArray(base + ["--frames=1", "--spp=4096"]))
	_check(not bool(stray["ok"]) and String(stray["error"]) == "--spp requires --reference=pt", "--spp without pt fails")
	var unknown: Dictionary = ArgsScript.parse(PackedStringArray(base + ["--frames=1", "--reference=lightmap"]))
	_check(not bool(unknown["ok"]) and String(unknown["error"]) == "--reference must be ssaa or pt", "unknown reference fails")
	var timing: Dictionary = ArgsScript.parse(PackedStringArray([
		"--mode=timing", "--scene=res://a.tscn", "--out=/tmp/out", "--frames=1", "--reference=pt",
	]))
	_check(not bool(timing["ok"]) and String(timing["error"]) == "--reference=pt requires --mode=capture", "pt timing fails")
	for flag in ["--upscaler=fsr2", "--msaa=8", "--scale=0.5", "--mask=alpha_particles", "--capture-frames=0", "--velocity-dump"]:
		var parsed: Dictionary = ArgsScript.parse(PackedStringArray(base + ["--reference=pt", flag]))
		_check(not bool(parsed["ok"]) and String(parsed["error"]).ends_with("does not apply to --reference=pt"), "%s with pt fails" % flag)
	var ssaa_script = load("res://harness/ssaa_reference.gd")
	_check(ssaa_script._has_upscaler_or_reference_flags(reference["args"]), "pt keeps SSAAReference on for native 1x")
	_check(ReferencePTScript.q1_patch_path("res://scenes/color_box_room/cbr_a.tscn") == "res://scenes/color_box_room/cbr_a_patches_q1.json", "patch file next to the scene")


func _test_reference_pt_statistics() -> void:
	# A disc of radius 0.25 at z = -2 facing the camera.
	var center := Vector3(0, 0, -2)
	var normal := Vector3(0, 0, 1)
	_check(is_equal_approx(ReferencePTScript.disc_hit_distance(Vector3.ZERO, Vector3(0, 0, -1), center, normal, 0.25), 2.0), "disc hit at its center")
	_check(ReferencePTScript.disc_hit_distance(Vector3.ZERO, Vector3(0.2, 0, -1).normalized(), center, normal, 0.25) < 0.0, "ray outside the radius misses")
	_check(ReferencePTScript.disc_hit_distance(Vector3(0, 0, -4), Vector3(0, 0, 1), center, normal, 0.25) < 0.0, "the back of a disc is not a hit")
	_check(ReferencePTScript.disc_hit_distance(Vector3(0, 0, -3), Vector3(0, 0, -1), center, normal, 0.25) < 0.0, "a disc behind the ray is not a hit")

	var ids := PackedInt32Array([-1, 0, 255, 300])
	var bytes := ReferencePTScript.encode_mask(ids)
	_check(bytes.size() == 12, "mask has three bytes per pixel")
	_check(bytes[0] == 0 and bytes[1] == 0, "no patch encodes 0")
	_check(bytes[3] == 1 and bytes[4] == 0, "patch 0 encodes 1")
	_check(bytes[6] == 0 and bytes[7] == 1, "patch 255 encodes 256")
	_check(bytes[9] + 256 * bytes[10] == 301, "patch 300 encodes 301")

	# Two pixels with sums (1, 1, 1) and (2, 2, 2) after 64 samples; luminance is linear.
	var data := PackedFloat32Array([1, 1, 1, 64, 2, 2, 2, 64])
	var sums := ReferencePTScript.patch_luminance_sums(data, [PackedInt32Array([0, 1])])
	_check(is_equal_approx(sums[0], 3.0), "patch luminance sum is 3")
	var image: Image = ReferencePTScript.mean_image(data, 2, 1, 2.0)
	_check(image.get_format() == Image.FORMAT_RGBH, "reference image is half float")
	_check(is_equal_approx(image.get_pixel(1, 0).r, 2.0 * 2.0 / 64.0), "mean times exposure")

	# Checkpoint sums of one 2-pixel patch at 0, 32 and 64 samples of 32 each.
	var checkpoints := [PackedFloat64Array([0.0]), PackedFloat64Array([64.0]), PackedFloat64Array([192.0])]
	var means: Array = ReferencePTScript.batch_patch_means(checkpoints, [PackedInt32Array([0, 1])], 32, 1.0)
	_check(means.size() == 2, "one mean per batch")
	_check(is_equal_approx((means[0] as PackedFloat64Array)[0], 1.0), "batch 1 mean is 64 / (32 * 2)")
	_check(is_equal_approx((means[1] as PackedFloat64Array)[0], 2.0), "batch 2 mean is 128 / (32 * 2)")

	# Indirect batches 1.0 and 1.2 (direct 0.5): mean 1.1, SE = sqrt(0.02 / (2 - 1) / 2) = 0.1.
	var full := [PackedFloat64Array([1.5, 0.5]), PackedFloat64Array([1.7, 0.5])]
	var direct := [PackedFloat64Array([0.5, 0.5]), PackedFloat64Array([0.5, 0.5])]
	var stats: Dictionary = ReferencePTScript.batch_statistics(full, direct)
	var luminance: PackedFloat64Array = stats["luminance"]
	var relative: Array = stats["relative_se"]
	_check(is_equal_approx(luminance[0], 1.1), "indirect mean is full minus direct")
	var expected := sqrt((0.01 + 0.01) / 1.0 / 2.0) / 1.1
	_check(absf(float(relative[0]) - expected) < 1e-9, "relative SE is std / sqrt(batches) / mean")
	_check(is_equal_approx(luminance[1], 0.0) and float(relative[1]) == 0.0, "zero indirect with no spread has zero SE")
	var spread: Dictionary = ReferencePTScript.batch_statistics([PackedFloat64Array([0.6]), PackedFloat64Array([0.4])], [PackedFloat64Array([0.5]), PackedFloat64Array([0.5])])
	_check((spread["relative_se"] as Array)[0] == null, "zero mean with spread has no finite relative SE")
