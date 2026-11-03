extends Node

## Capture and timing harness. Idle unless the user arguments contain --mode,
## so other testbed scenes are left alone.

const ArgsScript = preload("res://harness/args.gd")
const ManifestScript = preload("res://harness/manifest.gd")

const SETTLE_FLAT := 30
const SETTLE_BUDGET := 900
const SETTLE_HARD_STOP := 1020

enum Phase { BOOT, WAIT_SCENE, SETTLE, RELOAD, CAPTURE, TIMING }
enum WaitGoal { INITIAL, RELOAD }

var _active := false
var _failed := false
var _finalized := false
var _mode := ""
var _scene := ""
var _camera := ""
var _out := ""
var _seed_value := 1
var _frames := 0
var _warmup := 0
var _capture_frames: Array = []

var _phase := Phase.BOOT
var _wait_goal := WaitGoal.INITIAL
var _reload_from_id := 0

var _settle_draws := 0
var _watch_specialization := true
var _dropped_specialization := false
var _counter_value := {}
var _streak_by_counter := {}

var _capture_index := 0
var _movie_origin := -1
var _exr_saved := 0

var _last_usec := 0
var _timing_seen := 0
var _rows: Array = []
var _pending_timing_row := -1
var _seen_stamp_frames := {}
var _manifest_written := false
var _job := {}
var _driving := false
var _animation_frame := 0
var _nextgi_counters_present := false


func _ready() -> void:
	var parsed: Dictionary = ArgsScript.parse(OS.get_cmdline_user_args())
	if not bool(parsed["present"]):
		set_process(false)
		return
	if not bool(parsed["ok"]):
		_active = true
		_fail(String(parsed["error"]))
		return
	var args: Dictionary = parsed["args"]
	_active = true
	_mode = String(args["mode"])
	_scene = String(args["scene"])
	_camera = String(args["camera"])
	_out = String(args["out"])
	_seed_value = int(args["seed"])
	_frames = int(args["frames"])
	_warmup = int(args["warmup"])
	_capture_frames = args["capture_frames"]
	_load_job()
	DirAccess.make_dir_recursive_absolute(_out)
	OS.low_processor_usage_mode = false
	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)


func is_driving_animation() -> bool:
	return _active and not _failed and _driving


func get_animation_frame() -> int:
	return _animation_frame


func _process(_delta: float) -> void:
	if not _active or _failed:
		return
	if _phase == Phase.CAPTURE:
		_driving = true
		_animation_frame = _capture_index
	match _phase:
		Phase.BOOT:
			_boot()
		Phase.WAIT_SCENE:
			_poll_wait_scene()
		Phase.RELOAD:
			_start_reload()
		Phase.TIMING:
			_timing_tick()
		_:
			pass


func _exit_tree() -> void:
	if not _active or _finalized:
		return
	_finalized = true
	if _mode == "capture":
		_trim_movie_frames()
	_write_manifest()
	if _mode == "timing":
		_write_timing_files()
	if _failed:
		var tree := get_tree()
		if tree != null:
			tree.quit(1)


func _boot() -> void:
	_seed_window()
	if _mode == "capture":
		var viewport := get_viewport()
		if viewport != null:
			viewport.use_hdr_2d = true
	var current := get_tree().current_scene
	if current != null and current.scene_file_path == _scene:
		_enter_loaded_scene()
		return
	_request_scene(current)


func _enter_loaded_scene() -> void:
	if _mode == "capture":
		_phase = Phase.SETTLE
		_reset_settle()
		_apply_capture_view()
		print("harness: settling until pipeline compilations stay flat for %d frames" % SETTLE_FLAT)
	else:
		_begin_timing()


func _request_scene(current: Node) -> void:
	_reload_from_id = current.get_instance_id() if current != null else 0
	var err := get_tree().change_scene_to_file(_scene)
	if err != OK:
		_fail("cannot load scene %s (%s)" % [_scene, error_string(err)])
		return
	_phase = Phase.WAIT_SCENE
	_wait_goal = WaitGoal.INITIAL


