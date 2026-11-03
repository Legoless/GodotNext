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
