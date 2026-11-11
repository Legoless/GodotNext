extends Node3D

## Specular reflection check for #60.
##
## Loads the Roughness Corridor, turns NextGI on, and reads the gather's specular
## counters after a few frames. The expected tier (--expected-tier=HWRT, the default, or SWRT)
## must be active, the trace must run at half resolution and it must find hits.
##
## GodotNext: ps_swrt_trace (#186). With --expected-tier=SWRT the reflections run the shared
## reflect kernel in the software sibling: its clean SDF hits (reflect_sdf_hits minus the
## step-limit and near-origin ones) must be at least CLEAN_HIT_SHARE of the hardware tier's
## reflection hits on the same view (--hwrt-reflection-hits=N, from a hardware pass). A hardware
## pass prints "SWRT_GI_REFERENCE reflection_hits=N".
##
## Run with the NextGI project setting on, e.g.:
##   nextgi_reflect/run_reflect.sh [--hwrt=auto|off] -- godot --path godotnext/testbed \
##     --rendering-driver metal res://tests/nextgi_reflect/run_reflect.tscn [-- --expected-tier=SWRT --hwrt-reflection-hits=N]
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const FRAMES := 24
const CLEAN_HIT_SHARE := 0.25

var _failures := PackedStringArray()
var _expected_tier := "HWRT"
var _hwrt_reflection_hits := -1


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--expected-tier="):
			_expected_tier = arg.trim_prefix("--expected-tier=")
		elif arg.begins_with("--hwrt-reflection-hits="):
			_hwrt_reflection_hits = int(arg.trim_prefix("--hwrt-reflection-hits="))
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

	if tier != _expected_tier:
		_failures.append("tier is %s, expected %s" % [tier, _expected_tier])
	if rays <= 0:
		_failures.append("the specular trace did not run")
	if hits <= 0:
		_failures.append("the specular trace found no hits")
	if hits + misses > rays * 2:
		_failures.append("hit and miss counts exceed the traced pixels")
	var sdf_hits := int(status.get("reflect_sdf_hits", 0))
	var clean := sdf_hits - int(status.get("reflect_sdf_exhausted", 0)) - int(status.get("reflect_sdf_near", 0))
	if _expected_tier == "HWRT":
		print("SWRT_GI_REFERENCE reflection_hits=%d" % hits)
	elif _expected_tier == "SWRT":
		print("SWRT_GI_REFLECT rays=%d hits=%d sdf_hits=%d clean=%d exhausted=%d near=%d hwrt_hits=%d ratio=%.3f (report-only)" % [rays, hits, sdf_hits, clean, int(status.get("reflect_sdf_exhausted", 0)), int(status.get("reflect_sdf_near", 0)), _hwrt_reflection_hits, float(clean) / float(maxi(_hwrt_reflection_hits, 1))])
		if String(status.get("gather_tier", "")) != "SWRT":
			_failures.append("the reflections ran the %s shader, expected the software sibling" % [String(status.get("gather_tier", ""))])
		if _hwrt_reflection_hits <= 0:
			_failures.append("--hwrt-reflection-hits=N (the hardware tier's reflection hits) is required with --expected-tier=SWRT")
		elif clean <= 0 or float(clean) < CLEAN_HIT_SHARE * float(_hwrt_reflection_hits):
			_failures.append("%d clean SDF reflection hits, expected at least %.2f x the hardware tier's %d" % [clean, CLEAN_HIT_SHARE, _hwrt_reflection_hits])
	else:
		_failures.append("--expected-tier must be HWRT or SWRT")

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
		"sdf_hits": sdf_hits,
		"clean_sdf_hits": clean,
		"views": views,
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)
