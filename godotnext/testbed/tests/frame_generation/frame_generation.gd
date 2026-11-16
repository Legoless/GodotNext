extends Node
## Frame-generation split check (#203, GU+-1 subset and the GU+-6 content, plan §21.5 U2, U5).
##
## Shows the static-UI scene (#202) or its 3D-only twin ([code]--fg-scene=static_ui|twin[/code]).
## When [member ProjectSettings.rendering/frame_generation/provider] names the test-only
## SamplePassthrough provider (an override.cfg), the scene registers it and checks the seam on a
## real renderer: the provider is active at the requested multiplier, it gets exactly one
## _on_frame_inputs per rendered frame with the HUD-less colour, the UI layer, depth and the camera,
## _reset runs on the first frame and after the scene's history reset (frame 400), and on D3D12
## each presented frame gets exactly one _on_present pair (Vulkan and Metal have no present hook).
## Without that setting it checks that frame generation is off. With [code]--out=<dir>[/code] and
## [code]--fg-dump=<frame,...>[/code] it writes the HUD-less colour ([code]hudless_<frame>.png[/code],
## RGB) and the UI layer ([code]ui_<frame>.png[/code], premultiplied RGBA) of those frames, numbered
## like Movie Maker's frames, for comparison with captures of the other runs.
## [code]--fg-fallbacks[/code] then walks through the fallbacks a running game can reach (the reasons
## the provider reports, MFG at 2x, a second provider, an unregistered provider) and checks each
## status; run_frame_generation.sh checks that each fallback printed exactly one log line. The
## scene's own lines print reasons as numbers, so they never name a reason.
## [code]--fg-canvas-bg[/code] gives the scene a canvas background (a full-screen colour layer at
## -1, [constant Environment.BG_CANVAS] up to layer 0), so the split starts after the 3D, part way
## through the canvas layers. [code]--fg-expect-velocity[/code] (a run with a temporal upscaler)
## also checks that the inputs carry velocity, and a render size below the output size when the
## 3D scale is below 1.
## Every run checks the status's [code]split_textures[/code]: 2 while the sample provider is active,
## 0 while frame generation is off or the requested provider is not registered (which must report
## PROVIDER_UNAVAILABLE). [code]--fg-extra-viewport[/code] attaches a second 3D viewport to the main
## window: still one split and one _on_frame_inputs per frame. [code]--fg-second-window[/code] opens a
## second native window (a second swap chain on D3D12): _on_present still comes once per frame of the
## main window. [code]--fg-resize[/code] shrinks the window at frame 200: the first inputs of the new
## size reset. On D3D12 a present carries its frame inputs' index, except in the frame a swap chain is
## created or resized (the GPU is flushed before the present, see FrameGenerationInputs.frame_index):
## the second window's first frame and the resize frame, where the present's index is the larger.
## [code]--fg-scene=blend[/code] (with the sample provider) is a static scene whose HUD uses the Mix,
## Premultiplied Alpha and Add blend modes, a [code]blend_disabled[/code] item and LCD subpixel text.
## It reads back the frame with frame generation on, turns it off, and compares: the composite equals
## the UI layer over the HUD-less colour; pixels the UI layer leaves empty, and the LCD glyph cells'
## pixels with no coverage, are identical to the frame drawn without the split; the rest of the HUD
## is within 2/255, except LCD text, whose edges may only come out darker (see
## FrameGenerationExtension).

const SceneArgs := preload("res://scenes/scene_args.gd")
const Passthrough := preload("res://addons/sample_passthrough_fg/passthrough_fg.gd")

const SCENES := {
	"static_ui": "res://scenes/static_ui/static_ui.tscn",
	"twin": "res://scenes/static_ui/static_ui_3d_only.tscn",
	"blend": "",
}
const PROVIDER_SETTING := "rendering/frame_generation/provider"
const MULTIPLIER_SETTING := "rendering/frame_generation/multiplier"
const SECOND_PROVIDER := "SamplePassthroughB"
## The Upscaler Torture timeline resets the temporal history at its frame 400.
const TIMELINE_RESET_FRAME := 400
## Frames between a fallback step and its check.
const STEP_FRAMES := 4
## The blend check reads frame generation on at this frame, then off this many frames later (both
## long enough for the pipelines of each path to be compiled).
const BLEND_ON_FRAME := 90
const BLEND_SETTLE_FRAMES := 90
## Allowed difference, in 8-bit steps, where the composite rounds differently from drawing straight.
const BLEND_TOLERANCE := 2
## --fg-resize shrinks the window to three quarters at this frame.
const RESIZE_FRAME := 200

