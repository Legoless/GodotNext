extends SceneTree
## Content check for the static-UI scene and its 3D-only twin (#202, GU+-6). No GPU. It checks
## - the scene instances Upscaler Torture, so the #95 camera spline and timeline are reused;
## - the HUD: labels at 12, 18 and 32 px, a 50%-alpha panel, a 1-px crosshair through the center
##   pixel and an opaque 256-px minimap, at the same pixel sizes at 720p, 1080p, 1440p and 2160p;
## - the twin: the same scene text with the HUD hidden and nothing else changed;
## - the UI mask threshold (alpha >= 0.99).
##
## Usage: <godot> --headless --path godotnext/testbed --script res://scenes/static_ui/check_static_ui.gd

const Generator := preload("res://generators/static_ui.gd")
const UpscalerTorture := preload("res://generators/upscaler_torture.gd")
const StaticUI := preload("res://scenes/static_ui/static_ui.gd")

const OUTPUT_SIZES: Array[Vector2i] = [Vector2i(1280, 720), Vector2i(1920, 1080), Vector2i(2560, 1440), Vector2i(3840, 2160)]

var _failures: PackedStringArray = []
var _report := {}


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	var scene := (load(Generator.SCENE_PATH) as PackedScene).instantiate() as Node3D
	scene.set_meta("skip_runtime", true)
	_check_world(scene)
	_check_hud(scene)
	_check_twin()
	_check_mask_threshold()
	for size in OUTPUT_SIZES:
		await _check_layout(scene, size)
	scene.free()
	_report["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify({"test": "static_ui_content", "pass": _failures.is_empty(), "report": _report}))
	quit(0 if _failures.is_empty() else 1)


func _fail(message: String) -> void:
	_failures.append(message)
	printerr("check_static_ui FAIL: " + message)


func _check_world(scene: Node3D) -> void:
	if scene.get_script() != load(Generator.SCRIPT_PATH):
		_fail("the scene root does not carry static_ui.gd")
	var world := scene.get_node_or_null("UpscalerTorture")
	if world == null or world.scene_file_path != UpscalerTorture.SCENE_PATH:
		_fail("the scene does not instance %s" % UpscalerTorture.SCENE_PATH)
		return
	if world.get_script() != load("res://scenes/upscaler_torture/torture_timeline.gd"):
		_fail("the instanced world lost the Upscaler Torture timeline script")
	var path := world.get_node_or_null("CameraPath") as Path3D
	var camera := world.get_node_or_null("CameraPath/CameraFollow/Camera3D") as Camera3D
	if path == null or camera == null:
		_fail("the Upscaler Torture camera spline is missing")
		return
	if not path.curve.get_point_position(0).is_equal_approx(UpscalerTorture.STATIC_CAMERA_POSITION):
		_fail("the camera spline does not start at the Upscaler Torture static camera position")
	if not camera.current:
		_fail("the spline camera is not the current camera")
	_report["spline_points"] = path.curve.point_count


func _check_hud(scene: Node3D) -> void:
	var hud := scene.get_node_or_null("HUD") as CanvasLayer
	if hud == null:
		_fail("the scene has no HUD CanvasLayer")
		return
	if not hud.visible:
		_fail("the HUD is hidden in the static-UI scene")
	var sizes: Array = []
	for spec in Generator.LABELS:
		var label := hud.find_child(String(spec["name"]), true, false) as Label
		if label == null:
			_fail("%s is missing" % spec["name"])
			continue
		var font_size := label.get_theme_font_size("font_size")
		sizes.append(font_size)
		if font_size != int(spec["size"]):
			_fail("%s is %d px, expected %d px" % [spec["name"], font_size, spec["size"]])
		if not is_equal_approx(label.get_theme_color("font_color").a, 1.0):
			_fail("%s is not opaque" % spec["name"])
	sizes.sort()
	if sizes != [12, 18, 32]:
		_fail("the label sizes are %s, expected 12, 18 and 32 px" % str(sizes))
	var panel := hud.get_node_or_null("Layout/Panel") as ColorRect
	if panel == null or not is_equal_approx(panel.color.a, 0.5):
		_fail("the panel is missing or its alpha is not 0.5")
	elif panel.get_node_or_null("Label18") == null:
		_fail("the 18 px label is not on the panel")
	for line_name in ["CrosshairH", "CrosshairV"]:
		var line := hud.get_node_or_null("Layout/" + line_name) as ColorRect
		if line == null or line.color != Color.WHITE:
			_fail("%s is missing or is not opaque white" % line_name)
	var minimap := hud.get_node_or_null("Layout/Minimap") as TextureRect
	if minimap == null or minimap.texture == null:
		_fail("the minimap is missing")
		return
	var image := minimap.texture.get_image()
	if image.get_size() != Vector2i(Generator.MINIMAP_SIZE, Generator.MINIMAP_SIZE):
		_fail("the minimap texture is %s, expected 256 px" % str(image.get_size()))
	if image.detect_alpha() != Image.ALPHA_NONE:
		_fail("the minimap texture is not opaque")
	if minimap.texture_filter != CanvasItem.TEXTURE_FILTER_NEAREST:
		_fail("the minimap is not drawn with nearest filtering")
	_report["label_px"] = sizes
	_report["panel_alpha"] = panel.color.a if panel != null else -1.0


## The twin is the same scene file with one more line: the HUD's visible = false. The saver seeds
## the random part of an external resource id from the file's path, so only its index is compared.
func _check_twin() -> void:
	var resource_id := RegEx.new()
	resource_id.compile("\"(\\d+)_[a-z0-9]+\"")
	var scene_lines := resource_id.sub(FileAccess.get_file_as_string(Generator.SCENE_PATH), "\"$1\"", true).split("\n")
	var twin_lines := resource_id.sub(FileAccess.get_file_as_string(Generator.TWIN_SCENE_PATH), "\"$1\"", true).split("\n")
	var hud_line := twin_lines.find("[node name=\"HUD\" type=\"CanvasLayer\" parent=\".\"]")
	if hud_line < 0 or hud_line + 1 >= twin_lines.size() or twin_lines[hud_line + 1] != "visible = false":
		_fail("the twin's HUD is not hidden")
		return
	twin_lines.remove_at(hud_line + 1)
	if twin_lines != scene_lines:
		_fail("the twin differs from the scene in more than the HUD's visibility")
	var twin := (load(Generator.TWIN_SCENE_PATH) as PackedScene).instantiate()
	if (twin.get_node("HUD") as CanvasLayer).visible:
		_fail("the twin's HUD is visible once loaded")
	twin.free()
	_report["twin_extra_lines"] = 1


func _check_mask_threshold() -> void:
	var image := Image.create_empty(4, 1, false, Image.FORMAT_RGBA8)
	var alphas: Array[int] = [255, 253, 252, 0]
	for x in alphas.size():
		image.set_pixel(x, 0, Color8(200, 100, 50, alphas[x]))
	var mask := StaticUI.ui_mask(image)
	if mask.get_format() != Image.FORMAT_L8 or mask.get_data() != PackedByteArray([255, 255, 0, 0]):
		_fail("the UI mask does not keep exactly the pixels with alpha >= 0.99")
	if StaticUI.mask_pixel_count(mask) != 2:
		_fail("the UI mask pixel count is wrong")


## Lays the HUD out in a viewport of [param size] and checks every element's pixel rectangle.
func _check_layout(scene: Node3D, size: Vector2i) -> void:
	var viewport := SubViewport.new()
	viewport.size = size
	viewport.disable_3d = true
	var layout := scene.get_node("HUD/Layout").duplicate() as Control
	viewport.add_child(layout)
	root.add_child(viewport)
	await process_frame
	var margin := Generator.MARGIN
	var arm := Generator.CROSSHAIR_ARM
	var expected := {
		"Minimap": Rect2(size.x - margin - Generator.MINIMAP_SIZE, margin, Generator.MINIMAP_SIZE, Generator.MINIMAP_SIZE),
		"Panel": Rect2(margin, size.y - margin - Generator.PANEL_SIZE.y, Generator.PANEL_SIZE.x, Generator.PANEL_SIZE.y),
		"CrosshairH": Rect2(size.x / 2 - arm, size.y / 2, arm * 2 + 1, 1),
		"CrosshairV": Rect2(size.x / 2, size.y / 2 - arm, 1, arm * 2 + 1),
	}
	for node_name: String in expected:
		var rect := (layout.get_node(node_name) as Control).get_global_rect()
		if rect != expected[node_name]:
			_fail("%s is at %s at %dx%d, expected %s" % [node_name, str(rect), size.x, size.y, str(expected[node_name])])
	var frame := Rect2(Vector2.ZERO, size)
	var panel_rect: Rect2 = expected["Panel"]
	var minimap_rect: Rect2 = expected["Minimap"]
	var rows := {}
	for spec in Generator.LABELS:
		var label := layout.find_child(String(spec["name"]), true, false) as Label
		var rect := label.get_global_rect()
		rows[String(spec["name"])] = [rect.position.x, rect.position.y, rect.size.x, rect.size.y]
		if rect.size.y < float(spec["size"]) or rect.size.x <= 0.0:
			_fail("%s has no text rectangle at %dx%d" % [spec["name"], size.x, size.y])
		if not frame.encloses(rect) or rect.intersects(minimap_rect):
			_fail("%s leaves the frame or overlaps the minimap at %dx%d" % [spec["name"], size.x, size.y])
		var on_panel := label.get_parent().name == &"Panel"
		if on_panel and not panel_rect.encloses(rect):
			_fail("%s does not fit on the panel at %dx%d" % [spec["name"], size.x, size.y])
		if not on_panel and rect.intersects(panel_rect):
			_fail("%s overlaps the panel at %dx%d" % [spec["name"], size.x, size.y])
	_report["labels_%dx%d" % [size.x, size.y]] = rows
	root.remove_child(viewport)
	viewport.free()
