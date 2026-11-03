extends SceneTree
## Q15 collector (#74).
##
## Calls `RenderingServer.virtual_geometry_get_memory_report` after a
## 120-frame warmup on the Dense City fixed-camera poses when that method
## exists. Until #78/#79 land, the method is absent and this script writes
## the ten category names with `source: absent` and does not wait.
##
## One invocation fills one export (`--export-mode=`). Repeat for
## `strip_authored_to_coarse`, `discrete_lod` and `retain_authored`.
## `--out` is an absolute directory. Without it, the JSON is printed.

const Flight = preload("res://scenes/slow_io_flight/flight.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")

const WARMUP_FRAMES := 120
const EXPORTS: Array[String] = ["strip_authored_to_coarse", "discrete_lod", "retain_authored"]
const CATEGORIES: Array[String] = [
	"authored_remainder", "lods", "shadow_mesh", "coarse_fallback", "rt_arena",
	"blas", "tlas", "as_scratch", "dag_headers", "page_cache",
]

var _export_mode := "strip_authored_to_coarse"
var _root: Node3D


func _init() -> void:
	_boot.call_deferred()


func _boot() -> void:
	var requested := SceneArgs.value("--export-mode", _export_mode)
	if requested in EXPORTS:
		_export_mode = requested
	else:
		push_error("collect_memory: unknown export mode '%s'." % requested)
		quit(1)
		return
	if not RenderingServer.has_method(Flight.MEMORY_METHOD):
		_write(_absent_exports())
		print("collect_memory: %s is absent; wrote the 10 categories with source absent." % Flight.MEMORY_METHOD)
		quit(0)
		return
	_root = Node3D.new()
	_root.name = "MemoryCameras"
	get_root().add_child(_root)
	_add_camera("cam_main", Vector3(-260.0, 2.0, 0.0), Vector3(200.0, 14.0, 0.0), true)
	_add_camera("cam_aerial", Vector3(-560.0, 430.0, -560.0), Vector3(0.0, 0.0, 0.0), false)
	for _frame in WARMUP_FRAMES:
		await process_frame
	var report: Variant = RenderingServer.call(Flight.MEMORY_METHOD)
	var exports := _load_existing_exports()
	exports[_export_mode] = {
		"source": Flight.MEMORY_METHOD,
		"categories": _categories_from(report),
	}
	_write(exports)
	print("collect_memory: warmup=%d export=%s." % [WARMUP_FRAMES, _export_mode])
	quit(0)


func _add_camera(p_name: String, p_position: Vector3, p_target: Vector3, p_current: bool) -> void:
	var camera := Camera3D.new()
	camera.name = p_name
	var direction := (p_target - p_position).normalized()
	camera.transform = Transform3D(Basis.looking_at(direction, Vector3.UP), p_position)
	camera.current = p_current
	_root.add_child(camera)


func _categories_from(p_report: Variant) -> Dictionary:
	var out := {}
	var row: Dictionary = p_report if p_report is Dictionary else {}
	for name in CATEGORIES:
		out[name] = row[name] if row.has(name) else null
	return out


func _load_existing_exports() -> Dictionary:
	var out := SceneArgs.value("--out", "")
	if out.is_empty() or not out.is_absolute_path():
		return _absent_exports()
	var path := out.path_join("memory_report.json")
	if not FileAccess.file_exists(path):
		return _absent_exports()
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	if parsed is Dictionary and (parsed as Dictionary).get("exports") is Dictionary:
		return (parsed as Dictionary)["exports"]
	return _absent_exports()


func _absent_exports() -> Dictionary:
	var exports := {}
	for mode in EXPORTS:
		exports[mode] = {"source": "absent", "categories": _null_categories()}
	return exports


func _null_categories() -> Dictionary:
	var out := {}
	for name in CATEGORIES:
		out[name] = null
	return out


func _write(p_exports: Dictionary) -> void:
	var body := {
		"schema": 1,
		"warmup_frames": WARMUP_FRAMES,
		"cameras": ["cam_main", "cam_aerial"],
		"source": Flight.MEMORY_METHOD if RenderingServer.has_method(Flight.MEMORY_METHOD) else "absent",
		"exports": p_exports,
		"note": "retain_authored is reported and is not part of the stripped/baseline ratio. #78/#79 fill the byte counts.",
	}
	var text := JSON.stringify(body, "\t")
	var out := SceneArgs.value("--out", "")
	if out.is_empty():
		print(text)
		return
	if not out.is_absolute_path():
		push_error("collect_memory: --out must be absolute.")
		return
	DirAccess.make_dir_recursive_absolute(out)
	var file := FileAccess.open(out.path_join("memory_report.json"), FileAccess.WRITE)
	if file == null:
		push_error("collect_memory: cannot write memory_report.json.")
		return
	file.store_string(text)
	file.close()