func _poll_wait_scene() -> void:
	var current := get_tree().current_scene
	if current == null or current.scene_file_path != _scene:
		return
	if current.get_instance_id() == _reload_from_id:
		return
	if _wait_goal == WaitGoal.RELOAD:
		_phase = Phase.CAPTURE
		_capture_index = 0
		_driving = true
		_animation_frame = 0
		_seed_window()
		_apply_capture_view()
		return
	_enter_loaded_scene()


func _start_reload() -> void:
	var current := get_tree().current_scene
	_reload_from_id = current.get_instance_id() if current != null else 0
	_seed_window()
	var err := get_tree().change_scene_to_file(_scene)
	if err != OK:
		_fail("cannot reload scene %s (%s)" % [_scene, error_string(err)])
		return
	_phase = Phase.WAIT_SCENE
	_wait_goal = WaitGoal.RELOAD


func _begin_timing() -> void:
	_phase = Phase.TIMING
	_timing_seen = 0
	_driving = true
	_animation_frame = 0
	_nextgi_counters_present = RenderingServer.has_method("nextgi_get_counters")
	_last_usec = Time.get_ticks_usec()
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	if RenderingServer.get_rendering_device() == null:
		print("harness: no RenderingDevice; timing.csv will contain wall_ms only")


func _timing_tick() -> void:
	var now := Time.get_ticks_usec()
	var wall_ms := float(now - _last_usec) / 1000.0
	_last_usec = now
	if _timing_seen < _warmup:
		_animation_frame = 0
		_timing_seen += 1
		return
	if _rows.size() >= _frames:
		return
	_rows.append({"wall_ms": wall_ms, "passes": [], "counters": {}})
	_animation_frame = _rows.size() - 1
	_pending_timing_row = _rows.size() - 1
	if _rows.size() >= _frames:
		get_tree().quit(0 if not _failed else 1)


func _on_frame_post_draw() -> void:
	if not _active or _failed:
		return
	if _phase == Phase.SETTLE:
		_poll_settle()
		return
	if _phase == Phase.CAPTURE:
		_on_capture_presented()
		return
	if _pending_timing_row >= 0:
		var row_index := _pending_timing_row
		_pending_timing_row = -1
		_rows[row_index]["passes"] = _read_passes()
		_rows[row_index]["counters"] = _read_nextgi_counters()
		if row_index == 0:
			_write_manifest()


func _poll_settle() -> void:
	_settle_draws += 1
	var canvas := _counter_streak(RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_CANVAS)
	var mesh := _counter_streak(RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_MESH)
	var surface := _counter_streak(RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_SURFACE)
	var draw := _counter_streak(RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_DRAW)
	var spec := 0
	if _watch_specialization:
		spec = _counter_streak(RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_SPECIALIZATION)
	if _settle_draws % 60 == 0:
		print("harness: settle draws=%d streaks canvas=%d mesh=%d surface=%d draw=%d specialization=%d values canvas=%d mesh=%d surface=%d draw=%d specialization=%d" % [
			_settle_draws,
			canvas,
			mesh,
			surface,
			draw,
			spec,
			int(_counter_value.get(RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_CANVAS, 0)),
			int(_counter_value.get(RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_MESH, 0)),
			int(_counter_value.get(RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_SURFACE, 0)),
			int(_counter_value.get(RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_DRAW, 0)),
			int(_counter_value.get(RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_SPECIALIZATION, 0)),
		])
	var core_flat := canvas >= SETTLE_FLAT and mesh >= SETTLE_FLAT and surface >= SETTLE_FLAT and draw >= SETTLE_FLAT
	var spec_flat := (not _watch_specialization) or spec >= SETTLE_FLAT
	if core_flat and spec_flat:
		print("harness: pipeline counters stayed flat for %d frames after %d draws" % [SETTLE_FLAT, _settle_draws])
		_phase = Phase.RELOAD
		return
	if _watch_specialization and _settle_draws >= SETTLE_BUDGET and core_flat:
		_watch_specialization = false
		_dropped_specialization = true
		print("harness: specialization counter did not stay flat for %d frames within %d draws; settling on canvas, mesh, surface, and draw" % [SETTLE_FLAT, SETTLE_BUDGET])
		_phase = Phase.RELOAD
		return
	if _settle_draws >= SETTLE_HARD_STOP:
		_fail("pipeline compilations did not settle within %d draws" % SETTLE_HARD_STOP)


