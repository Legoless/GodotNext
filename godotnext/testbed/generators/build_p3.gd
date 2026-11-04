extends SceneTree
## Builds the P3 gate scenes (#52).
## godot --headless --path godotnext/testbed --script res://generators/build_p3.gd

const P3 := preload("res://generators/build_p3_content.gd")


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var failures: Array[String] = await P3.save_all(self)
	if failures.is_empty():
		print("build_p3: all scenes built and verified.")
		quit(0)
	else:
		for failure in failures:
			push_error("build_p3: " + failure)
		quit(1)