var _provider: Passthrough
var _second: Passthrough
var _requested := false
var _frames := 0
var _total := 600
var _dump := {}
var _out := ""
var _fallbacks := false
var _canvas_bg := false
var _expect_velocity := false
var _provider_setting := ""
var _blend := false
var _blend_lcd_rect := Rect2i()
var _blend_on := {}
var _extra_viewport := RID()
var _extra_camera := RID()
var _second_window: Window
var _resize := false
var _resize_from := Vector2i()
var _resize_to := Vector2i()
var _failures := PackedStringArray()
var _count_mismatches := 0
var _steps: Array[Dictionary] = []
var _step := -1
var _step_frame := 0
var _step_inputs := 0
var _done := false


func _ready() -> void:
	var scene_key := SceneArgs.value("--fg-scene", "static_ui")
	if not SCENES.has(scene_key):
		_fail("unknown --fg-scene %s" % scene_key)
		scene_key = "static_ui"
	_total = SceneArgs.int_value("--fg-frames", 600)
	_out = SceneArgs.value("--out", "")
	for item in SceneArgs.value("--fg-dump", "").split(",", false):
		if String(item).is_valid_int():
			_dump[int(item)] = true
	_fallbacks = OS.get_cmdline_user_args().has("--fg-fallbacks")
	_canvas_bg = OS.get_cmdline_user_args().has("--fg-canvas-bg")
	_expect_velocity = OS.get_cmdline_user_args().has("--fg-expect-velocity")
	_provider_setting = String(ProjectSettings.get_setting(PROVIDER_SETTING, ""))
	_requested = _provider_setting == Passthrough.PROVIDER_NAME
	if _requested:
		_provider = Passthrough.new()
		RenderingServer.register_frame_generation_extension(_provider)
	_blend = scene_key == "blend"
	var scene: Node
	if _blend:
		_expect(_requested, "the blend check needs the SamplePassthrough provider")
		_total = BLEND_ON_FRAME + BLEND_SETTLE_FRAMES
		scene = _build_blend_scene()
	else:
		scene = (load(SCENES[scene_key]) as PackedScene).instantiate()
	if _canvas_bg:
		_use_canvas_background(scene)
	add_child(scene)
	if OS.get_cmdline_user_args().has("--fg-extra-viewport"):
		_add_extra_viewport()
	if OS.get_cmdline_user_args().has("--fg-second-window"):
		get_viewport().gui_embed_subwindows = false
		_second_window = Window.new()
		_second_window.title = "Frame generation: second window"
		_second_window.size = Vector2i(320, 240)
		_second_window.position = Vector2i(60, 60)
		var label := Label.new()
		label.text = "Second window"
		_second_window.add_child(label)
		add_child(_second_window)
	_resize = OS.get_cmdline_user_args().has("--fg-resize")
	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)
	print("FG_SCENE scene=%s requested=%s provider=%s driver=%s method=%s frames=%d canvas_bg=%s extra_viewport=%s second_window=%s resize=%s msaa_2d=%d hdr_2d=%s scaling_3d_mode=%d scale=%.2f" % [scene_key, _requested, _provider_setting, RenderingServer.get_current_rendering_driver_name(), RenderingServer.get_current_rendering_method(), _total, _canvas_bg, _extra_viewport.is_valid(), _second_window != null, _resize, get_viewport().msaa_2d, get_viewport().use_hdr_2d, get_viewport().scaling_3d_mode, get_viewport().scaling_3d_scale])


## A second 3D viewport attached to the main window (a corner of it), on the scene's world. Frame
## generation must still split only one viewport.
func _add_extra_viewport() -> void:
	_extra_camera = RenderingServer.camera_create()
	RenderingServer.camera_set_perspective(_extra_camera, 60.0, 0.05, 1000.0)
	RenderingServer.camera_set_transform(_extra_camera, Transform3D(Basis(), Vector3(0.0, 2.0, 8.0)))
	_extra_viewport = RenderingServer.viewport_create()
	RenderingServer.viewport_set_size(_extra_viewport, 320, 180)
	RenderingServer.viewport_set_scenario(_extra_viewport, get_viewport().find_world_3d().scenario)
	RenderingServer.viewport_attach_camera(_extra_viewport, _extra_camera)
	RenderingServer.viewport_set_update_mode(_extra_viewport, RenderingServer.VIEWPORT_UPDATE_ALWAYS)
	RenderingServer.viewport_attach_to_screen(_extra_viewport, Rect2(0, 0, 320, 180), DisplayServer.MAIN_WINDOW_ID)
	RenderingServer.viewport_set_active(_extra_viewport, true)


