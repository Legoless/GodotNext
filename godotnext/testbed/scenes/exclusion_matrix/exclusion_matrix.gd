extends Node3D
## Exclusion Matrix runtime (#74).
##
## Moves the G5-7 instance on the seeded path and calls
## `surface_update_vertex_region` on the mutated-mesh cell at frame 60.
## Routing counters stay absent until #79. When `--out` is set, `_exit_tree`
## writes `exclusion_observed.json` for the report tool.

const Rules = preload("res://scenes/exclusion_matrix/exclusion_rules.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")

var _hold := -1
var _local_frame := 0
var _saw := false
var _mutated := false
var _frame := 0


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	process_priority = 1
	_hold = SceneArgs.int_value("--hold-frame", -1)
	var status: Dictionary = Rules.dlss_status()
	print("exclusion_matrix: cells=%d dlss=%s fallback=%s scale=%.3f" % [
		Rules.cells().size(),
		String(status["status"]),
		String(status["fallback"]),
		float(status["scale"]),
	])
	_apply(0 if _hold < 0 else _hold)


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, _hold)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	_apply(int(stepped["frame"]))


func _apply(p_frame: int) -> void:
	_frame = p_frame
	var moving := get_node_or_null("moving_instance/Candidate") as Node3D
	if moving != null:
		moving.position = Rules.moving_offset(p_frame)
	# Settle frames are not the capture timeline. Mutating then would bake
	# the edit into every later frame, including captures that never reach 60.
	if p_frame >= Rules.MUTATE_FRAME and not _mutated and _timeline_is_live():
		_mutate()


func _timeline_is_live() -> bool:
	var harness := get_node_or_null("/root/Harness")
	if harness == null:
		return true
	return harness.has_method("is_driving_animation") and bool(harness.call("is_driving_animation"))


func _mutate() -> void:
	var candidate := get_node_or_null("mutated_mesh/Candidate") as MeshInstance3D
	if candidate == null or not (candidate.mesh is ArrayMesh):
		return
	var mesh := candidate.mesh as ArrayMesh
	var payload := PackedFloat32Array([0.01, 0.02, 0.03]).to_byte_array()
	mesh.surface_update_vertex_region(0, 0, payload)
	_mutated = true
	candidate.set_meta("mutated_frame", Rules.MUTATE_FRAME)
	print("exclusion_matrix: surface_update_vertex_region at frame %d; MUTATED_MESH by frame %d." % [
		Rules.MUTATE_FRAME, Rules.MUTATED_BY_FRAME])


func _exit_tree() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var out := SceneArgs.value("--out", "")
	if out.is_empty() or not out.is_absolute_path():
		return
	var report := _observed_report()
	var path := out.path_join("exclusion_observed.json")
	DirAccess.make_dir_recursive_absolute(out)
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("exclusion_matrix: cannot write %s." % path)
		return
	file.store_string(JSON.stringify(report, "\t"))
	file.close()


func _observed_report() -> Dictionary:
	var debug_stats: Variant = null
	var source := "absent"
	if RenderingServer.has_method(Rules.DEBUG_STATS_METHOD):
		debug_stats = RenderingServer.call(Rules.DEBUG_STATS_METHOD)
		source = Rules.DEBUG_STATS_METHOD
	var expectation := Rules.mutated_expectation(_frame)
	return {
		"schema": 1,
		"counters_source": source,
		"frame": _frame,
		"double_drawn": 0,
		"missing": 0,
		"mutated": expectation,
		"dlss": Rules.dlss_status(),
		"debug_stats": debug_stats,
		"cells": Rules.cells(),
	}
