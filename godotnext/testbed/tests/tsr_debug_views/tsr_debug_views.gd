extends Node3D

## TSR pass visualizer check for #198.
##
## Loads Upscaler Torture under TSR at scale 0.667 with the preset of --tsr-preset= (low, medium,
## high, epic or cinematic; default high) and thin-geometry detection set by --tsr-thin= (on or
## off; default off). After WARMUP_FRAMES it renders each of the six views of
## rendering/anti_aliasing/tsr/debug_view with Viewport.DEBUG_DRAW_TSR and reads the image back.
## It fails unless:
## - every view differs from the normal image over most of the frame;
## - no view is a single color, and no two views are the same image, except that views whose
##   feature is off are all magenta (resurrection hits, flicker period and thin coverage);
## - a view whose feature is off is magenta over at least 99% of the sampled pixels, and a view
##   whose feature is on is not;
## - with thin geometry on, the thin-coverage view marks some pixels green (detections);
## - the visualizer's texture exists while a view is on, and is freed again (the "tsr_debug"
##   scope of the viewport's RenderSceneBuffersRD holds 0 bytes) within a few frames of turning
##   the debug draw off. A CompositorEffect on the camera reads the scope's size every frame.
##
## Run through run_tsr_debug_views.sh, e.g. on the Linux L4 machine:
##   tests/tsr_debug_views/run_tsr_debug_views.sh -- godot --path . --rendering-driver vulkan \
##     --gpu-validation --resolution 1920x1080 res://tests/tsr_debug_views/tsr_debug_views.tscn \
##     -- --tsr-preset=high --tsr-thin=on
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const SceneArgs := preload("res://scenes/scene_args.gd")

const WARMUP_FRAMES := 40
const VIEW_FRAMES := 6
const RELEASE_FRAMES := 3
const SCALE := 0.667
const DEBUG_VIEW_SETTING := "rendering/anti_aliasing/tsr/debug_view"
const THIN_SETTING := "rendering/anti_aliasing/tsr/thin_geometry_detection"

# TSRDebugView order (servers/rendering/renderer_rd/effects/tsr_stability.h).
const VIEW_NAMES := ["history_validity", "rejection", "disocclusion", "resurrection_hits", "flicker_period", "thin_coverage"]
const VIEW_RESURRECTION := 3
const VIEW_FLICKER := 4
const VIEW_THIN := 5

const PRESETS := {
	"low": Viewport.TSR_QUALITY_LOW,
	"medium": Viewport.TSR_QUALITY_MEDIUM,
	"high": Viewport.TSR_QUALITY_HIGH,
	"epic": Viewport.TSR_QUALITY_EPIC,
	"cinematic": Viewport.TSR_QUALITY_CINEMATIC,
}

# Sampling steps, in pixels: a fine grid for the coverage shares, a coarse one for "single color".
const SHARE_STEP := 2
const UNIFORM_STEP := 64
const MAGENTA_SHARE := 0.99
const DIFFERS_SHARE := 0.5
const MIN_THIN_GREEN := 32


## Reads the size of the visualizer's scope on the render thread, once per frame.
class TsrDebugScopeProbe extends CompositorEffect:
	var bytes := -1
	var target := Vector2i()

	func _init() -> void:
		effect_callback_type = CompositorEffect.EFFECT_CALLBACK_TYPE_POST_TRANSPARENT

	func _render_callback(_effect_callback_type: int, render_data: RenderData) -> void:
		var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
		if buffers == null:
			return
		bytes = buffers.get_context_memory_bytes(&"tsr_debug")
		target = buffers.get_target_size()


var _failures := PackedStringArray()
var _probe := TsrDebugScopeProbe.new()


func _ready() -> void:
	var preset_name := SceneArgs.value("--tsr-preset", "high").to_lower()
	var thin := SceneArgs.value("--tsr-thin", "off").to_lower() == "on"
	if not PRESETS.has(preset_name):
		_finish(preset_name, thin, {}, ["unknown --tsr-preset '%s'" % preset_name])
		return
	ProjectSettings.set_setting(THIN_SETTING, thin)
	ProjectSettings.set_setting(DEBUG_VIEW_SETTING, 0)

	var packed: PackedScene = load("res://scenes/upscaler_torture/upscaler_torture.tscn")
	var scene := packed.instantiate()
	add_child(scene)
	var camera := scene.get_node_or_null("CameraPath/CameraFollow/Camera3D") as Camera3D
	if camera == null:
		_finish(preset_name, thin, {}, ["Upscaler Torture has no CameraPath/CameraFollow/Camera3D"])
		return
	camera.current = true
	var compositor := Compositor.new()
	compositor.compositor_effects = [_probe]
	camera.compositor = compositor

	var viewport := get_viewport()
	viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_TSR
	viewport.scaling_3d_scale = SCALE
	viewport.tsr_quality = PRESETS[preset_name]
	viewport.debug_draw = Viewport.DEBUG_DRAW_DISABLED
	_run(preset_name, thin)