func _counter_streak(key: int) -> int:
	var value := int(RenderingServer.get_rendering_info(key))
	if not _counter_value.has(key) or int(_counter_value[key]) != value:
		_counter_value[key] = value
		_streak_by_counter[key] = 1
	else:
		_streak_by_counter[key] = int(_streak_by_counter[key]) + 1
	return int(_streak_by_counter[key])


func _reset_settle() -> void:
	_settle_draws = 0
	_watch_specialization = true
	_counter_value = {}
	_streak_by_counter = {}


func _on_capture_presented() -> void:
	var window := _warmup + _frames
	if _capture_index >= window:
		return
	var slot := _capture_index
	_capture_index += 1
	_on_capture_drawn(slot)
	if _capture_index >= window:
		var tree := get_tree()
		if tree != null:
			tree.quit(0 if not _failed else 1)


func _on_capture_drawn(slot: int) -> void:
	if slot == 0:
		# MovieWriterPNGWAV numbers PNGs from zero, one file per drawn iteration.
		# get_frames_drawn() is still the count of draws completed before this one
		# while frame_post_draw is running, which is the PNG index about to be written.
		_movie_origin = int(Engine.get_frames_drawn())
		print("harness: captured window started at movie frame %d" % _movie_origin)
	if _capture_frames.has(slot):
		_save_exr(slot)
	if slot == 0:
		_write_manifest()


func _save_exr(slot: int) -> void:
	var viewport := get_viewport()
	if viewport == null or viewport.get_texture() == null:
		_fail("viewport texture is missing")
		return
	var image := viewport.get_texture().get_image()
	if image == null:
		_fail("viewport image is missing")
		return
	var path := _out.path_join("hdr_%05d.exr" % slot)
	# color_image false keeps the linear viewport values, including negatives.
	var err := image.save_exr(path, false, false, -1.0)
	if err != OK:
		_fail("failed to save %s (%s)" % [path, error_string(err)])
		return
	_exr_saved += 1


func _read_passes() -> Array:
	var device := RenderingServer.get_rendering_device()
	if device == null:
		return []
	var count := int(device.get_captured_timestamps_count())
	if count <= 1:
		return []
	var stamp_frame := int(device.get_captured_timestamps_frame())
	if _seen_stamp_frames.has(stamp_frame):
		return []
	_seen_stamp_frames[stamp_frame] = true
	var names := PackedStringArray()
	var times: Array = []
	for index in count:
		names.append(device.get_captured_timestamp_name(index))
		times.append(int(device.get_captured_timestamp_gpu_time(index)))
	var pass_names := PackedStringArray()
	for index in count - 1:
		pass_names.append(names[index])
	var columns := ManifestScript.columns_for_passes(pass_names)
	var passes: Array = []
	for index in count - 1:
		var delta := int(times[index + 1]) - int(times[index])
		if delta < 0:
			delta = 0
		passes.append({
			"column": String(columns[index]),
			"ms": float(delta) / 1000000.0,
		})
	return passes


func _read_nextgi_counters() -> Dictionary:
	if not RenderingServer.has_method("nextgi_get_counters"):
		return {}
	var value = RenderingServer.call("nextgi_get_counters")
	if typeof(value) != TYPE_DICTIONARY:
		return {}
	return (value as Dictionary).duplicate()


func _apply_capture_view() -> void:
	var viewport := get_viewport()
	if viewport == null:
		return
	viewport.use_hdr_2d = true
	viewport.use_debanding = false
	viewport.use_taa = false
	viewport.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	var scene := get_tree().current_scene
	if scene == null:
		return
	for node in scene.find_children("*", "WorldEnvironment", true, true):
		var environment := (node as WorldEnvironment).environment
		if environment == null:
			continue
		environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
		environment.tonemap_exposure = 1.0
		environment.glow_enabled = false
		environment.adjustment_enabled = false
		environment.ssao_enabled = false
		environment.ssil_enabled = false
		environment.ssr_enabled = false
		environment.fog_enabled = false
		environment.volumetric_fog_enabled = false
	_activate_camera(scene)


