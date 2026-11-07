extends Node3D
## #127: front-layer reflection allocation and tracing checks on Glass & Water.
## This is a build/Metal smoke test, not a PQ quality or timing measurement.


func _ready() -> void:
	var packed: PackedScene = load("res://scenes/glass_water/glass_water.tscn")
	var scene := packed.instantiate()
	add_child(scene)
	var world := scene.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		var environment := world.environment.duplicate()
		environment.nextgi_enabled = true
		if "--on" in OS.get_cmdline_user_args():
			environment.set("nextgi_translucency_front_layer_reflections", true)
		world.environment = environment
	var camera := scene.get_node_or_null("CaptureCam") as Camera3D
	if camera != null:
		camera.current = true
	_run()


func _run() -> void:
	for i in 60:
		await RenderingServer.frame_post_draw
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var enabled := "--on" in OS.get_cmdline_user_args() and not "--disallow" in OS.get_cmdline_user_args()
	var targets := bool(info.get("front_layer_targets_created", false))
	var shader_count := int(info.get("front_layer_shader_count", 0))
	var failures := PackedStringArray()
	if String(info.get("tier", "")) != "HWRT":
		failures.append("tier is not HWRT")
	if targets != enabled or shader_count != (1 if enabled else 0):
		failures.append("lazy target or shader allocation is incorrect")
	if enabled:
		if int(info.get("front_layer_dispatch_count", 0)) <= 0:
			failures.append("no front-layer dispatch")
		if int(info.get("front_layer_traced", 0)) <= 0:
			failures.append("no front-layer reflection rays")
		if not bool(info.get("front_layer_independent_history", false)):
			failures.append("front-layer history was not allocated independently")
	if "--capture" in OS.get_cmdline_user_args():
		var mode := "on" if enabled else "off"
		var error := get_viewport().get_texture().get_image().save_png("user://front_layer_%s.png" % mode)
		if error != OK:
			failures.append("capture failed with error %d" % error)
		else:
			print("FRONT_LAYER_CAPTURE path=%s" % OS.get_user_data_dir().path_join("front_layer_%s.png" % mode))
	var result := {
		"test": "front_layer",
		"pass": failures.is_empty(),
		"targets": targets,
		"shader_count": shader_count,
		"dispatches": int(info.get("front_layer_dispatch_count", 0)),
		"rays": int(info.get("front_layer_traced", 0)),
		"hits": int(info.get("front_layer_hits", 0)),
		"failures": failures,
	}
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if failures.is_empty() else 1)
