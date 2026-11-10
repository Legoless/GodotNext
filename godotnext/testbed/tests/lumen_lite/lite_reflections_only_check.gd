extends Node3D
## #186: Lumen Lite with rendering/global_illumination/nextgi/mode = reflections only, on the
## software tier the folder override picks. Lite traces no software view, so nothing allocates
## the last-frame copy the software screen trace reads. The Forward+ renderer copied into it
## anyway and logged an error every frame. No engine error may be logged while it renders.
## The mode is set before the first frame: a frame of another mode (or the Screen Probes
## gather) allocates the copy and hides the fault.

const FRAMES := 120

var _frames := 0
var _errors := ErrorLog.new()


## Collects the engine errors (not warnings) logged from any thread.
class ErrorLog:
	extends Logger

	var _mutex := Mutex.new()
	var _lines := PackedStringArray()

	func _log_error(function: String, file: String, line: int, code: String, rationale: String, _editor_notify: bool, error_type: int, _script_backtraces: Array[ScriptBacktrace]) -> void:
		if error_type == ERROR_TYPE_WARNING:
			return
		_mutex.lock()
		_lines.append("%s:%d %s: %s" % [file, line, function, rationale if not rationale.is_empty() else code])
		_mutex.unlock()

	func _log_message(_message: String, _error: bool) -> void:
		pass

	func lines() -> PackedStringArray:
		_mutex.lock()
		var copy := _lines.duplicate()
		_mutex.unlock()
		return copy


func _ready() -> void:
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/mode", 1)
	OS.add_logger(_errors)
	var packed: PackedScene = load("res://scenes/swrt_kitbash/swrt_kitbash.tscn")
	var root := packed.instantiate()
	add_child(root)
	var world := root.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		var environment := world.environment.duplicate()
		environment.nextgi_enabled = true
		world.environment = environment
	var camera := Camera3D.new()
	add_child(camera)
	camera.position = Vector3(0.0, 2.0, 8.0)
	camera.look_at(Vector3(0.0, 1.0, 0.0))
	camera.current = true


func _process(_delta: float) -> void:
	_frames += 1
	if _frames < FRAMES:
		return
	set_process(false)
	OS.remove_logger(_errors)
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var status: Dictionary = RenderingServer.nextgi_get_status()
	var errors := _errors.lines()
	var reasons := PackedStringArray()
	if str(info.get("tier", "")) != "SWRT":
		reasons.append("tier is %s, not SWRT" % str(info.get("tier", "")))
	if not bool(info.get("lite_beta", false)):
		reasons.append("Lite is not active (quality %s)" % str(info.get("quality", "")))
	if str(status.get("mode", "")) != "reflections_only":
		reasons.append("mode is %s, not reflections_only" % str(status.get("mode", "")))
	if not errors.is_empty():
		reasons.append("%d engine errors in %d frames, the first: %s" % [errors.size(), FRAMES, errors[0]])
	var summary := {
		"tier": str(info.get("tier", "")),
		"lite": bool(info.get("lite_beta", false)),
		"mode": str(status.get("mode", "")),
		"frames": FRAMES,
		"engine_errors": errors.size(),
		"pass": reasons.is_empty(),
		"reasons": reasons,
	}
	print("GODOTNEXT_RESULT %s" % JSON.stringify(summary))
	_write_manifest(summary)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/mode", 0)
	get_tree().quit(0 if reasons.is_empty() else 1)


func _write_manifest(p_summary: Dictionary) -> void:
	for arg in OS.get_cmdline_user_args():
		if str(arg).begins_with("--out="):
			var out_dir := str(arg).substr("--out=".length())
			DirAccess.make_dir_recursive_absolute(out_dir)
			var file := FileAccess.open(out_dir.path_join("manifest.json"), FileAccess.WRITE)
			if file != null:
				file.store_string(JSON.stringify({ "source": "lite_reflections_only_check", "summary": p_summary }, "\t") + "\n")