func _activate_camera(scene: Node) -> void:
	if _camera.is_empty():
		return
	var node := scene.get_node_or_null(NodePath(_camera))
	if node == null:
		var matches := scene.find_children(_camera, "Camera3D", true, true)
		if matches.is_empty():
			_fail("camera not found: %s" % _camera)
			return
		node = matches[0]
	if node is Camera3D:
		(node as Camera3D).current = true


func _seed_window() -> void:
	seed(_seed_value)
	var root := get_tree().root if get_tree() != null else null
	if root != null:
		_seed_nodes(root)


func _seed_nodes(node: Node) -> void:
	for property in node.get_property_list():
		if String(property["name"]) == "seed" and int(property["type"]) == TYPE_INT:
			node.set("seed", _seed_value)
			break
	for child in node.get_children():
		_seed_nodes(child)


func _load_job() -> void:
	var path := OS.get_environment("GODOTNEXT_HARNESS_JOB")
	if path.is_empty():
		_job = {}
		return
	var text := FileAccess.get_file_as_string(path)
	if text.is_empty():
		_fail("runner job file is empty: %s" % path)
		return
	var parsed = JSON.parse_string(text)
	if typeof(parsed) != TYPE_DICTIONARY:
		_fail("runner job file is not an object: %s" % path)
		return
	_job = parsed


func _build_manifest() -> Dictionary:
	var viewport := get_viewport()
	var scaling_mode := 0
	var scaling_scale := 1.0
	var output_size: Array = [0, 0]
	if viewport != null:
		scaling_mode = int(viewport.scaling_3d_mode)
		scaling_scale = float(viewport.scaling_3d_scale)
		var visible := viewport.get_visible_rect().size
		output_size = [int(visible.x), int(visible.y)]
	var version := Engine.get_version_info()
	var host := String(_job.get("host", OS.get_name()))
	var clock: Dictionary = _job.get("clock", {"locked": false, "detail": "no runner job"})
	var power: Dictionary = _job.get("power", {
		"on_ac": false,
		"powermode": -1,
		"powermode_name": "unknown",
		"thermal": "unknown",
		"before": {},
	})
	if not power.has("after") or typeof(power["after"]) != TYPE_DICTIONARY:
		power = power.duplicate(true)
		power["after"] = {}
	var manifest := {
		"schema": ManifestScript.SCHEMA,
		"host": host,
		"adapter": {
			"name": RenderingServer.get_video_adapter_name(),
			"api_version": RenderingServer.get_video_adapter_api_version(),
		},
		"commit": String(version.get("hash", "")),
		"command_line": {
			"engine": _string_array(OS.get_cmdline_args()),
			"user": _string_array(OS.get_cmdline_user_args()),
		},
		"seed": _seed_value,
		"mode": _mode,
		"scene": _scene,
		"camera": _camera,
		"frames": _frames,
		"warmup": _warmup,
		"capture_frames": _capture_frames.duplicate(),
		"runner": {
			"clock": clock,
			"power": power,
		},
		"scaling_3d_mode": scaling_mode,
		"scaling_3d_scale": scaling_scale,
		"output_size": output_size,
	}
	if _job.has("dlss") and typeof(_job["dlss"]) == TYPE_DICTIONARY:
		manifest["dlss"] = (_job["dlss"] as Dictionary).duplicate(true)
	return manifest


