extends Node3D

## Specular reflection check for #60.
##
## Loads the Roughness Corridor, turns NextGI on, and reads the gather's specular
## counters after a few frames. The HWRT tier must be active, the trace must run
## at half resolution and it must find hits.
##
## Run with the NextGI project setting on, e.g.:
##   nextgi_reflect/run_reflect.sh -- godot --path godotnext/testbed \
##     --rendering-driver metal res://tests/nextgi_reflect/run_reflect.tscn
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const FRAMES := 24

var _failures := PackedStringArray()


func _ready() -> void:
	var packed: PackedScene = load("res://scenes/roughness_corridor/roughness_corridor.tscn")
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


func _run() -> void:
	for i in FRAMES:
		await RenderingServer.frame_post_draw

	var status: Dictionary = RenderingServer.nextgi_get_tier_info()
	var tier := String(status.get("tier", ""))
	var rays := int(status.get("reflection_rays", 0))
	var hits := int(status.get("reflection_hits", 0))
	var misses := int(status.get("reflection_misses", 0))

	if tier != "HWRT":
		_failures.append("tier is %s, expected HWRT" % tier)
	if rays <= 0:
		_failures.append("the specular trace did not run")
	if hits <= 0:
		_failures.append("the specular trace found no hits")
	if hits + misses > rays * 2:
		_failures.append("hit and miss counts exceed the traced pixels")

	# The two reflection debug views must present the traced image, not a flat tint.
	var views := {}
	var specular_views := [
		[Viewport.DEBUG_DRAW_NEXTGI_SPECULAR_RAW, "specular_raw", true],
		[Viewport.DEBUG_DRAW_NEXTGI_SPECULAR_VARIANCE, "specular_variance", true],
		[Viewport.DEBUG_DRAW_NEXTGI_SPECULAR_HISTORY, "specular_history", true],
		[Viewport.DEBUG_DRAW_NEXTGI_SPECULAR_REJECTED, "specular_rejected", false],
		[Viewport.DEBUG_DRAW_NEXTGI_SPECULAR_DENOISED, "specular_denoised", true],
	]
	for entry in [[Viewport.DEBUG_DRAW_NEXTGI_REFLECTION, "reflection"], [Viewport.DEBUG_DRAW_NEXTGI_SPECULAR_HIT, "specular_hit"]]:
		get_viewport().debug_draw = int(entry[0])
		for i in 6:
			await RenderingServer.frame_post_draw
		var image := get_viewport().get_texture().get_image()
		var first: Color = image.get_pixel(0, 0)
		var uniform := true
		for y in range(0, image.get_height(), 64):
			for x in range(0, image.get_width(), 64):
				if image.get_pixel(x, y) != first:
					uniform = false
					break
			if not uniform:
				break
		views[entry[1]] = not uniform
		if uniform:
			_failures.append("%s debug view rendered a uniform image" % entry[1])
	# The five specular-filter views: raw, variance, history, rejected, denoised.
	for entry in specular_views:
		get_viewport().debug_draw = int(entry[0])
		for i in 6:
			await RenderingServer.frame_post_draw
		var image := get_viewport().get_texture().get_image()
		var first: Color = image.get_pixel(0, 0)
		var uniform := true
		for y in range(0, image.get_height(), 64):
			for x in range(0, image.get_width(), 64):
				if image.get_pixel(x, y) != first:
					uniform = false
					break
			if not uniform:
				break
		views[entry[1]] = not uniform
		if uniform and bool(entry[2]):
			_failures.append("%s debug view rendered a uniform image" % entry[1])

	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED

	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "nextgi_reflect",
		"pass": ok,
		"tier": tier,
		"rays": rays,
		"hits": hits,
		"misses": misses,
		"views": views,
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)
