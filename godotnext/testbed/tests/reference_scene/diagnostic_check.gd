extends Node

## Gather hit radiance uses the reference estimator (#27). One Color Box Room view.

const TARGET_RAYS := 256


func _ready() -> void:
	var result := await run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func run() -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
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
	var camera := room.get_node("CovCam0") as Camera3D
	camera.current = true
	var info := {}
	for _frame in 12:
		await get_tree().process_frame
		info = RenderingServer.nextgi_get_tier_info()
	var rays := int(info.get("src_gather_rays", 0))
	var diagnostic := bool(info.get("gather_hit_radiance_reference", false))
	var patches := _q1_patches(camera, info)
	var failures := PackedStringArray()
	if not diagnostic:
		failures.append("gather hit radiance stayed on SRC")
	if rays != TARGET_RAYS:
		failures.append("gather rays %d, expected %d" % [rays, TARGET_RAYS])
	if int(patches["count"]) < 8:
		failures.append("Q1 patches %d" % int(patches["count"]))
	return {
		"pass": failures.is_empty(),
		"failures": " | ".join(failures),
		"rays": rays,
		"diagnostic": diagnostic,
		"reference_mean": float(info.get("reference_mean", 0.0)),
		"q1_count": int(patches["count"]),
		"q1_mean": float(patches["mean"]),
		"q1_min": float(patches["min"]),
		"q1_max": float(patches["max"]),
	}


func _q1_patches(camera: Camera3D, info: Dictionary) -> Dictionary:
	var empty := {"count": 0, "mean": 0.0, "min": 0.0, "max": 0.0}
	if not info.has("reference_luminance"):
		return empty
	var luminance: PackedFloat32Array = info["reference_luminance"]
	var width := int(info.get("reference_width", 0))
	var height := int(info.get("reference_height", 0))
	if width <= 0 or height <= 0 or luminance.size() != width * height:
		return empty
	var document = JSON.parse_string(FileAccess.get_file_as_string("res://scenes/color_box_room/cbr_a_patches_q1.json"))
	if typeof(document) != TYPE_DICTIONARY:
		return empty
	var sum := 0.0
	var lo := 1.0e30
	var hi := -1.0e30
	var count := 0
	for patch in document["patches"]:
		if String(patch["camera"]) != "CovCam0":
			continue
		var pos_array: Array = patch["pos"]
		var screen := camera.unproject_position(Vector3(pos_array[0], pos_array[1], pos_array[2]))
		var view_size := camera.get_viewport().get_visible_rect().size
		if view_size.x <= 0.0 or view_size.y <= 0.0:
			continue
		var pixel := Vector2(screen.x / view_size.x * float(width), screen.y / view_size.y * float(height))
		if pixel.x < 0.0 or pixel.y < 0.0 or pixel.x >= float(width) or pixel.y >= float(height):
			continue
		var value := luminance[int(pixel.y) * width + int(pixel.x)]
		sum += value
		lo = minf(lo, value)
		hi = maxf(hi, value)
		count += 1
	if count == 0:
		return empty
	return {"count": count, "mean": sum / float(count), "min": lo, "max": hi}
