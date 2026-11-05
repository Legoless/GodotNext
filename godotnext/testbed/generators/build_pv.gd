extends SceneTree
## Builds the PV content scenes (#213).
## godot --headless --path godotnext/testbed --script res://generators/build_pv.gd

const PV := preload("res://generators/build_pv_content.gd")


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var failures: Array[String] = await PV.save_all(self)
	if failures.is_empty():
		print("build_pv: all scenes built and verified.")
		quit(0)
	else:
		for failure in failures:
			push_error("build_pv: " + failure)
		quit(1)
