extends Node3D
## Upscaler fallback check (#103, GU-3). Requests DLSS and prints the
## resolved status after a few frames. On Metal the API reason applies and
## the active mode is MetalFX temporal when supported, else FSR2.

var _frames := 0


func _ready() -> void:
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 3.0)
	camera.current = true
	add_child(camera)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50.0, -30.0, 0.0)
	add_child(sun)
	get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_DLSS
	get_viewport().scaling_3d_scale = 0.75


func _process(_delta: float) -> void:
	_frames += 1
	if _frames == 5:
		var status: Dictionary = RenderingServer.viewport_get_scaling_3d_status(get_viewport().get_viewport_rid())
		print("UPSCALER_STATUS requested=%s active=%s reason=%s scale=%s preset=%s" % [
			status.get("requested_mode", -1),
			status.get("active_mode", -1),
			status.get("reason", -1),
			status.get("effective_scale", -1.0),
			status.get("preset", -1),
		])
		print("UPSCALER_API dlss_availability=%s" % RenderingServer.scaling_3d_mode_get_availability(RenderingServer.VIEWPORT_SCALING_3D_MODE_DLSS))
		get_tree().quit(0)
