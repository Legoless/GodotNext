extends SceneTree
## Static-UI scene for GU+-6 (#202, plan §21.5): Upscaler Torture (#95) with its seeded camera
## spline and timeline, under a fixed CanvasLayer HUD, and a twin whose HUD is hidden, the
## 3D-only reference. It is the PU+ phase smoke scene (§22.3).
##
## The HUD is laid out in pixels from the viewport's edges and center, so every element keeps its
## pixel size at any output size: three labels at 12, 18 and 32 px, a 50%-alpha panel, a 1-px
## crosshair and a 256-px minimap. The two scenes differ only in the HUD's [code]visible[/code].
##
## Usage: <godot> --headless --path godotnext/testbed --script res://generators/static_ui.gd

const Scaffold := preload("res://generators/scaffold.gd")
const UpscalerTorture := preload("res://generators/upscaler_torture.gd")

const SEED := 0x6E657874
const SCENE_PATH := "res://scenes/static_ui/static_ui.tscn"
const TWIN_SCENE_PATH := "res://scenes/static_ui/static_ui_3d_only.tscn"
const MINIMAP_PATH := "res://scenes/static_ui/minimap.res"
const SCRIPT_PATH := "res://scenes/static_ui/static_ui.gd"

## Pixels between the HUD and the viewport's edges.
const MARGIN := 32
const MINIMAP_SIZE := 256
const PANEL_SIZE := Vector2i(640, 96)
const PANEL_COLOR := Color(0.04, 0.06, 0.09, 0.5)
## The crosshair is two 1-px lines through the center pixel, this many pixels long each side of it.
const CROSSHAIR_ARM := 16
## Font size in pixels, position from the top left corner, and text. The 18 px label sits on the
## panel instead (its position is from the panel's top left corner).
const LABELS: Array[Dictionary] = [
	{"name": "Label12", "size": 12, "position": Vector2i(MARGIN, MARGIN), "text": "12 px  The quick brown fox jumps over the lazy dog  0123456789"},
	{"name": "Label32", "size": 32, "position": Vector2i(MARGIN, MARGIN + 28), "text": "32 px  STATIC UI  0123456789"},
	{"name": "Label18", "size": 18, "position": Vector2i(24, 34), "text": "18 px  HP 100 / 100   AMMO 30   WAVE 07   00:42.195"},
]


func _init() -> void:
	call_deferred("_run")


func _run() -> void:
	var failures := save_all()
	print("GODOTNEXT_RESULT " + JSON.stringify({"test": "static_ui_generator", "pass": failures.is_empty(), "seed": SEED, "failures": failures}))
	quit(0 if failures.is_empty() else 1)


## Writes [constant MINIMAP_PATH], the scene and its 3D-only twin. Returns the failures, empty when
## all three were saved.
static func save_all() -> Array[String]:
	var failures: Array[String] = []
	if not ResourceLoader.exists(UpscalerTorture.SCENE_PATH):
		failures.append("static_ui: %s is missing; generate it first." % UpscalerTorture.SCENE_PATH)
		return failures
	var minimap := _minimap_texture(failures)
	if minimap == null:
		return failures
	for job: Dictionary in [{"path": SCENE_PATH, "hud": true}, {"path": TWIN_SCENE_PATH, "hud": false}]:
		var root := build(bool(job["hud"]), minimap)
		var err := Scaffold.save_scene(root, String(job["path"]))
		root.free()
		if err != OK:
			failures.append("%s: save failed with error %d." % [job["path"], err])
		else:
			print("static_ui: wrote " + String(job["path"]))
	return failures


static func build(hud_visible: bool, minimap: Texture2D) -> Node3D:
	var root := Node3D.new()
	root.name = "StaticUI"
	root.set_script(load(SCRIPT_PATH))
	# The edit state makes the saved scene reference the torture scene instead of copying it.
	var world := (load(UpscalerTorture.SCENE_PATH) as PackedScene).instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	Scaffold.add_owned(root, world, root)
	var hud := CanvasLayer.new()
	hud.name = "HUD"
	hud.visible = hud_visible
	Scaffold.add_owned(root, hud, root)
	var layout := Control.new()
	layout.name = "Layout"
	layout.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	layout.mouse_filter = Control.MOUSE_FILTER_IGNORE
	Scaffold.add_owned(hud, layout, root)
	_add_panel(layout, root)
	_add_labels(layout, root)
	_add_crosshair(layout, root)
	_add_minimap(layout, root, minimap)
	return root


## Places [param control] by pixel offsets from the anchor point ([param anchor] of the parent's
## size): its top left corner at [param position] and its size [param size].
static func _place(control: Control, anchor: Vector2, position: Vector2i, size: Vector2i) -> void:
	control.anchor_left = anchor.x
	control.anchor_right = anchor.x
	control.anchor_top = anchor.y
	control.anchor_bottom = anchor.y
	control.offset_left = position.x
	control.offset_top = position.y
	control.offset_right = position.x + size.x
	control.offset_bottom = position.y + size.y
	control.mouse_filter = Control.MOUSE_FILTER_IGNORE


