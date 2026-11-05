extends SceneTree
## Builds the PN content scenes (#148): Foliage Field variants, Displacement
## Cliff and Spline Road. Micro Detail is Dense City's `micro_detail` variant.
## godot --headless --path godotnext/testbed --script res://generators/build_pn.gd

const PN := preload("res://generators/build_pn_content.gd")


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var failures: Array[String] = await PN.save_all(self)
	if failures.is_empty():
		print("build_pn: all scenes built and verified.")
		quit(0)
	else:
		for failure in failures:
			push_error("build_pn: " + failure)
		quit(1)
