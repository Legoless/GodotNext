extends Node3D
## Slow IO Flight runtime (#74).
##
## The camera follows the seeded Dense City spline by frame index. The
## delayed variant records uniform 50-250 ms per request, applied in wall
## time by the streamer (#78). The nodelay variant is the same path with
## the delay fixed at 0 (G5-2). Page-upload samples stay null until #78
## exposes `virtual_geometry_get_routing_counters`.

const Flight = preload("res://scenes/slow_io_flight/flight.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")

var _variant := "delayed"
var _hold := -1
var _local_frame := 0
var _saw := false
var _page_upload: Array = []
var _routing_source := "absent"


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	process_priority = 1
	var requested := SceneArgs.value("--variant", "delayed")
	if requested in Flight.VARIANTS:
		_variant = requested
	else:
		push_error("slow_io_flight: unknown variant '%s', using delayed." % requested)
		_variant = "delayed"
	_hold = SceneArgs.int_value("--hold-frame", -1)
	var setting_present := Flight.apply_delay_setting(_variant)
	var record: Dictionary = Flight.delay_record(_variant)
	print("slow_io_flight: variant=%s frames=%d delay=%d-%d ms applied_in=%s setting_present=%s" % [
		_variant,
		Flight.FLIGHT_FRAMES,
		int(record["min_ms"]),
		int(record["max_ms"]),
		String(record["applied_in"]),
		str(setting_present),
	])
	_apply(0 if _hold < 0 else _hold)
	_maybe_instance_city()


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, _hold)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	_apply(int(stepped["frame"]))
	_sample_routing()


func _maybe_instance_city() -> void:
	# The saved scene keeps stand-in boxes. Pass --with-city=1 after the
	# Dense City generator has written its scene. Capture identity runs
	# leave this off.
	if SceneArgs.value("--with-city", "0") != "1":
		return
	if not ResourceLoader.exists(Flight.DENSE_CITY_SCENE):
		push_warning("slow_io_flight: %s is not generated; flying the stand-in." % Flight.DENSE_CITY_SCENE)
		return
	var packed := load(Flight.DENSE_CITY_SCENE) as PackedScene
	if packed == null:
		push_warning("slow_io_flight: dense city scene did not load.")
		return
	var city := packed.instantiate()
	city.name = "DenseCity"
	add_child(city)


func _apply(p_frame: int) -> void:
	var camera := get_node_or_null("FlightCam") as Camera3D
	if camera == null:
		return
	camera.global_transform = Flight.camera_transform(p_frame)
	camera.set_meta("flight_frame", p_frame)


func _sample_routing() -> void:
	if not RenderingServer.has_method(Flight.PAGE_UPLOAD_METHOD):
		return
	_routing_source = Flight.PAGE_UPLOAD_METHOD
	var row: Variant = RenderingServer.call(Flight.PAGE_UPLOAD_METHOD)
	if row is Dictionary and (row as Dictionary).has("page_upload_ms"):
		_page_upload.append((row as Dictionary)["page_upload_ms"])


func _exit_tree() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var out := SceneArgs.value("--out", "")
	if out.is_empty() or not out.is_absolute_path():
		return
	var record: Dictionary = Flight.delay_record(_variant)
	record["setting_present"] = ProjectSettings.has_setting(Flight.DELAY_SETTING)
	var report := {
		"schema": 1,
		"variant": _variant,
		"seed": int(get_meta("seed", 0)),
		"frames": Flight.FLIGHT_FRAMES,
		"fps": Flight.FLIGHT_FPS,
		"seconds": float(Flight.FLIGHT_FRAMES) / float(Flight.FLIGHT_FPS),
		"debug_io_delay_ms": record,
		"page_upload_ms": _page_upload if _routing_source != "absent" else null,
		"page_upload_source": _routing_source,
		"running_from_pck": not OS.has_feature("editor"),
		"dense_city_scene": Flight.DENSE_CITY_SCENE,
		"geometry_cache_setting": Flight.GEOMETRY_CACHE_SETTING,
		"geometry_cache_mb": Flight.GEOMETRY_CACHE_MB,
	}
	DirAccess.make_dir_recursive_absolute(out)
	var path := out.path_join("slow_io_params.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("slow_io_flight: cannot write %s." % path)
		return
	file.store_string(JSON.stringify(report, "\t"))
	file.close()