## The blend check's scene: a still 3D view (no animation, no temporal effects) under a HUD with each
## blend mode the split composites.
func _build_blend_scene() -> Node:
	var root := Node3D.new()
	root.name = "BlendCheck"
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.32, 0.42, 0.55)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.45, 0.45, 0.45)
	var world := WorldEnvironment.new()
	world.environment = environment
	root.add_child(world)
	var light := DirectionalLight3D.new()
	light.transform = Transform3D(Basis.from_euler(Vector3(deg_to_rad(-50.0), deg_to_rad(30.0), 0.0)), Vector3.ZERO)
	root.add_child(light)
	var camera := Camera3D.new()
	camera.transform = Transform3D(Basis(), Vector3(0.0, 2.0, 9.0)).looking_at(Vector3(0.0, 1.0, 0.0))
	camera.current = true
	root.add_child(camera)
	# A wall of colored boxes behind the HUD, so every HUD item lies over varied 3D colors.
	for row in 5:
		for column in 9:
			var box := MeshInstance3D.new()
			var mesh := BoxMesh.new()
			mesh.size = Vector3(1.1, 1.1, 1.1)
			var material := StandardMaterial3D.new()
			material.albedo_color = Color.from_hsv(fmod(0.11 * column + 0.23 * row, 1.0), 0.65, 0.35 + 0.12 * (row % 4))
			mesh.material = material
			box.mesh = mesh
			box.position = Vector3(-6.0 + 1.5 * column, -1.4 + 1.25 * row, -2.0 - 0.3 * ((row + column) % 3))
			box.rotation = Vector3(0.3 * row, 0.4 * column, 0.0)
			root.add_child(box)

	var hud := CanvasLayer.new()
	hud.name = "HUD"
	hud.layer = 1
	root.add_child(hud)
	_add_rect(hud, Rect2(40, 40, 260, 110), Color(0.1, 0.8, 0.3, 0.6), CanvasItemMaterial.BLEND_MODE_MIX)
	_add_rect(hud, Rect2(330, 40, 260, 110), Color(1.0, 0.6, 0.2, 0.5), CanvasItemMaterial.BLEND_MODE_ADD)
	_add_rect(hud, Rect2(620, 40, 260, 110), Color(0.2, 0.3, 0.9, 1.0), CanvasItemMaterial.BLEND_MODE_ADD)
	_add_rect(hud, Rect2(910, 40, 260, 110), Color(0.3, 0.1, 0.4, 0.6), CanvasItemMaterial.BLEND_MODE_PREMULT_ALPHA)
	# A soft additive glow: alpha from 1 at the center to 0 at the edge.
	var glow := TextureRect.new()
	var gradient := Gradient.new()
	gradient.set_color(0, Color(1.0, 0.9, 0.5, 1.0))
	gradient.set_color(1, Color(1.0, 0.9, 0.5, 0.0))
	var glow_texture := GradientTexture2D.new()
	glow_texture.gradient = gradient
	glow_texture.fill = GradientTexture2D.FILL_RADIAL
	glow_texture.fill_from = Vector2(0.5, 0.5)
	glow_texture.fill_to = Vector2(1.0, 0.5)
	glow_texture.width = 200
	glow_texture.height = 200
	glow.texture = glow_texture
	glow.position = Vector2(40, 180)
	var glow_material := CanvasItemMaterial.new()
	glow_material.blend_mode = CanvasItemMaterial.BLEND_MODE_ADD
	glow.material = glow_material
	hud.add_child(glow)
	# blend_disabled at alpha 0.5: it replaces the 3D under it.
	var opaque := ColorRect.new()
	opaque.position = Vector2(270, 180)
	opaque.size = Vector2(260, 110)
	opaque.color = Color(0.9, 0.2, 0.1, 0.5)
	var shader := Shader.new()
	shader.code = "shader_type canvas_item;\nrender_mode blend_disabled;\n"
	var shader_material := ShaderMaterial.new()
	shader_material.shader = shader
	opaque.material = shader_material
	hud.add_child(opaque)
	# Grayscale-antialiased text (the default) and LCD subpixel text, white over the 3D.
	var gray := Label.new()
	gray.text = "Grayscale text 0123456789"
	gray.position = Vector2(560, 190)
	gray.add_theme_font_size_override("font_size", 36)
	gray.add_theme_color_override("font_color", Color.WHITE)
	hud.add_child(gray)
	var lcd := Label.new()
	lcd.text = "LCD subpixel text 0123456789 ABCDEFGHIJ"
	lcd.position = Vector2(40, 400)
	lcd.add_theme_font_size_override("font_size", 40)
	lcd.add_theme_color_override("font_color", Color.WHITE)
	var base_font := ThemeDB.fallback_font as FontFile
	_expect(base_font != null, "the fallback font is a FontFile (for LCD antialiasing)")
	if base_font != null:
		var lcd_font := FontFile.new()
		lcd_font.data = base_font.data
		lcd_font.antialiasing = TextServer.FONT_ANTIALIASING_LCD
		lcd.add_theme_font_override("font", lcd_font)
	hud.add_child(lcd)
	_expect(int(ProjectSettings.get_setting("gui/theme/lcd_subpixel_layout", 1)) != TextServer.FONT_LCD_SUBPIXEL_LAYOUT_NONE, "an LCD subpixel layout is set")
	# The label's cell on screen, a little larger (the window has no stretch, so canvas units are pixels).
	_blend_lcd_rect = Rect2i(36, 396, 960, 64)
	return root


