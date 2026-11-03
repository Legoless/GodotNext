extends Node3D
## Feature Parity Room (#64): fixed-capture setup.
##
## Selects the camera given by the user argument [code]--camera=cam_N[/code]
## (default [code]cam_0[/code]) and forces the capture contract: TAA, FXAA
## (screen-space AA) and MSAA off. The scene's [Environment] already uses
## the linear tonemapper at exposure 1.0. When the #12 harness runs the
## scene in capture mode it applies the same settings, so this only matters
## for plain [code]--write-movie[/code] runs.

const DEFAULT_CAMERA := "cam_0"


func _ready() -> void:
	var viewport := get_viewport()
	viewport.use_taa = false
	viewport.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	var selected := DEFAULT_CAMERA
	for arg in OS.get_cmdline_user_args():
		var text := String(arg)
		if text.begins_with("--camera="):
			selected = text.substr("--camera=".length())
	var camera := get_node_or_null(selected) as Camera3D
	if camera == null:
		push_error("parity_room: camera '%s' not found; keeping %s." % [selected, DEFAULT_CAMERA])
		return
	for name in ["cam_0", "cam_1", "cam_2"]:
		var other := get_node_or_null(name) as Camera3D
		if other != null:
			other.current = other == camera