static func _label(spec: Dictionary) -> Label:
	var label := Label.new()
	label.name = String(spec["name"])
	label.text = String(spec["text"])
	label.add_theme_font_size_override("font_size", int(spec["size"]))
	label.add_theme_color_override("font_color", Color.WHITE)
	_place(label, Vector2.ZERO, spec["position"], Vector2i.ZERO)
	return label


## The 50%-alpha panel sits at the bottom left and carries the 18 px label.
static func _add_panel(layout: Control, root: Node3D) -> void:
	var panel := ColorRect.new()
	panel.name = "Panel"
	panel.color = PANEL_COLOR
	_place(panel, Vector2(0.0, 1.0), Vector2i(MARGIN, -MARGIN - PANEL_SIZE.y), PANEL_SIZE)
	Scaffold.add_owned(layout, panel, root)
	Scaffold.add_owned(panel, _label(LABELS[2]), root)


static func _add_labels(layout: Control, root: Node3D) -> void:
	Scaffold.add_owned(layout, _label(LABELS[0]), root)
	Scaffold.add_owned(layout, _label(LABELS[1]), root)


static func _add_crosshair(layout: Control, root: Node3D) -> void:
	var length := CROSSHAIR_ARM * 2 + 1
	var lines: Array[Dictionary] = [
		{"name": "CrosshairH", "position": Vector2i(-CROSSHAIR_ARM, 0), "size": Vector2i(length, 1)},
		{"name": "CrosshairV", "position": Vector2i(0, -CROSSHAIR_ARM), "size": Vector2i(1, length)},
	]
	for spec in lines:
		var line := ColorRect.new()
		line.name = String(spec["name"])
		line.color = Color.WHITE
		_place(line, Vector2(0.5, 0.5), spec["position"], spec["size"])
		Scaffold.add_owned(layout, line, root)


static func _add_minimap(layout: Control, root: Node3D, minimap: Texture2D) -> void:
	var rect := TextureRect.new()
	rect.name = "Minimap"
	rect.texture = minimap
	rect.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	rect.stretch_mode = TextureRect.STRETCH_KEEP
	_place(rect, Vector2(1.0, 0.0), Vector2i(-MARGIN - MINIMAP_SIZE, MARGIN), Vector2i(MINIMAP_SIZE, MINIMAP_SIZE))
	Scaffold.add_owned(layout, rect, root)


## An opaque 256-px map drawn one texel per pixel: seeded blocks, a 1-px road grid every 32 px,
## a 1-px border and a 3-px marker at the center. Saved to [constant MINIMAP_PATH]; when that fails,
## the error goes to [param failures] and the result is null.
static func _minimap_texture(failures: Array[String]) -> ImageTexture:
	var rng := RandomNumberGenerator.new()
	rng.seed = SEED
	var image := Image.create_empty(MINIMAP_SIZE, MINIMAP_SIZE, false, Image.FORMAT_RGB8)
	image.fill(Color(0.05, 0.07, 0.09))
	var block_colors: Array[Color] = [Color(0.2, 0.24, 0.3), Color(0.28, 0.3, 0.26), Color(0.33, 0.25, 0.22), Color(0.16, 0.3, 0.33)]
	for cell_y in MINIMAP_SIZE / 32:
		for cell_x in MINIMAP_SIZE / 32:
			var inset := rng.randi_range(2, 9)
			var color := block_colors[rng.randi_range(0, block_colors.size() - 1)]
			image.fill_rect(Rect2i(cell_x * 32 + inset, cell_y * 32 + inset, 32 - inset * 2, 32 - inset * 2), color)
	var road := Color(0.55, 0.57, 0.6)
	for index in range(0, MINIMAP_SIZE, 32):
		image.fill_rect(Rect2i(index, 0, 1, MINIMAP_SIZE), road)
		image.fill_rect(Rect2i(0, index, MINIMAP_SIZE, 1), road)
	var border := Color(0.9, 0.9, 0.88)
	image.fill_rect(Rect2i(0, 0, MINIMAP_SIZE, 1), border)
	image.fill_rect(Rect2i(0, MINIMAP_SIZE - 1, MINIMAP_SIZE, 1), border)
	image.fill_rect(Rect2i(0, 0, 1, MINIMAP_SIZE), border)
	image.fill_rect(Rect2i(MINIMAP_SIZE - 1, 0, 1, MINIMAP_SIZE), border)
	image.fill_rect(Rect2i(MINIMAP_SIZE / 2 - 1, MINIMAP_SIZE / 2 - 1, 3, 3), Color(1.0, 0.85, 0.2))
	var texture := ImageTexture.create_from_image(image)
	texture.resource_name = "minimap"
	texture.resource_path = MINIMAP_PATH
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(MINIMAP_PATH.get_base_dir()))
	var err := ResourceSaver.save(texture, MINIMAP_PATH)
	if err != OK:
		failures.append("%s: save failed with error %d." % [MINIMAP_PATH, err])
		return null
	return texture
