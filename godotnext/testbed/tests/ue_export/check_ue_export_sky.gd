extends SceneTree
## Compares an exported sky.hdr with direct linear renders of the source scene's Sky.
## Run rendered, with the scene's source tree and an export directory:
##
##   godot --path godotnext/testbed --rendering-driver vulkan \
##     --script res://tests/ue_export/check_ue_export_sky.gd -- /absolute/export
##
## Six ray samples cover the gradient and ground. The exported directional-light peak is rendered
## too, checking its direction and radiance. Geometry and post-processing stay out of the check.

const Lib := preload("res://tools/ue_export/ue_export_lib.gd")
const SAMPLES: Array[Vector2i] = [
	Vector2i(0, 0),
	Vector2i(100, 60),
	Vector2i(256, 127),
	Vector2i(300, 128),
	Vector2i(511, 255),
	Vector2i(64, 90),
]
const TOLERANCE := 0.02
const MAX_FRAMES := 600

var _frames := 0


func _initialize() -> void:
	call_deferred("_run")


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames > MAX_FRAMES:
		_fail("the sky comparison did not finish within %d frames" % MAX_FRAMES)
	return false


func _run() -> void:
	if DisplayServer.get_name() == "headless":
		_fail("the sky comparison needs a real renderer")
		return
	var args := OS.get_cmdline_user_args()
	if args.size() != 1 or not String(args[0]).is_absolute_path():
		_fail("pass one absolute export directory after --")
		return
	var export_dir := String(args[0])
	var scene_data: Variant = JSON.parse_string(FileAccess.get_file_as_string(export_dir.path_join("scene.json")))
	if not scene_data is Dictionary:
		_fail("scene.json does not decode")
		return
	var environment_data: Variant = scene_data.get("environment")
	if not environment_data is Dictionary:
		_fail("the scene has no environment")
		return
	var sky_entry: Variant = environment_data.get("sky")
	if not sky_entry is Dictionary:
		_fail("the scene has no exported sky")
		return
	if String(sky_entry.get("panorama", "")) != "sky.hdr" or String(sky_entry.get("panorama_projection", "")) != "godot_equirect":
		_fail("the scene does not name an exported Godot panorama")
		return
	var decoded := Lib.decode_hdr(FileAccess.get_file_as_bytes(export_dir.path_join("sky.hdr")))
	if decoded.has("error"):
		_fail("sky.hdr does not decode: %s" % decoded["error"])
		return
	var packed := load(String(scene_data["scene"])) as PackedScene
	if packed == null:
		_fail("the source scene does not load")
		return
	var shell := packed.instantiate()
	shell.set_meta("skip_runtime", true)
	get_root().add_child(shell)
	_hide_geometry(shell)
	var environment_node := shell.get_node_or_null(NodePath(String(environment_data.get("node", "")))) as WorldEnvironment
	if environment_node == null or environment_node.environment == null or environment_node.environment.sky == null:
		_fail("the source scene has no WorldEnvironment sky")
		return
	var environment := environment_node.environment
	if not environment.sky.sky_material is ProceduralSkyMaterial:
		_fail("the source sky is not procedural")
		return
	environment.sky.sky_material.use_debanding = false
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	environment.tonemap_exposure = 1.0
	environment.background_energy_multiplier = 1.0
	environment.glow_enabled = false
	environment.adjustment_enabled = false
	environment.fog_enabled = false
	environment.volumetric_fog_enabled = false
	var viewport := SubViewport.new()
	viewport.size = Vector2i(32, 32)
	viewport.world_3d = get_root().get_world_3d()
	viewport.use_hdr_2d = true
	viewport.use_debanding = false
	viewport.use_taa = false
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	viewport.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	var camera := Camera3D.new()
	camera.fov = 1.0
	camera.current = true
	viewport.add_child(camera)
	get_root().add_child(viewport)
	var size := Vector2i(int(decoded["width"]), int(decoded["height"]))
	var exported: PackedFloat32Array = decoded["pixels"]
	var failures: Array[String] = []
	var worst := 0.0
	for sample in SAMPLES:
		var base := (sample.y * size.x + sample.x) * 3
		var expected := Color(exported[base], exported[base + 1], exported[base + 2])
		var actual := await _render_ray(viewport, camera, Lib.panorama_direction(sample.x, sample.y, size.x, size.y))
		var error := _error(expected, actual)
		worst = maxf(worst, error)
		if error > TOLERANCE:
			failures.append("pixel %s: exported %s, rendered %s (relative error %.6f)" % [sample, expected, actual, error])
	var peak := _brightest_exported(exported, size)
	var peak_direction := Lib.panorama_direction(int(peak["x"]), int(peak["y"]), size.x, size.y)
	var rendered_peak := await _render_ray(viewport, camera, peak_direction)
	var peak_error := _error(peak["color"], rendered_peak)
	if peak_error > TOLERANCE:
		failures.append("peak radiance at %s: exported %s, rendered %s (relative error %.6f)" % [Vector2i(peak["x"], peak["y"]), peak["color"], rendered_peak, peak_error])
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "ue_export_sky",
		"scene": scene_data["scene"],
		"ok": failures.is_empty(),
		"samples": SAMPLES.size() + 1,
		"worst_sample_error": worst,
		"peak_error": peak_error,
		"peak_pixel": [peak["x"], peak["y"]],
		"size": [size.x, size.y],
		"failures": failures,
	}))
	quit(0 if failures.is_empty() else 1)


func _render_ray(p_viewport: SubViewport, p_camera: Camera3D, p_direction: Vector3) -> Color:
	var up := Vector3.RIGHT if absf(p_direction.y) > 0.999 else Vector3.UP
	p_camera.transform = Transform3D(Basis.looking_at(p_direction.normalized(), up), Vector3.ZERO)
	for frame in 3:
		await RenderingServer.frame_post_draw
	var image := p_viewport.get_texture().get_image()
	if image == null or image.is_empty() or image.get_size() != p_viewport.size:
		_fail("the renderer returned no sky image at the viewport's size")
		return Color.BLACK
	return image.get_pixelv(p_viewport.size / 2)


func _error(p_expected: Color, p_actual: Color) -> float:
	var scale := maxf(0.01, maxf(p_expected.r, maxf(p_expected.g, p_expected.b)))
	return maxf(absf(p_expected.r - p_actual.r), maxf(absf(p_expected.g - p_actual.g), absf(p_expected.b - p_actual.b))) / scale


func _brightest_exported(p_pixels: PackedFloat32Array, p_size: Vector2i) -> Dictionary:
	var result := {"x": 0, "y": 0, "color": Color.BLACK}
	var peak := -1.0
	for y in p_size.y:
		for x in p_size.x:
			var base := (y * p_size.x + x) * 3
			var color := Color(p_pixels[base], p_pixels[base + 1], p_pixels[base + 2])
			var luminance := color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			if luminance > peak:
				peak = luminance
				result = {"x": x, "y": y, "color": color}
	return result


func _hide_geometry(p_node: Node) -> void:
	if p_node is GeometryInstance3D:
		(p_node as GeometryInstance3D).visible = false
	for child in p_node.get_children():
		_hide_geometry(child)


func _fail(p_message: String) -> void:
	printerr("check_ue_export_sky.gd: " + p_message)
	quit(1)
