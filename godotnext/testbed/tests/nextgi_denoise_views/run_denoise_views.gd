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
## Then a rough box wider than the view slides left in front of the still camera.
## The specular temporal filter reprojects dynamic texels with the prepass
## velocity (previous minus current uv, as TAA reads it), so the box's texels at
## the right edge, which were off the view last frame, reject their history and
## those at the left edge keep it. The opposite sign flips both edges (#54).
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
# The sliding box: its distance from the camera, its size against the view, and how far it moves
# per frame as a share of the view's width.
const SLIDER_DISTANCE := 1.0
const SLIDER_WIDTH_VIEWS := 2.0
const SLIDER_HEIGHT_VIEWS := 0.3
const SLIDER_STEP_VIEWS := 0.03
# Sampled bands at the left and right edges, as a share of the image width. Narrower than the step.
const EDGE_BAND := 0.012

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

	var camera := get_viewport().get_camera_3d()
	if camera != null:
		await _check_sliding_box(camera)

	var hashes := {}
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


# A dynamic box moving left reprojects to the right: its history exists at the left edge and is
# off the view at the right edge. Reads the specular rejected-history view (1 where rejected).
func _check_sliding_box(camera: Camera3D) -> void:
	var half_height := SLIDER_DISTANCE * tan(deg_to_rad(camera.fov * 0.5))
	var view_size := get_viewport().get_visible_rect().size
	var half_width := half_height * view_size.x / view_size.y
	var box := MeshInstance3D.new()
	var mesh := BoxMesh.new()
	mesh.size = Vector3(half_width * 2.0 * SLIDER_WIDTH_VIEWS, half_height * 2.0 * SLIDER_HEIGHT_VIEWS, 0.05)
	var material := StandardMaterial3D.new()
	material.roughness = 1.0
	mesh.material = material
	box.mesh = mesh
	box.position = Vector3(0.0, 0.0, -SLIDER_DISTANCE)
	camera.add_child(box)

	get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGI_SPECULAR_REJECTED
	for i in VIEW_FRAMES:
		box.position.x -= half_width * 2.0 * SLIDER_STEP_VIEWS
		await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	box.queue_free()
	await RenderingServer.frame_post_draw
	if image == null or image.is_empty():
		_failures.append("the specular rejected-history view produced no image")
		return
	var band := maxi(1, int(image.get_width() * EDGE_BAND))
	var left := _rejected_in_band(image, 0, band)
	var right := _rejected_in_band(image, image.get_width() - band, image.get_width())
	if left.x > 0:
		_failures.append("a box sliding left rejected the history of %d of %d sampled texels at the left edge, where its last position is in view" % [left.x, left.y])
	if right.x == 0:
		_failures.append("a box sliding left kept the history of all %d sampled texels at the right edge, where its last position is off the view" % [right.y])


# Rejected and sampled counts over the box's rows (the middle tenth of the image).
func _rejected_in_band(image: Image, from_x: int, to_x: int) -> Vector2i:
	var rejected := 0
	var sampled := 0
	for y in range(int(image.get_height() * 0.45), int(image.get_height() * 0.55), 4):
		for x in range(from_x, to_x, 2):
			sampled += 1
			if image.get_pixel(x, y).r > 0.5:
				rejected += 1
	return Vector2i(rejected, sampled)