func _write_manifest() -> void:
	if _out.is_empty():
		return
	var manifest := _build_manifest()
	var errors := ManifestScript.validate(manifest)
	if errors.size() > 0:
		_fail("manifest failed validation: %s" % ", ".join(errors))
		return
	var path := _out.path_join("manifest.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		_fail("cannot write %s" % path)
		return
	file.store_string(JSON.stringify(manifest, "\t"))
	file.store_string("\n")
	file.close()
	_manifest_written = true


func _write_timing_files() -> void:
	if _out.is_empty():
		return
	if _rows.size() != _frames:
		_fail("timing recorded %d rows, expected %d" % [_rows.size(), _frames])
	var csv_path := _out.path_join("timing.csv")
	var csv_file := FileAccess.open(csv_path, FileAccess.WRITE)
	if csv_file == null:
		_fail("cannot write %s" % csv_path)
		return
	csv_file.store_string(ManifestScript.render_csv(_rows))
	csv_file.close()
	var host := String(_job.get("host", OS.get_name()))
	var adapter := RenderingServer.get_video_adapter_name()
	var summary := ManifestScript.summarize(_rows, _warmup, host, adapter, _nextgi_counters_present)
	var summary_path := _out.path_join("summary.json")
	var summary_file := FileAccess.open(summary_path, FileAccess.WRITE)
	if summary_file == null:
		_fail("cannot write %s" % summary_path)
		return
	summary_file.store_string(JSON.stringify(summary, "\t"))
	summary_file.store_string("\n")
	summary_file.close()
	if not _failed:
		print("harness: timing complete rows=%d columns=%d" % [_rows.size(), summary["columns"].size()])


func _trim_movie_frames() -> void:
	if _failed and _movie_origin < 0:
		return
	var movie_path := Engine.get_write_movie_path()
	if movie_path.is_empty():
		print("harness: capture had no movie writer path; PNG trim skipped")
		return
	if movie_path.is_relative_path():
		movie_path = ProjectSettings.globalize_path("res://".path_join(movie_path))
	var stem := movie_path.get_basename()
	var keep := _warmup + _frames
	if _movie_origin < 0:
		_fail("captured window never started")
		return
	for index in keep:
		var source := "%s%08d.png" % [stem, _movie_origin + index]
		var temp := "%s.keep_%08d.png" % [stem, index]
		if not FileAccess.file_exists(source):
			_fail("missing movie frame %s" % source.get_file())
			return
		var err := DirAccess.rename_absolute(source, temp)
		if err != OK:
			_fail("cannot stage %s (%s)" % [source.get_file(), error_string(err)])
			return
	_delete_movie_pngs(stem)
	for index in keep:
		var temp := "%s.keep_%08d.png" % [stem, index]
		var final_path := "%s%08d.png" % [stem, index]
		var err := DirAccess.rename_absolute(temp, final_path)
		if err != OK:
			_fail("cannot rename kept frame %s" % final_path.get_file())
			return
	var wav_path := stem + ".wav"
	if FileAccess.file_exists(wav_path):
		DirAccess.remove_absolute(wav_path)
	if not _failed:
		var expected_exr := 0
		for slot in _capture_frames:
			if int(slot) < keep:
				expected_exr += 1
		if _exr_saved != expected_exr:
			_fail("saved %d EXR files, expected %d" % [_exr_saved, expected_exr])
			return
		print("harness: capture complete kept=%d exr=%d" % [keep, _exr_saved])
		if _dropped_specialization:
			print("harness: capture settled without the specialization counter")


func _delete_movie_pngs(stem: String) -> void:
	var dir_path := stem.get_base_dir()
	var prefix := stem.get_file()
	var dir := DirAccess.open(dir_path)
	if dir == null:
		_fail("cannot open movie directory")
		return
	var doomed: Array = []
	dir.list_dir_begin()
	var file_name := dir.get_next()
	while file_name != "":
		if not dir.current_is_dir() and _is_movie_png(prefix, file_name):
			doomed.append(file_name)
		file_name = dir.get_next()
	dir.list_dir_end()
	for doomed_name in doomed:
		DirAccess.remove_absolute(dir_path.path_join(String(doomed_name)))


func _is_movie_png(prefix: String, file_name: String) -> bool:
	if not file_name.begins_with(prefix) or not file_name.ends_with(".png"):
		return false
	var middle := file_name.substr(prefix.length(), file_name.length() - prefix.length() - 4)
	return middle.length() == 8 and middle.is_valid_int()


func _string_array(values: PackedStringArray) -> Array:
	var out: Array = []
	for value in values:
		out.append(String(value))
	return out


func _fail(message: String) -> void:
	if not _failed:
		_failed = true
		print("harness: " + message)
	var tree := get_tree()
	if tree != null:
		tree.quit(1)
