extends Node3D

# GodotNext: #69 draw-identity benchmark runner (-- --path=c|a|b [--warmup=120] [--frames=600]).

const DrawIdentityEffect := preload("res://bench/draw_identity/draw_identity_effect.gd")

var _effect: DrawIdentityEffect
var _printed := false

func _ready() -> void:
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	var path_mode := "c"
	var warmup := 120
	var frames := 600
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--path="):
			path_mode = arg.trim_prefix("--path=").to_lower()
		elif arg.begins_with("--warmup="):
			warmup = int(arg.trim_prefix("--warmup="))
		elif arg.begins_with("--frames="):
			frames = int(arg.trim_prefix("--frames="))

	if not (path_mode in ["c", "a", "b"]):
		printerr("DRAW_IDENTITY FAIL: invalid --path=" + path_mode)
		get_tree().quit(1)
		return

	_effect = DrawIdentityEffect.new()
	_effect.path_mode = path_mode
	_effect.warmup_frames = warmup
	_effect.measure_frames = frames
	if not _effect.prepare_dense_city_data():
		printerr("DRAW_IDENTITY FAIL: " + _effect.fail_reason)
		get_tree().quit(1)
		return

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.05, 0.05, 0.08)
	var comp := Compositor.new()
	comp.compositor_effects = [_effect]
	var we := WorldEnvironment.new()
	we.environment = env
	we.compositor = comp
	add_child(we)

	# Dense City cam_main exact transform/FOV/far.
	var cam := Camera3D.new()
	cam.transform = Transform3D(
		Vector3(0.0, 0.0, 1.0),
		Vector3(-0.026078084, 0.9996599, 0.0),
		Vector3(-0.9996599, -0.026078084, 0.0),
		Vector3(-260.0, 2.0, 0.0)
	)
	cam.fov = 70.0
	cam.far = 3000.0
	cam.current = true
	add_child(cam)

func _process(_delta: float) -> void:
	if _effect == null or _printed:
		return
	if _effect.failed:
		_printed = true
		printerr("DRAW_IDENTITY FAIL: " + _effect.fail_reason)
		RenderingServer.call_on_render_thread(_effect.cleanup_gpu)
		get_tree().quit(1)
		return
	if _effect.done:
		_printed = true
		print("DRAW_IDENTITY_RESULT " + JSON.stringify(_effect.result_summary))
		RenderingServer.call_on_render_thread(_effect.cleanup_gpu)
		get_tree().quit(0)
