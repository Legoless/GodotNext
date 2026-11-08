extends Node3D

var _frames := 0
var _mode := "fsr2"
var _requested_mode := Viewport.SCALING_3D_MODE_FSR2
var _second_viewport: SubViewport


func _ready() -> void:
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--reset-backend="):
			_mode = argument.trim_prefix("--reset-backend=")
	if _mode == "metalfx":
		_requested_mode = Viewport.SCALING_3D_MODE_METALFX_TEMPORAL
	elif _mode.begins_with("copy"):
		_requested_mode = Viewport.SCALING_3D_MODE_DLSS
	elif _mode.begins_with("tsr"):
		_requested_mode = Viewport.SCALING_3D_MODE_TSR

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.02, 0.02, 0.02)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color.WHITE
	environment.ambient_light_energy = 0.5
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	add_child(world_environment)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.6, 3.0)
	camera.current = true
	add_child(camera)
	camera.look_at(Vector3.ZERO)
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-45.0, -30.0, 0.0)
	add_child(light)
	var cube := MeshInstance3D.new()
	cube.mesh = BoxMesh.new()
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.8, 0.2, 0.1)
	cube.material_override = material
	add_child(cube)

	RenderingServer.viewport_set_size(get_viewport().get_viewport_rid(), 1280, 720, 1)
	get_viewport().scaling_3d_scale = 0.75
	get_viewport().scaling_3d_mode = _requested_mode

	if _mode == "copy_second_viewport" or _mode == "tsr_second_viewport":
		_second_viewport = SubViewport.new()
		_second_viewport.size = Vector2i(1280, 720)
		_second_viewport.world_3d = get_viewport().world_3d
		_second_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
		add_child(_second_viewport)
		var second_camera := Camera3D.new()
		second_camera.transform = camera.transform
		_second_viewport.add_child(second_camera)
		second_camera.current = true
		_second_viewport.scaling_3d_scale = 0.75
		_second_viewport.scaling_3d_mode = _requested_mode


func _center_is_visible(image: Image) -> bool:
	if image == null:
		return false
	var center := image.get_pixel(image.get_width() / 2, image.get_height() / 2)
	return maxf(center.r, maxf(center.g, center.b)) > 0.1


func _process(_delta: float) -> void:
	_frames += 1
	if _frames == 5:
		print("UPSCALER_RESET_REQUEST mode=" + _mode)
		get_viewport().reset_temporal_history()
		if _second_viewport != null:
			_second_viewport.reset_temporal_history()
	if _frames < 8:
		return

	var status: Dictionary = RenderingServer.viewport_get_scaling_3d_status(get_viewport().get_viewport_rid())
	var ok := int(status["active_mode"]) == _requested_mode and _center_is_visible(get_viewport().get_texture().get_image())
	if _second_viewport != null:
		var second_status: Dictionary = RenderingServer.viewport_get_scaling_3d_status(_second_viewport.get_viewport_rid())
		ok = ok and int(second_status["active_mode"]) == _requested_mode
		ok = ok and _center_is_visible(_second_viewport.get_texture().get_image())
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "temporal_history_reset",
		"pass": ok,
		"mode": _mode,
		"active_mode": status["active_mode"],
		"viewports": 2 if _second_viewport != null else 1,
	}))
	get_tree().quit(0 if ok else 1)
