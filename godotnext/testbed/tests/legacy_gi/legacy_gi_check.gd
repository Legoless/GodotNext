extends Node

## The legacy GI paths reach the image with NextGI off (#248).
##
## Renders a legacy scene from its HallCam (inside the hall, where the GI shows) in a SubViewport,
## first with its GI on, then off: --scene=sdfgi turns the Environment's SDFGI off, --scene=voxelgi
## hides the VoxelGI node. The two images must differ by at least MIN_MEAN_DIFFERENCE (mean
## absolute difference over the RGB channels): before #248, a view without NextGI computed the GI
## buffers and then discarded them, so both images were the same. Run with NextGI off (the
## testbed's default). Prints one "GODOTNEXT_RESULT <json>" line and exits 0 when the check passes.

const SCENES := {"sdfgi": "res://scenes/legacy/legacy_sdfgi.tscn", "voxelgi": "res://scenes/legacy/legacy_voxelgi.tscn"}
const VIEW_SIZE := Vector2i(640, 360)
const SETTLE_FRAMES := 150
const MIN_MEAN_DIFFERENCE := 0.005

var _viewport: SubViewport
var _scene: Node3D
var _which := "sdfgi"
var _frames := 0
var _image_on: Image


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--scene="):
			_which = arg.trim_prefix("--scene=")
	if not SCENES.has(_which):
		_finish(false, ["unknown --scene=%s (sdfgi or voxelgi)" % _which], {})
		return
	_viewport = SubViewport.new()
	_viewport.size = VIEW_SIZE
	_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(_viewport)
	_scene = load(SCENES[_which]).instantiate()
	_viewport.add_child(_scene)
	var camera := _scene.get_node_or_null("HallCam") as Camera3D
	if camera == null:
		_finish(false, ["%s has no HallCam" % SCENES[_which]], {})
		return
	camera.current = true


func _process(_delta: float) -> void:
	_frames += 1
	if _frames == SETTLE_FRAMES:
		_image_on = _viewport.get_texture().get_image()
		_set_gi(false)
	elif _frames == 2 * SETTLE_FRAMES:
		var image_off := _viewport.get_texture().get_image()
		var difference := _mean_difference(_image_on, image_off)
		var summary := {"scene": _which, "mean_difference": difference, "mean_on": _mean(_image_on), "mean_off": _mean(image_off)}
		var failures := PackedStringArray()
		if difference < MIN_MEAN_DIFFERENCE:
			failures.append("%s on and off differ by %.6f, expected at least %.3f: the GI does not reach the image" % [_which, difference, MIN_MEAN_DIFFERENCE])
		_finish(failures.is_empty(), failures, summary)


func _set_gi(p_enabled: bool) -> void:
	if _which == "sdfgi":
		var world := _scene.get_node("WorldEnvironment") as WorldEnvironment
		world.environment.sdfgi_enabled = p_enabled
	else:
		(_scene.get_node("VoxelGI") as Node3D).visible = p_enabled


func _mean(p_image: Image) -> float:
	var sum := 0.0
	for y in range(0, p_image.get_height(), 2):
		for x in range(0, p_image.get_width(), 2):
			var color := p_image.get_pixel(x, y)
			sum += (color.r + color.g + color.b) / 3.0
	return sum / float((p_image.get_height() / 2) * (p_image.get_width() / 2))


func _mean_difference(p_a: Image, p_b: Image) -> float:
	var sum := 0.0
	for y in range(0, p_a.get_height(), 2):
		for x in range(0, p_a.get_width(), 2):
			var a := p_a.get_pixel(x, y)
			var b := p_b.get_pixel(x, y)
			sum += (absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b)) / 3.0
	return sum / float((p_a.get_height() / 2) * (p_a.get_width() / 2))


func _finish(p_pass: bool, p_failures: Array, p_summary: Dictionary) -> void:
	p_summary["test"] = "legacy_gi"
	p_summary["pass"] = p_pass
	p_summary["failures"] = p_failures
	print("GODOTNEXT_RESULT " + JSON.stringify(p_summary))
	get_tree().quit(0 if p_pass else 1)