func _add_rect(parent: Node, rect: Rect2, color: Color, blend_mode: CanvasItemMaterial.BlendMode) -> void:
	var item := ColorRect.new()
	item.position = rect.position
	item.size = rect.size
	item.color = color
	var material := CanvasItemMaterial.new()
	material.blend_mode = blend_mode
	item.material = material
	parent.add_child(item)


## The scene's environments draw the canvas up to layer 0 as their background, under the 3D; a
## full-screen colour layer at -1 is that background. The HUD (layer 1) stays above the 3D.
func _use_canvas_background(scene: Node) -> void:
	var environments := 0
	for node in scene.find_children("*", "WorldEnvironment", true, false):
		var world := node as WorldEnvironment
		if world.environment == null:
			continue
		var environment := world.environment.duplicate() as Environment
		environment.background_mode = Environment.BG_CANVAS
		environment.background_canvas_max_layer = 0
		world.environment = environment
		environments += 1
	_expect(environments > 0, "the scene has an environment to give a canvas background")
	var layer := CanvasLayer.new()
	layer.name = "CanvasBackground"
	layer.layer = -1
	var rect := ColorRect.new()
	rect.color = Color(0.25, 0.4, 0.55)
	rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	layer.add_child(rect)
	add_child(layer)


## A provider must be unregistered before its script goes away (a reloaded scene registers a new one).
func _exit_tree() -> void:
	for provider in [_provider, _second]:
		if provider != null:
			RenderingServer.unregister_frame_generation_extension(provider)
	for rid in [_extra_viewport, _extra_camera]:
		if rid.is_valid():
			RenderingServer.free_rid(rid)


func _fail(what: String) -> void:
	_failures.push_back(what)
	print("FG_FAIL %s status=%s" % [what, RenderingServer.get_frame_generation_status()])


func _expect(ok: bool, what: String) -> void:
	if not ok:
		_fail(what)


func _status() -> Dictionary:
	return RenderingServer.get_frame_generation_status()


func _driver_has_present_hook() -> bool:
	return RenderingServer.get_current_rendering_driver_name() == "d3d12"


func _process(_delta: float) -> void:
	_frames += 1
	if _frames == 10:
		_check_status()
	if _resize and _frames == RESIZE_FRAME:
		_resize_from = Vector2i(get_viewport().get_visible_rect().size)
		get_window().size = _resize_from * 3 / 4
	elif _resize and _frames == RESIZE_FRAME + 5:
		_resize_to = Vector2i(get_viewport().get_visible_rect().size)
		print("FG_RESIZE from=%s to=%s" % [_resize_from, _resize_to])
	if _blend:
		return
	if _frames == _total:
		_check_run()
		if _fallbacks and _requested:
			_build_steps()
			_next_step()
		else:
			_finish()
	elif _step >= 0 and not _done and _frames - _step_frame == STEP_FRAMES:
		_check_step()
		_next_step()


## Runs after each drawn frame: the provider got this frame's inputs, and the dumps are read back.
func _on_frame_post_draw() -> void:
	if _blend:
		_blend_frame()
		return
	if not _requested or _step >= 0:
		return
	if _provider.inputs_count != _frames:
		_count_mismatches += 1
		if _count_mismatches == 1:
			print("FG_FAIL _on_frame_inputs calls %d after %d frames" % [_provider.inputs_count, _frames])
	# Movie Maker numbers the frame drawn after the first _process 0.
	var movie_frame := _frames - 1
	if not _out.is_empty() and _dump.has(movie_frame):
		_write_dump(movie_frame)


func _write_dump(movie_frame: int) -> void:
	DirAccess.make_dir_recursive_absolute(_out)
	var hudless := _provider.read_hudless()
	var ui := _provider.read_ui()
	if hudless == null or ui == null:
		_fail("frame %d could not be read back" % movie_frame)
		return
	hudless.convert(Image.FORMAT_RGB8)
	ui.convert(Image.FORMAT_RGBA8)
	var hudless_path := _out.path_join("hudless_%08d.png" % movie_frame)
	var ui_path := _out.path_join("ui_%08d.png" % movie_frame)
	_expect(hudless.save_png(hudless_path) == OK, "write %s" % hudless_path)
	_expect(ui.save_png(ui_path) == OK, "write %s" % ui_path)
	print("FG_DUMP frame=%d size=%dx%d" % [movie_frame, hudless.get_width(), hudless.get_height()])