func _run(preset_name: String, thin: bool) -> void:
	var viewport := get_viewport()
	for i in WARMUP_FRAMES:
		await RenderingServer.frame_post_draw
	var normal := viewport.get_texture().get_image()
	if normal == null or normal.is_empty():
		_finish(preset_name, thin, {}, ["the normal image is empty"])
		return
	var normal_bytes := _probe.bytes
	if normal_bytes != 0:
		_failures.append("the tsr_debug scope holds %d bytes before any view is on" % normal_bytes)

	var preset := int(PRESETS[preset_name])
	var feature_on := {
		VIEW_RESURRECTION: preset >= Viewport.TSR_QUALITY_MEDIUM,
		VIEW_FLICKER: preset >= Viewport.TSR_QUALITY_HIGH,
		VIEW_THIN: thin,
	}

	var hashes := {}
	var views := {}
	for view in VIEW_NAMES.size():
		var view_name: String = VIEW_NAMES[view]
		ProjectSettings.set_setting(DEBUG_VIEW_SETTING, view)
		viewport.debug_draw = Viewport.DEBUG_DRAW_TSR
		for i in VIEW_FRAMES:
			await RenderingServer.frame_post_draw
		var image := viewport.get_texture().get_image()
		if image == null or image.is_empty():
			_failures.append("%s produced no image" % view_name)
			continue
		var stats := _stats(image, normal)
		stats["scope_bytes"] = _probe.bytes
		views[view_name] = stats

		var expected_bytes := _probe.target.x * _probe.target.y * 4
		if _probe.bytes != expected_bytes:
			_failures.append("%s: the tsr_debug scope holds %d bytes while the view is on, expected %d (RGBA8 at %s)" % [view_name, _probe.bytes, expected_bytes, str(_probe.target)])
		if float(stats["differs"]) < DIFFERS_SHARE:
			_failures.append("%s differs from the normal image on only %.3f of the sampled pixels" % [view_name, float(stats["differs"])])

		var off: bool = feature_on.has(view) and not bool(feature_on[view])
		if off:
			if float(stats["magenta"]) < MAGENTA_SHARE:
				_failures.append("%s: the feature is off, but only %.4f of the sampled pixels are magenta" % [view_name, float(stats["magenta"])])
			continue
		if float(stats["magenta"]) >= MAGENTA_SHARE:
			_failures.append("%s: the feature is on, but %.4f of the sampled pixels are magenta" % [view_name, float(stats["magenta"])])
		if bool(stats["uniform"]):
			_failures.append("%s rendered a single color" % view_name)
		var image_hash := _hash(image)
		if hashes.has(image_hash):
			_failures.append("%s is the same image as %s" % [view_name, hashes[image_hash]])
		hashes[image_hash] = view_name
		if view == VIEW_THIN and int(stats["green"]) < MIN_THIN_GREEN:
			_failures.append("thin_coverage marks only %d sampled pixels green with thin geometry on" % int(stats["green"]))

	# The texture is freed on the first frame the view is off; the probe reports the frame after.
	viewport.debug_draw = Viewport.DEBUG_DRAW_DISABLED
	for i in RELEASE_FRAMES:
		await RenderingServer.frame_post_draw
	var released_bytes := _probe.bytes
	if released_bytes != 0:
		_failures.append("the tsr_debug scope still holds %d bytes %d frames after the debug draw was turned off" % [released_bytes, RELEASE_FRAMES])

	views["released_bytes"] = released_bytes
	_finish(preset_name, thin, views, [])


# Shares over a SHARE_STEP grid: magenta pixels, pixels that differ from the normal image, and the
# count of green-overlay pixels; "uniform" over a coarse grid.
func _stats(image: Image, normal: Image) -> Dictionary:
	var samples := 0
	var magenta := 0
	var differs := 0
	var green := 0
	var same_size := normal.get_size() == image.get_size()
	for y in range(0, image.get_height(), SHARE_STEP):
		for x in range(0, image.get_width(), SHARE_STEP):
			var c := image.get_pixel(x, y)
			samples += 1
			if c.r > 0.9 and c.g < 0.1 and c.b > 0.9:
				magenta += 1
			if c.g > 0.35 and c.g > c.r * 2.0 and c.g > c.b * 2.0:
				green += 1
			if same_size:
				var n := normal.get_pixel(x, y)
				if absf(c.r - n.r) > 0.05 or absf(c.g - n.g) > 0.05 or absf(c.b - n.b) > 0.05:
					differs += 1
	var first := image.get_pixel(0, 0)
	var uniform := true
	for y in range(0, image.get_height(), UNIFORM_STEP):
		for x in range(0, image.get_width(), UNIFORM_STEP):
			if image.get_pixel(x, y) != first:
				uniform = false
				break
		if not uniform:
			break
	return {
		"magenta": float(magenta) / float(maxi(samples, 1)),
		"differs": float(differs) / float(maxi(samples, 1)),
		"green": green,
		"uniform": uniform,
	}


func _hash(image: Image) -> String:
	var ctx := HashingContext.new()
	ctx.start(HashingContext.HASH_SHA256)
	ctx.update(image.get_data())
	return ctx.finish().hex_encode()


func _finish(preset_name: String, thin: bool, views: Dictionary, extra_failures: Array) -> void:
	for failure in extra_failures:
		_failures.append(String(failure))
	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "tsr_debug_views",
		"pass": ok,
		"preset": preset_name,
		"thin_geometry": thin,
		"scale": SCALE,
		"views": views,
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)
