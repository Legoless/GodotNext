extends SceneTree

## Quits with the code given after "--" as --code=N (default 0) once a frame has been drawn, so a
## runner can check that it returns Godot's exit code (#237). It prints the rendering device and
## the display server and the runner's environment so the log shows where and how it ran.
##
## Usage: <godot> --path godotnext/testbed -s res://harness/tests/exit_code.gd -- --code=7

var _code := 0
var _frames := 0


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--code="):
			_code = int(arg.trim_prefix("--code="))
	print("exit_code: adapter=%s driver=%s display=%s" % [RenderingServer.get_video_adapter_name(), RenderingServer.get_current_rendering_driver_name(), DisplayServer.get_name()])
	for variable in ["GODOTNEXT_HARNESS_JOB", "NV_ALLOW_RAYTRACING_VALIDATION", "VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "GODOTNEXT_JOB_ENV_PROBE"]:
		print("exit_code: %s=%s" % [variable, OS.get_environment(variable)])


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames < 3:
		return false
	print("exit_code: quitting with %d after %d frames" % [_code, _frames])
	quit(_code)
	return false