func _check_status() -> void:
	var status := _status()
	if not _requested:
		var expected_reason := FrameGenerationExtension.REASON_OK if _provider_setting.is_empty() else FrameGenerationExtension.REASON_PROVIDER_UNAVAILABLE
		_expect(not bool(status.get("active", true)), "frame generation is off")
		_expect(int(status.get("reason", -1)) == expected_reason, "reason %d while off (expected %d)" % [int(status.get("reason", -1)), expected_reason])
		_expect(int(status.get("multiplier", -1)) == 1, "multiplier 1 while off")
		_expect(int(status.get("split_textures", -1)) == 0, "the split holds no textures while off (%d)" % int(status.get("split_textures", -1)))
		print("FG_STATUS reason=%d active=%s multiplier=%d split_textures=%d" % [int(status.get("reason", -1)), status.get("active"), int(status.get("multiplier", -1)), int(status.get("split_textures", -1))])
		return
	var requested_multiplier := int(ProjectSettings.get_setting(MULTIPLIER_SETTING, 2))
	_expect(bool(status.get("active", false)), "the provider is active")
	_expect(String(status.get("active_provider", "")) == Passthrough.PROVIDER_NAME, "the active provider is the sample")
	_expect(int(status.get("reason", -1)) == FrameGenerationExtension.REASON_OK, "no fallback")
	_expect(int(status.get("multiplier", -1)) == requested_multiplier, "the multiplier is the requested one")
	_expect(bool(status.get("present_hook", false)) == _driver_has_present_hook(), "the present hook exists on D3D12 only")
	_expect(_provider.mode_count >= 1 and int(_provider.last_mode.get("multiplier", -1)) == requested_multiplier, "_set_mode got the multiplier")
	_expect(int(status.get("split_textures", -1)) == 2, "one viewport splits: 2 textures (%d)" % int(status.get("split_textures", -1)))
	print("FG_STATUS reason=%d active=%s multiplier=%d present_hook=%s split_textures=%d" % [int(status.get("reason", -1)), status.get("active"), int(status.get("multiplier", -1)), status.get("present_hook"), int(status.get("split_textures", -1))])


func _check_run() -> void:
	var split_textures := int(_status().get("split_textures", -1))
	if not _requested:
		_expect(split_textures == 0, "the split holds no textures at the end (%d)" % split_textures)
		return
	_expect(split_textures == 2, "one viewport splits at the end: 2 textures (%d)" % split_textures)
	var last: Dictionary = _provider.last
	var size := Vector2i(get_viewport().get_visible_rect().size)
	_expect(_count_mismatches == 0, "exactly one _on_frame_inputs per rendered frame (%d mismatched frames)" % _count_mismatches)
	_expect(bool(last.get("has_hudless", false)) and bool(last.get("has_ui", false)) and bool(last.get("has_depth", false)), "the HUD-less colour, the UI layer and depth are set")
	_expect(last.get("output_size", Vector2i()) == size, "the output size is the viewport size")
	var render_size: Vector2i = last.get("render_size", Vector2i())
	_expect(render_size.x > 0 and render_size.y > 0, "the render size is set")
	_expect(float(last.get("z_near", 0.0)) > 0.0 and float(last.get("z_far", 0.0)) > float(last.get("z_near", 0.0)), "near and far are set")
	_expect(float(last.get("fovy", 0.0)) > 1.0 and float(last.get("fovy", 0.0)) < 179.0, "the vertical field of view is in degrees")
	# The camera renders at the render size, so its aspect is that size's (the viewport's without scaling).
	_expect(render_size.y > 0 and is_equal_approx(float(last.get("aspect", 0.0)), float(render_size.x) / float(render_size.y)), "the aspect is the render size's")
	if _expect_velocity:
		_expect(bool(last.get("has_velocity", false)), "the inputs carry velocity")
		if get_viewport().scaling_3d_scale < 1.0:
			_expect(render_size.x < size.x and render_size.y < size.y, "the render size is below the output size")
	var size_changes := _provider.size_changed_at
	if _resize:
		_expect(_resize_to != _resize_from and _resize_to.x > 0, "the window was resized (%s to %s)" % [_resize_from, _resize_to])
		_expect(size_changes.size() == 1 and _provider.size_changed_reset.size() == 1 and _provider.size_changed_reset[0], "the first inputs of the new size reset (%s, %s)" % [size_changes, _provider.size_changed_reset])
	else:
		_expect(size_changes.is_empty(), "the output size never changes (%s)" % [size_changes])
	if _total > TIMELINE_RESET_FRAME + 4:
		var resets := _provider.reset_at
		var expected := 3 if _resize else 2
		_expect(resets.size() == expected, "%d resets: the first frame, %sthe timeline reset (%s)" % [expected, "the resize, " if _resize else "", resets])
		if resets.size() == expected:
			_expect(resets[0] == 1, "the first frame resets")
			if _resize and size_changes.size() == 1:
				_expect(resets[1] == size_changes[0], "the resize reaches _reset with the first inputs of the new size (%d, %d)" % [resets[1], size_changes[0]])
			_expect(resets[-1] > TIMELINE_RESET_FRAME and resets[-1] <= TIMELINE_RESET_FRAME + 3, "the timeline reset reaches _reset on the next frame (%d)" % resets[-1])
	if _driver_has_present_hook():
		var pairs := _provider.present_after_count
		_expect(_provider.present_order_errors == 0, "_on_present comes in before/after pairs")
		_expect(_provider.present_frame_errors == 0, "one _on_present pair per presented frame (increasing frame indices)")
		_expect(_provider.presents_without_inputs == 0, "every present follows its frame's inputs (%d without)" % _provider.presents_without_inputs)
		# A swap chain created or resized in a frame flushes the GPU before that frame's present, so that
		# present alone carries a larger index: the second window's first frame, and the resize frame.
		var expected_unmatched: Array[int] = []
		if _second_window != null:
			expected_unmatched.append(1)
		if _resize:
			expected_unmatched.append(size_changes[0] if size_changes.size() == 1 else RESIZE_FRAME)
		var unmatched_frames: Array[int] = []
		for entry in _provider.present_unmatched:
			unmatched_frames.append(int(entry["frame"]))
			_expect(int(entry["present_index"]) > int(entry["inputs_index"]), "an unmatched present carries the larger index (%s)" % [entry])
		print("FG_PRESENT matched=%d of %d unmatched=%s" % [_provider.present_matched, pairs, _provider.present_unmatched])
		_expect(unmatched_frames == expected_unmatched, "each present carries its frame inputs' index, except in the frames a swap chain is created or resized (%s, expected %s)" % [unmatched_frames, expected_unmatched])
		_expect(absi(pairs - _provider.inputs_count) <= 1, "one _on_present pair per rendered frame (%d pairs, %d frames)" % [pairs, _provider.inputs_count])
		_expect(_provider.present_swap_chain != 0, "_on_present gets the swap chain")
	else:
		_expect(_provider.present_before_count + _provider.present_after_count == 0, "no _on_present without a present hook")
	print("FG_RUN inputs=%d resets=%s presents=%d/%d order_errors=%d frame_errors=%d matched=%d velocity=%s render_size=%s output_size=%s" % [
		_provider.inputs_count, _provider.reset_at, _provider.present_before_count, _provider.present_after_count,
		_provider.present_order_errors, _provider.present_frame_errors, _provider.present_matched, last.get("has_velocity"), render_size, last.get("output_size")])


