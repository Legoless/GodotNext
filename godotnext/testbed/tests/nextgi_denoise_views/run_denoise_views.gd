extends Node3D

## Denoise debug-view check for #53.
##
## Loads the G1 static scene, turns NextGI on, and renders each of the five
## denoise views (raw, variance, history, rejected history, denoised) in turn.
## The HWRT tier must be active, every view must present a non-uniform image,
## and the five views must not collapse to the same image. The camera pans while
## the views render, so history and rejection vary at disocclusions.
## First, with the camera still, no probe may reject its history: the temporal
## pass reprojects a still view onto itself (#186 world reconstruction).
##
## Run with the NextGI project setting on, e.g.:
##   GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
##     MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
##     godot --path godotnext/testbed --rendering-driver metal \
##       res://tests/nextgi_denoise_views/run_denoise_views.tscn
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const SETTLE_FRAMES := 8
const VIEW_FRAMES := 6

const VIEWS: Array[Array] = [
	[Viewport.DEBUG_DRAW_NEXTGI_DENOISE_RAW, "denoise_raw"],
	[Viewport.DEBUG_DRAW_NEXTGI_DENOISE_VARIANCE, "denoise_variance"],
	[Viewport.DEBUG_DRAW_NEXTGI_DENOISE_HISTORY, "denoise_history"],
	[Viewport.DEBUG_DRAW_NEXTGI_DENOISE_REJECTED, "denoise_rejected"],
	[Viewport.DEBUG_DRAW_NEXTGI_DENOISE_DENOISED, "denoise_denoised"],
]

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


func _run() -> void:
	for i in SETTLE_FRAMES:
		await RenderingServer.frame_post_draw

	var status: Dictionary = RenderingServer.nextgi_get_status()
	var tier := String(status.get("tier", ""))
	if tier != "HWRT":
		_failures.append("tier is %s, expected HWRT" % tier)

	# A still camera reprojects every probe onto itself and rejects nothing.
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGI_DENOISE_REJECTED
	for i in VIEW_FRAMES:
		await RenderingServer.frame_post_draw
	var still := get_viewport().get_texture().get_image()
	if still != null and not still.is_empty():
		var rejected := 0
		for y in range(0, still.get_height(), 16):
			for x in range(0, still.get_width(), 16):
				if still.get_pixel(x, y).r > 0.0:
					rejected += 1
		if rejected > 0:
			_failures.append("a still camera rejected the history of %d sampled pixels" % rejected)

	var hashes := {}
	var camera := get_viewport().get_camera_3d()
	for view in VIEWS:
		get_viewport().debug_draw = int(view[0])
		for i in VIEW_FRAMES:
			if camera != null:
				camera.position.x += 0.05
			await RenderingServer.frame_post_draw
		var image := get_viewport().get_texture().get_image()
		if image == null or image.is_empty():
			_failures.append("%s produced no image" % view[1])
			continue
		var ctx := HashingContext.new()
		ctx.start(HashingContext.HASH_SHA256)
		ctx.update(image.get_data())
		var hash := ctx.finish().hex_encode()
		if hashes.has(hash):
			_failures.append("%s is identical to %s" % [view[1], hashes[hash]])
		hashes[hash] = view[1]
		var first: Color = image.get_pixel(0, 0)
		var uniform := true
		for y in range(0, image.get_height(), 64):
			for x in range(0, image.get_width(), 64):
				if image.get_pixel(x, y) != first:
					uniform = false
					break
			if not uniform:
				break
		if uniform:
			_failures.append("%s rendered a uniform image" % view[1])

	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "nextgi_denoise_views",
		"pass": ok,
		"tier": tier,
		"views": VIEWS.size(),
		"distinct": hashes.size(),
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)
