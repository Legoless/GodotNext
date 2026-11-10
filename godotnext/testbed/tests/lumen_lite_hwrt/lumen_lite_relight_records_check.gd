extends Node3D
## #183: SRC relight schedules only the page sets of the current instance records (hardware
## tier, Lumen Lite).
##
## A page set stays resident after its instance leaves the instance records (here the boxes are
## hidden). Its record index then names another instance, or none, so relight must leave it
## alone: a relit stale set reads whatever card set that record has, which can be no card set
## at all.
## The check sweeps an omni light over a floor and a row of boxes and reads the number of pages
## the light dirtied (the src_dirty_pages counter, counted over the sets relight walks):
## - floor only: F pages;
## - boxes shown: more than F;
## - boxes hidden again: F. A relight that still walks the hidden boxes' sets keeps the larger
##   count;
## - boxes shown again: the larger count is back, so the sets return with their instances.
##
## Prints LITE_RELIGHT lines and LITE_RELIGHT_PASS, or LITE_RELIGHT_FAIL with every failed
## assertion, and exits 0 or 1.

const WARMUP_FRAMES := 150
const SETTLE_FRAMES := 20
const SWEEP_FRAMES := 40
const BOX_COUNT := 8

var _environment: Environment
var _light: OmniLight3D
var _boxes: Array[MeshInstance3D] = []
var _phase := 0
var _phase_frame := 0
var _sweep_max := 0
var _floor_pages := 0
var _shown_pages := 0
var _hidden_pages := 0
var _failures := PackedStringArray()
var _summary := {}
var _done := false


func _ready() -> void:
	_environment = Environment.new()
	_environment.background_mode = Environment.BG_COLOR
	_environment.background_color = Color(0.02, 0.02, 0.02)
	_environment.nextgi_enabled = true
	var world := WorldEnvironment.new()
	world.environment = _environment
	add_child(world)

	_add_box(Vector3(0.0, -0.1, 0.0), Vector3(12.0, 0.2, 12.0), Color(0.7, 0.7, 0.7))
	for index in BOX_COUNT:
		var box := _add_box(Vector3(-3.5 + float(index), 0.5, -1.5), Vector3(0.8, 1.0, 0.8), Color(0.8, 0.3, 0.2))
		box.visible = false
		_boxes.append(box)

	_light = OmniLight3D.new()
	_light.omni_range = 14.0
	_light.light_energy = 2.0
	_light.position = Vector3(0.0, 3.0, 0.0)
	add_child(_light)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 4.0, 8.0)
	camera.rotation = Vector3(-0.45, 0.0, 0.0)
	add_child(camera)
	camera.current = true


func _add_box(center: Vector3, size: Vector3, color: Color) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	mesh.material = material
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = center
	add_child(instance)
	return instance


func _write_manifest() -> void:
	var args := OS.get_cmdline_user_args()
	var out_dir := ""
	for index in args.size():
		var arg := str(args[index])
		if arg.begins_with("--out="):
			out_dir = arg.substr("--out=".length())
		elif arg == "--out" and index + 1 < args.size():
			out_dir = str(args[index + 1])
	if out_dir.is_empty():
		return
	DirAccess.make_dir_recursive_absolute(out_dir)
	var file := FileAccess.open(out_dir.path_join("manifest.json"), FileAccess.WRITE)
	if file == null:
		return
	file.store_string(JSON.stringify({"source": "lumen_lite_relight_records_check", "summary": _summary, "failures": _failures}, "\t") + "\n")


func _finish() -> void:
	_done = true
	_write_manifest()
	if _failures.is_empty():
		print("LITE_RELIGHT_PASS")
		get_tree().quit(0)
		return
	for failure in _failures:
		push_error(failure)
	print("LITE_RELIGHT_FAIL %s" % "; ".join(_failures))
	get_tree().quit(1)


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


func _set_boxes_visible(shown: bool) -> void:
	for box in _boxes:
		box.visible = shown


# Moves the light every frame of a sweep, so every page in its range is dirty again, and keeps
# the largest dirty-page count. True when the sweep is over.
func _sweep() -> bool:
	if _phase_frame <= SETTLE_FRAMES:
		_sweep_max = 0
		return false
	_light.position.x = sin(float(_phase_frame) * 0.7) * 1.5
	var counters: Dictionary = RenderingServer.nextgi_get_counters()
	_sweep_max = maxi(_sweep_max, int(counters.get("src_dirty_pages", 0)))
	return _phase_frame >= SETTLE_FRAMES + SWEEP_FRAMES


func _process(_delta: float) -> void:
	if _done:
		return
	_phase_frame += 1
	match _phase:
		0:
			if _phase_frame < WARMUP_FRAMES:
				return
			var info: Dictionary = RenderingServer.nextgi_get_tier_info()
			_check(str(info.get("tier", "")) == "HWRT", "tier is %s, expected HWRT" % str(info.get("tier", "")))
			_check(bool(info.get("lite_beta", false)), "Lite is not active (quality %s)" % str(info.get("quality", "")))
			_check(int(info.get("src_resident_pages", 0)) > 0, "the floor has no radiance pages")
			if not _failures.is_empty():
				_finish()
				return
			_next_phase(1)
		1:
			if not _sweep():
				return
			_floor_pages = _sweep_max
			_check(_floor_pages > 0, "the moving light dirtied no floor page")
			_set_boxes_visible(true)
			_next_phase(2)
		2:
			if _phase_frame < WARMUP_FRAMES:
				return
			_next_phase(3)
		3:
			if not _sweep():
				return
			_shown_pages = _sweep_max
			_summary["resident_pages_shown"] = int(RenderingServer.nextgi_get_tier_info().get("src_resident_pages", 0))
			_check(_shown_pages > _floor_pages, "the boxes added no dirty page (%d with the boxes, %d for the floor)" % [_shown_pages, _floor_pages])
			_set_boxes_visible(false)
			_next_phase(4)
		4:
			if not _sweep():
				return
			_hidden_pages = _sweep_max
			_summary["resident_pages_hidden"] = int(RenderingServer.nextgi_get_tier_info().get("src_resident_pages", 0))
			_check(_hidden_pages == _floor_pages, "relight still walks the hidden boxes' page sets: %d dirty pages, %d for the floor alone (%d with the boxes)" % [_hidden_pages, _floor_pages, _shown_pages])
			_set_boxes_visible(true)
			_next_phase(5)
		5:
			if not _sweep():
				return
			_summary["dirty_pages"] = {"floor": _floor_pages, "shown": _shown_pages, "hidden": _hidden_pages, "shown_again": _sweep_max}
			print("LITE_RELIGHT dirty_pages floor=%d shown=%d hidden=%d shown_again=%d" % [_floor_pages, _shown_pages, _hidden_pages, _sweep_max])
			_check(_sweep_max == _shown_pages, "the boxes' page sets did not come back with the boxes (%d dirty pages, %d before)" % [_sweep_max, _shown_pages])
			_finish()


func _next_phase(phase: int) -> void:
	_phase = phase
	_phase_frame = 0