func _build_steps() -> void:
	_steps = [
		{"action": "force", "reason": FrameGenerationExtension.REASON_API_UNSUPPORTED, "active": false},
		{"action": "force", "reason": FrameGenerationExtension.REASON_FG_GPU_UNSUPPORTED, "active": false},
		{"action": "force", "reason": FrameGenerationExtension.REASON_NOT_NVIDIA_RTX, "active": false},
		{"action": "multiplier", "value": 4, "expect": FrameGenerationExtension.REASON_OK, "active": true, "multiplier": 4},
		{"action": "force", "reason": FrameGenerationExtension.REASON_MFG_REQUIRES_RTX50, "active": true, "multiplier": 2},
		{"action": "force", "reason": FrameGenerationExtension.REASON_OK, "active": true, "multiplier": 4},
		{"action": "second", "expect": FrameGenerationExtension.REASON_PROVIDER_ALREADY_ACTIVE, "active": true, "multiplier": 4},
		{"action": "first", "expect": FrameGenerationExtension.REASON_OK, "active": true, "multiplier": 4},
		{"action": "unregister", "expect": FrameGenerationExtension.REASON_PROVIDER_UNAVAILABLE, "active": false},
	]


func _next_step() -> void:
	_step += 1
	if _step >= _steps.size():
		_finish()
		return
	var step := _steps[_step]
	match String(step["action"]):
		"force":
			_provider.forced_reason = int(step["reason"])
			# Registering the same provider again has its availability checked again.
			RenderingServer.register_frame_generation_extension(_provider)
		"multiplier":
			_provider.forced_reason = FrameGenerationExtension.REASON_OK
			ProjectSettings.set_setting(MULTIPLIER_SETTING, int(step["value"]))
			RenderingServer.register_frame_generation_extension(_provider)
		"second":
			_second = Passthrough.new()
			_second.provider_name = SECOND_PROVIDER
			RenderingServer.register_frame_generation_extension(_second)
			ProjectSettings.set_setting(PROVIDER_SETTING, SECOND_PROVIDER)
		"first":
			ProjectSettings.set_setting(PROVIDER_SETTING, Passthrough.PROVIDER_NAME)
		"unregister":
			RenderingServer.unregister_frame_generation_extension(_provider)
	_step_frame = _frames
	_step_inputs = _provider.inputs_count


