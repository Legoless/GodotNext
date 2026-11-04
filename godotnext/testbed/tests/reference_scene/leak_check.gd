extends Node

## Reference leak on Color Box Room A, B and the 40 m mesh (#27, Q3).
## Dark-room viewport mean divided by the lit-room mean must stay under 0.1%.

const SCENES := [
	"res://scenes/color_box_room/cbr_a.tscn",
	"res://scenes/color_box_room/cbr_b.tscn",
	"res://scenes/color_box_room/cbr_single_40m.tscn",
]
const TARGET_SAMPLES := 512
const LEAK_LIMIT := 0.001


func _ready() -> void:
	var result := await run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func run() -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(viewport)
	var failures := PackedStringArray()
	var rows := []
	for scene_path in SCENES:
		var row: Dictionary = await _measure(viewport, scene_path)
		rows.append(row)
		var leak := float(row["leak"])
		if int(row["lit_samples"]) < TARGET_SAMPLES or int(row["dark_samples"]) < TARGET_SAMPLES:
			failures.append("%s did not reach %d samples" % [scene_path, TARGET_SAMPLES])
		elif float(row["lit_mean"]) <= 0.0:
			failures.append("%s lit mean is 0" % scene_path)
		elif leak > LEAK_LIMIT:
			failures.append("%s leak %.6f" % [scene_path, leak])
	return {"pass": failures.is_empty(), "failures": " | ".join(failures), "scenes": rows}


func _measure(viewport: SubViewport, scene_path: String) -> Dictionary:
	for child in viewport.get_children():
		child.queue_free()
	await get_tree().process_frame
	var room: Node3D = load(scene_path).instantiate()
	viewport.add_child(room)
	_enable_nextgi(room)
	var lit := room.get_node("LeakCamLit") as Camera3D
	var dark := room.get_node("LeakCamDark") as Camera3D
	var lit_info := await _capture(lit, dark)
	var dark_info := await _capture(dark, lit)
	var lit_mean := float(lit_info.get("reference_mean", 0.0))
	var dark_mean := float(dark_info.get("reference_mean", 0.0))
	var leak := dark_mean / lit_mean if lit_mean > 0.0 else 1.0
	return {
		"scene": scene_path,
		"lit_mean": lit_mean,
		"dark_mean": dark_mean,
		"leak": leak,
		"lit_samples": int(lit_info.get("reference_samples", 0)),
		"dark_samples": int(dark_info.get("reference_samples", 0)),
	}


func _capture(active: Camera3D, other: Camera3D) -> Dictionary:
	other.current = false
	active.current = true
	var info := {}
	var saw_reset := false
	for _frame in 80:
		await get_tree().process_frame
		info = RenderingServer.nextgi_get_tier_info()
		var samples := int(info.get("reference_samples", 0))
		if samples < 64:
			saw_reset = true
		if saw_reset and samples >= TARGET_SAMPLES:
			break
	return info


func _enable_nextgi(room: Node) -> void:
	var world_environment := room.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world_environment == null or world_environment.environment == null:
		return
	var environment := world_environment.environment.duplicate()
	environment.nextgi_enabled = true
	environment.sdfgi_enabled = false
	world_environment.environment = environment
