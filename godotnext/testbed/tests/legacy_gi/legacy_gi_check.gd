extends Node

## The legacy GI paths reach the image with NextGI off (#248), and SDFGI lights from upstream's lights (#265).
##
## Renders a legacy scene from its HallCam (inside the hall, where the GI shows) in a SubViewport,
## first with its GI on, then off: --scene=sdfgi turns the Environment's SDFGI off, --scene=voxelgi
## hides the VoxelGI node. The two images must differ by at least MIN_MEAN_DIFFERENCE (mean
## absolute difference over the RGB channels): before #248, a view without NextGI computed the GI
## buffers and then discarded them, so both images were the same.
##
## --scene=sdfgi_lights checks which positional lights SDFGI lights its cascades from. Upstream's list
## adds a light when it is shown in a scenario, or when its first update finds it in one, but not when
## it enters one. A scene change frees the old scene, which updates the new scene's lights before they
## enter the tree, so upstream SDFGI leaves them out. The check makes that order, then turns the
## HallLight's indirect energy off: the image must stay within MAX_SAME_DIFFERENCE. Hiding and showing
## the HallLight then adds it, and the image must change by at least MIN_MEAN_DIFFERENCE. Before #265,
## NextGI's light list also fed SDFGI and took the HallLight in as it entered the scenario.
##
## Run with NextGI off (the testbed's default). Prints one "GODOTNEXT_RESULT <json>" line and exits 0
## when the check passes.

const SCENES := {"sdfgi": "res://scenes/legacy/legacy_sdfgi.tscn", "voxelgi": "res://scenes/legacy/legacy_voxelgi.tscn", "sdfgi_lights": "res://scenes/legacy/legacy_sdfgi.tscn"}
const VIEW_SIZE := Vector2i(640, 360)
const SETTLE_FRAMES := 150
const MIN_MEAN_DIFFERENCE := 0.005
const MAX_SAME_DIFFERENCE := 0.001

var _viewport: SubViewport
var _scene: Node3D
var _which := "sdfgi"
var _frames := 0
var _image_on: Image
var _image_off: Image


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--scene="):
			_which = arg.trim_prefix("--scene=")
	if not SCENES.has(_which):
		_finish(false, ["unknown --scene=%s (sdfgi, voxelgi or sdfgi_lights)" % _which], {})
		return
	_viewport = SubViewport.new()
	_viewport.size = VIEW_SIZE
	_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(_viewport)
	_scene = load(SCENES[_which]).instantiate()
	if _which == "sdfgi_lights":
		# Free an instance, as a scene change frees the old scene: the rendering server first updates
		# every dirty instance, so the scene's lights get their first update outside the scenario.
		RenderingServer.free_rid(RenderingServer.instance_create())
	_viewport.add_child(_scene)
	var camera := _scene.get_node_or_null("HallCam") as Camera3D
	if camera == null:
		_finish(false, ["%s has no HallCam" % SCENES[_which]], {})
		return
	camera.current = true


func _process(_delta: float) -> void:
	_frames += 1
	if _which == "sdfgi_lights":
		_process_lights()
		return
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


func _process_lights() -> void:
	var light := _scene.get_node("HallLight") as Light3D
	if _frames == SETTLE_FRAMES:
		_image_on = _viewport.get_texture().get_image()
		light.light_indirect_energy = 0.0
	elif _frames == 2 * SETTLE_FRAMES:
		_image_off = _viewport.get_texture().get_image()
		light.light_indirect_energy = 1.0
		light.visible = false
	elif _frames == 2 * SETTLE_FRAMES + 1:
		light.visible = true
	elif _frames == 3 * SETTLE_FRAMES + 1:
		var image_shown := _viewport.get_texture().get_image()
		var unlisted := _mean_difference(_image_on, _image_off)
		var shown := _mean_difference(_image_on, image_shown)
		var summary := {"scene": _which, "indirect_off_difference": unlisted, "shown_difference": shown, "mean_entered": _mean(_image_on), "mean_indirect_off": _mean(_image_off), "mean_shown": _mean(image_shown)}
		var failures := PackedStringArray()
		if unlisted > MAX_SAME_DIFFERENCE:
			failures.append("the HallLight's indirect energy changed the image by %.6f, expected at most %.3f: SDFGI lights from a light that entered the scenario after its first update, which upstream leaves out" % [unlisted, MAX_SAME_DIFFERENCE])
		if shown < MIN_MEAN_DIFFERENCE:
			failures.append("showing the HallLight changed the image by %.6f, expected at least %.3f: SDFGI either lit from it before it was shown or does not add a light shown in the scenario" % [shown, MIN_MEAN_DIFFERENCE])
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
