extends Node

## One SRC capture draw list, within the card and texel caps (#41).


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
	var camera := room.get_node("CaptureCam") as Camera3D
	if camera != null:
		camera.current = true
	var max_lists := 0
	var max_texels := 0
	var captures := 0
	for _frame in 16:
		await get_tree().process_frame
		var info := RenderingServer.nextgi_get_tier_info()
		max_lists = maxi(max_lists, int(info.get("src_capture_draw_lists", 0)))
		max_texels = maxi(max_texels, int(info.get("src_capture_texels", 0)))
		captures = maxi(captures, int(info.get("src_material_captures", 0)))
	var failures := PackedStringArray()
	if max_lists != 1:
		failures.append("draw lists %d" % max_lists)
	if max_texels > 1000000:
		failures.append("texels %d" % max_texels)
	if captures < 1:
		failures.append("material captures %d" % captures)
	return {"pass": failures.is_empty(), "failures": " | ".join(failures), "draw_lists": max_lists, "texels": max_texels, "captures": captures}
