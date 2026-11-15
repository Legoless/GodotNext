extends Node3D

## Quality preset switching check for #55.
##
## Loads Color Box Room A with NextGI on and switches the preset at runtime:
## Medium -> Epic -> High, then High and Epic with the specular hit debug view
## on, then Medium (Lumen Lite, which traces no reflections) and High again.
## Each change must be reflected in the status and the run must finish without
## validation errors (the runner log carries them). At Epic and High the
## specular trace must follow the preset's reflection divisor (#274), so each
## switch resizes the trace textures and RB_TEX_SPECULAR_HIT, and while the
## debug view is on, its texture must have the trace size too.
##
## Run through run_quality.sh, which enables NextGI.

const FRAMES_PER_STEP := 8
const REFLECTION_DIVISORS := {"EPIC": 1, "HIGH": 2}

var _failures := PackedStringArray()


func _ready() -> void:
	var packed: PackedScene = load("res://scenes/color_box_room/cbr_a.tscn")
	var scene := packed.instantiate()
	add_child(scene)
	var world := scene.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		var environment := world.environment.duplicate()
		environment.nextgi_enabled = true
		world.environment = environment
	var camera := scene.get_node_or_null("CaptureCam") as Camera3D
	if camera != null:
		camera.current = true
	_run()


func _set_quality(p_quality: int, p_expected: String, p_require_lite: bool = false) -> void:
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/quality", p_quality)
	for i in FRAMES_PER_STEP:
		await RenderingServer.frame_post_draw
	var status: Dictionary = RenderingServer.nextgi_get_status()
	var quality := str(status.get("quality", ""))
	if quality != p_expected:
		_failures.append("quality is %s, expected %s" % [quality, p_expected])
	if REFLECTION_DIVISORS.has(p_expected):
		# The view renders at scaling_3d_scale 1.0, so the internal size is the viewport's.
		var divisor: int = REFLECTION_DIVISORS[p_expected]
		var view := get_viewport().get_visible_rect().size
		var expected := Vector2i(maxi(int(view.x) / divisor, 1), maxi(int(view.y) / divisor, 1))
		var info: Dictionary = RenderingServer.nextgi_get_tier_info()
		var trace := Vector2i(int(info.get("reflection_trace_width", -1)), int(info.get("reflection_trace_height", -1)))
		if trace != expected:
			_failures.append("%s traces %dx%d, expected %dx%d" % [p_expected, trace.x, trace.y, expected.x, expected.y])
		if get_viewport().debug_draw == Viewport.DEBUG_DRAW_NEXTGI_SPECULAR_HIT:
			var debug := Vector2i(int(info.get("reflection_debug_width", -1)), int(info.get("reflection_debug_height", -1)))
			if debug != expected:
				_failures.append("%s debug view texture is %dx%d, expected %dx%d" % [p_expected, debug.x, debug.y, expected.x, expected.y])
	elif p_require_lite:
		var lite_info: Dictionary = RenderingServer.nextgi_get_tier_info()
		if not bool(lite_info.get("lite_beta", false)):
			_failures.append("MEDIUM does not run Lumen Lite, so the traced -> Lite switch is not covered")


func _run() -> void:
	await _set_quality(1, "MEDIUM")
	await _set_quality(3, "EPIC")
	await _set_quality(2, "HIGH")
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGI_SPECULAR_HIT
	await _set_quality(2, "HIGH")
	await _set_quality(3, "EPIC")
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
	await _set_quality(1, "MEDIUM", true)
	await _set_quality(2, "HIGH")

	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "nextgi_quality",
		"pass": ok,
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)