func _check_step() -> void:
	var step := _steps[_step]
	var status := _status()
	var expected := int(step.get("expect", step.get("reason", -1)))
	var reason := int(status.get("reason", -1))
	var active := bool(status.get("active", false))
	print("FG_FALLBACK step=%d reason=%d active=%s multiplier=%d" % [_step, reason, active, int(status.get("multiplier", -1))])
	_expect(reason == expected, "step %d: reason %d, expected %d" % [_step, reason, expected])
	_expect(active == bool(step["active"]), "step %d: active is %s" % [_step, active])
	if step.has("multiplier"):
		_expect(int(status.get("multiplier", -1)) == int(step["multiplier"]), "step %d: multiplier %d" % [_step, int(status.get("multiplier", -1))])
	if bool(step["active"]):
		_expect(String(status.get("active_provider", "")) == Passthrough.PROVIDER_NAME, "step %d: the first provider keeps running" % _step)
		_expect(_provider.inputs_count > _step_inputs, "step %d: the active provider still gets frame inputs" % _step)
	else:
		# Off: the split stops, so the provider gets no frame inputs (one may be in flight).
		_expect(_provider.inputs_count <= _step_inputs + 1, "step %d: no frame inputs while off" % _step)
	if _second != null:
		_expect(_second.inputs_count == 0 and _second.available_count == 0, "step %d: the second provider never runs" % _step)


## The blend check: the frame with frame generation on, then the same still frame with it off.
func _blend_frame() -> void:
	if _done:
		return
	if _frames == BLEND_ON_FRAME:
		var status := _status()
		_expect(bool(status.get("active", false)), "blend: frame generation is on")
		_expect(int(status.get("split_textures", -1)) == 2, "blend: one viewport splits (%d textures)" % int(status.get("split_textures", -1)))
		var final_image := get_viewport().get_texture().get_image()
		var ui := _provider.read_ui()
		var hudless := _provider.read_hudless()
		if final_image == null or ui == null or hudless == null:
			_fail("blend: the frame could not be read back")
			_finish()
			return
		# Whether the UI layer is empty is read from its own data: in HDR 2D (half floats) a coverage
		# too small for 8 bits is still drawn.
		_blend_on = {"ui_raw": ui.get_data(), "ui_half": ui.get_format() == Image.FORMAT_RGBAH}
		_blend_on.merge({"final": _rgba8(final_image), "ui": _rgba8(ui), "hudless": _rgba8(hudless)})
		ProjectSettings.set_setting(PROVIDER_SETTING, "")
	elif _frames == BLEND_ON_FRAME + BLEND_SETTLE_FRAMES:
		var status := _status()
		_expect(not bool(status.get("active", true)), "blend: frame generation is off")
		_expect(int(status.get("split_textures", -1)) == 0, "blend: the split freed its textures (%d)" % int(status.get("split_textures", -1)))
		if not _blend_on.is_empty():
			_blend_compare(_rgba8(get_viewport().get_texture().get_image()))
		_finish()


static func _rgba8(image: Image) -> Image:
	if image.get_format() != Image.FORMAT_RGBA8:
		image.convert(Image.FORMAT_RGBA8)
	return image


