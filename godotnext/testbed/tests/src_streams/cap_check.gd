extends Node

## High SRC relight stays inside the 256k texel and shadow-ray caps (#42).


func _ready() -> void:
	var result := await run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func run() -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(viewport)
	var room: Node3D = load("res://scenes/color_box_room/cbr_a.tscn").instantiate()
	viewport.add_child(room)
	var world_environment := room.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world_environment != null and world_environment.environment != null:
		var environment := world_environment.environment.duplicate()
		environment.nextgi_enabled = true
		environment.sdfgi_enabled = false
		world_environment.environment = environment
	var camera := room.get_node_or_null("CaptureCam") as Camera3D
	if camera != null:
		camera.current = true
	var max_texels := 0
	var max_rays := 0
	for _frame in 24:
		await get_tree().process_frame
		var info := RenderingServer.nextgi_get_tier_info()
		max_texels = maxi(max_texels, int(info.get("src_relight_texels", 0)))
		max_rays = maxi(max_rays, int(info.get("src_shadow_rays", info.get("src_relight_rays", 0))))
	var failures := PackedStringArray()
	if max_texels > 256 * 1024:
		failures.append("texels %d" % max_texels)
	if max_rays > 256 * 1024:
		failures.append("rays %d" % max_rays)
	return {"pass": failures.is_empty(), "failures": " | ".join(failures), "texels": max_texels, "rays": max_rays}
