extends Node3D
## Static-UI scene (#202, GU+-6, plan §21.5): Upscaler Torture under a fixed HUD.
##
## The instanced Upscaler Torture scene drives the camera spline and the timeline by frame index,
## exactly as it does on its own. This script only dumps the UI mask: once per scene instance,
## when the run has an output folder ([code]--out=<absolute dir>[/code], the harness's or a
## standalone run's) and the HUD is visible, it writes [code]cmask_ui.png[/code] there, the
## content mask "ui" of [code]docs/godotnext/metrics.md[/code] (one image for every frame). The
## mask is a single-channel PNG at the viewport's size (the output size, unless the desktop clamped
## the window), white where the HUD's own alpha is at least 0.99. The HUD is drawn alone into a
## transparent viewport of that size, so the mask does not depend on the 3D frame behind it.

const SceneArgs := preload("res://scenes/scene_args.gd")

const UI_MASK_FILE := "cmask_ui.png"
## A pixel is in the UI mask when the HUD's alpha there is at least this.
const UI_ALPHA_MIN := 0.99


func _ready() -> void:
	set_process(not bool(get_meta("skip_runtime", false)))


## The first frame, not _ready: by then a harness run has set the output size.
func _process(_delta: float) -> void:
	set_process(false)
	var hud := get_node("HUD") as CanvasLayer
	var out_dir := SceneArgs.value("--out", "")
	print("STATIC_UI hud_visible=%s" % hud.visible)
	if hud.visible and not out_dir.is_empty():
		_dump_ui_mask(hud, out_dir)


func _dump_ui_mask(hud: CanvasLayer, out_dir: String) -> void:
	var viewport := get_viewport()
	var mask_viewport := SubViewport.new()
	mask_viewport.name = "UIMaskViewport"
	mask_viewport.size = Vector2i(viewport.get_visible_rect().size)
	mask_viewport.transparent_bg = true
	mask_viewport.disable_3d = true
	mask_viewport.gui_snap_controls_to_pixels = viewport.gui_snap_controls_to_pixels
	mask_viewport.render_target_update_mode = SubViewport.UPDATE_ONCE
	mask_viewport.add_child(hud.get_node("Layout").duplicate())
	add_child(mask_viewport)
	await RenderingServer.frame_post_draw
	var image := mask_viewport.get_texture().get_image()
	mask_viewport.queue_free()
	if image == null or image.is_empty():
		push_error("static_ui: the UI mask viewport did not render.")
		return
	var mask := ui_mask(image)
	DirAccess.make_dir_recursive_absolute(out_dir)
	var path := out_dir.path_join(UI_MASK_FILE)
	var err := mask.save_png(path)
	if err != OK:
		push_error("static_ui: cannot write %s (%s)." % [path, error_string(err)])
		return
	print("STATIC_UI ui_mask=%s size=%dx%d pixels=%d" % [UI_MASK_FILE, mask.get_width(), mask.get_height(), mask_pixel_count(mask)])


## White where [param image]'s alpha is at least [constant UI_ALPHA_MIN], black elsewhere.
static func ui_mask(image: Image) -> Image:
	var rgba := image.duplicate() as Image
	rgba.convert(Image.FORMAT_RGBA8)
	var source := rgba.get_data()
	var count := rgba.get_width() * rgba.get_height()
	# The smallest 8-bit alpha that is at least UI_ALPHA_MIN (253 for 0.99).
	var minimum := ceili(UI_ALPHA_MIN * 255.0)
	var data := PackedByteArray()
	data.resize(count)
	for index in count:
		if source[index * 4 + 3] >= minimum:
			data[index] = 255
	return Image.create_from_data(rgba.get_width(), rgba.get_height(), false, Image.FORMAT_L8, data)


static func mask_pixel_count(mask: Image) -> int:
	var count := 0
	for value in mask.get_data():
		if value != 0:
			count += 1
	return count