func _blend_compare(off_image: Image) -> void:
	var final_image: Image = _blend_on["final"]
	var ui_image: Image = _blend_on["ui"]
	var hudless_image: Image = _blend_on["hudless"]
	var size := final_image.get_size()
	for image: Image in [ui_image, hudless_image, off_image]:
		_expect(image.get_size() == size, "blend: image sizes match (%s, %s)" % [image.get_size(), size])
		if image.get_size() != size:
			return
	if not _out.is_empty():
		DirAccess.make_dir_recursive_absolute(_out)
		final_image.save_png(_out.path_join("blend_on.png"))
		off_image.save_png(_out.path_join("blend_off.png"))
		ui_image.save_png(_out.path_join("blend_ui.png"))
		hudless_image.save_png(_out.path_join("blend_hudless.png"))
	var on := final_image.get_data()
	var off := off_image.get_data()
	var ui := ui_image.get_data()
	var hudless := hudless_image.get_data()
	var ui_raw: PackedByteArray = _blend_on["ui_raw"]
	var ui_half: bool = _blend_on["ui_half"]
	var lcd := _blend_lcd_rect
	# Counters: pixels checked and pixels failing each rule, and the largest differences.
	var empty_px := 0
	var empty_bad := 0
	var composite_bad := 0
	var composite_max := 0
	var hud_px := 0
	var hud_bad := 0
	var hud_max := 0
	var lcd_covered_px := 0
	var lcd_fringe_px := 0
	var lcd_zero_px := 0
	var lcd_zero_bad := 0
	var lcd_brighter := 0
	var lcd_max_darker := 0
	for y in size.y:
		var lcd_row := y >= lcd.position.y and y < lcd.end.y
		for x in size.x:
			var i := (y * size.x + x) * 4
			var ua := ui[i + 3]
			var ui_rgb_zero: bool
			var ui_a_zero: bool
			if ui_half:
				# Half floats, little endian: zero (or minus zero) has no bits set but the sign.
				var k := i * 2
				ui_rgb_zero = ui_raw[k] == 0 and (ui_raw[k + 1] & 0x7f) == 0 and ui_raw[k + 2] == 0 and (ui_raw[k + 3] & 0x7f) == 0 and ui_raw[k + 4] == 0 and (ui_raw[k + 5] & 0x7f) == 0
				ui_a_zero = ui_raw[k + 6] == 0 and (ui_raw[k + 7] & 0x7f) == 0
			else:
				ui_rgb_zero = ui[i] == 0 and ui[i + 1] == 0 and ui[i + 2] == 0
				ui_a_zero = ua == 0
			# The composite is the UI layer over the HUD-less colour (exact where the layer is empty).
			var composite_diff := 0
			var on_off_max := 0
			var on_minus_off_max := -255
			for c in 3:
				var expected := mini(ui[i + c] + int(round(hudless[i + c] * (255 - ua) / 255.0)), 255)
				composite_diff = maxi(composite_diff, absi(on[i + c] - expected))
				var d := on[i + c] - off[i + c]
				on_off_max = maxi(on_off_max, absi(d))
				on_minus_off_max = maxi(on_minus_off_max, d)
			var in_lcd := lcd_row and x >= lcd.position.x and x < lcd.end.x
			if in_lcd and ui_rgb_zero:
				# White LCD text with no coverage in any channel (between glyphs, and the empty part of
				# each glyph's cell): nothing of the text is drawn, so the 3D must be exactly as without
				# the split.
				lcd_zero_px += 1
				if on_off_max != 0 or composite_diff != 0:
					lcd_zero_bad += 1
				continue
			if ui_rgb_zero and ui_a_zero:
				# Nothing of the HUD here: the split leaves the 3D exactly as it was drawn.
				empty_px += 1
				if on_off_max != 0 or composite_diff != 0:
					empty_bad += 1
				continue
			composite_max = maxi(composite_max, composite_diff)
			if composite_diff > BLEND_TOLERANCE:
				composite_bad += 1
			if in_lcd:
				lcd_covered_px += 1
				if maxi(ui[i], maxi(ui[i + 1], ui[i + 2])) - mini(ui[i], mini(ui[i + 1], ui[i + 2])) > 8:
					lcd_fringe_px += 1
				if on_minus_off_max > BLEND_TOLERANCE:
					lcd_brighter += 1
				for c in 3:
					lcd_max_darker = maxi(lcd_max_darker, off[i + c] - on[i + c])
				continue
			hud_px += 1
			hud_max = maxi(hud_max, on_off_max)
			if on_off_max > BLEND_TOLERANCE:
				hud_bad += 1
	print("FG_BLEND size=%dx%d empty=%d/%d composite_bad=%d composite_max=%d hud=%d/%d hud_max=%d lcd_covered=%d lcd_fringe=%d lcd_zero=%d/%d lcd_brighter=%d lcd_max_darker=%d" % [
		size.x, size.y, empty_bad, empty_px, composite_bad, composite_max, hud_bad, hud_px, hud_max,
		lcd_covered_px, lcd_fringe_px, lcd_zero_bad, lcd_zero_px, lcd_brighter, lcd_max_darker])
	_expect(empty_px > 0 and empty_bad == 0, "blend: where the UI layer is empty the frame is the 3D, as without the split (%d of %d differ)" % [empty_bad, empty_px])
	_expect(composite_bad == 0, "blend: the frame is the UI layer over the HUD-less colour (%d pixels off by more than %d)" % [composite_bad, BLEND_TOLERANCE])
	_expect(hud_px > 50000, "blend: the HUD covers the expected area (%d pixels)" % hud_px)
	_expect(hud_bad == 0, "blend: Mix, Premultiplied Alpha, Add and blend_disabled give the frame drawn without the split (%d pixels off by more than %d, max %d)" % [hud_bad, BLEND_TOLERANCE, hud_max])
	_expect(lcd_fringe_px > 0, "blend: the LCD label is drawn with subpixel coverage (%d coloured-edge pixels)" % lcd_fringe_px)
	_expect(lcd_zero_px > 0 and lcd_zero_bad == 0, "blend: LCD glyph cells leave the 3D where they cover nothing (%d of %d differ)" % [lcd_zero_bad, lcd_zero_px])
	_expect(lcd_brighter == 0, "blend: LCD text is never brighter than without the split (%d pixels)" % lcd_brighter)


func _finish() -> void:
	_done = true
	var ok := _failures.is_empty()
	var result := {"test": "frame_generation", "pass": ok, "requested": _requested, "frames": _frames, "failures": _failures}
	if _requested:
		result["inputs"] = _provider.inputs_count
		result["presents"] = _provider.present_after_count
		result["fallback_steps"] = maxi(_step, 0)
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if ok else 1)
