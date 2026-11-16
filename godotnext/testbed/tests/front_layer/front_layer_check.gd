extends Node3D
## #127: front-layer reflection allocation and tracing checks on Glass & Water.
## This is a build/Metal smoke test, not a PQ quality or timing measurement.
## --switch (with --on, #276): then switches the preset at runtime, High (the runner's) -> Epic ->
## Medium (Lumen Lite, which keeps the front layer) -> High. At each step the front layer must trace at
## the preset's reflection divisor (Epic 1, High 2, Medium 4) and its own hit distance target, which its
## gather recreates on every change, must have that size.

const SWITCH_STEPS := [[2, "HIGH", 2], [3, "EPIC", 1], [1, "MEDIUM", 4], [2, "HIGH", 2]]
const FRAMES_PER_STEP := 8


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
	var switched := []
	if enabled and "--switch" in OS.get_cmdline_user_args():
		switched = await _switch_presets(info, failures)
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
		"switched": switched,
		"failures": failures,
	}
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if failures.is_empty() else 1)


func _switch_presets(p_info: Dictionary, r_failures: PackedStringArray) -> Array:
	var steps := []
	var dispatches := int(p_info.get("front_layer_dispatch_count", 0))
	# The view renders at scaling_3d_scale 1.0, so the internal size is the viewport's.
	var view := Vector2i(get_viewport().get_visible_rect().size)
	for step: Array in SWITCH_STEPS:
		ProjectSettings.set_setting("rendering/global_illumination/nextgi/quality", step[0])
		for i in FRAMES_PER_STEP:
			await RenderingServer.frame_post_draw
		var info: Dictionary = RenderingServer.nextgi_get_tier_info()
		var divisor: int = step[2]
		var expected := Vector2i(maxi(view.x / divisor, 1), maxi(view.y / divisor, 1))
		var trace := Vector2i(int(info.get("front_layer_trace_width", -1)), int(info.get("front_layer_trace_height", -1)))
		var target := Vector2i(int(info.get("front_layer_hit_distance_width", -1)), int(info.get("front_layer_hit_distance_height", -1)))
		var now := int(info.get("front_layer_dispatch_count", 0))
		steps.append({"quality": String(info.get("quality", "")), "lite": bool(info.get("lite_beta", false)), "trace": [trace.x, trace.y], "hit_distance": [target.x, target.y], "dispatches": now - dispatches})
		if String(info.get("quality", "")) != step[1]:
			r_failures.append("quality is %s, expected %s" % [info.get("quality", ""), step[1]])
		if now <= dispatches:
			r_failures.append("%s: the front layer did not trace after the switch" % step[1])
		if trace != expected:
			r_failures.append("%s: the front layer traces %dx%d, expected %dx%d" % [step[1], trace.x, trace.y, expected.x, expected.y])
		if target != expected:
			r_failures.append("%s: the front layer's hit distance target is %dx%d, expected %dx%d" % [step[1], target.x, target.y, expected.x, expected.y])
		dispatches = now
	return steps
