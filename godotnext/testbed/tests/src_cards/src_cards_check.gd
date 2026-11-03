extends Node

## NextGI SRC Stage-1 card check (#30; plan §8.3 and D6).
##
## Loads a testbed scene, turns nextgi_enabled on in its Environment, turns every OmniLight3D's
## shadow on and adds a shadowed DirectionalLight3D, then renders --frames frames (600 by default)
## and reads the src_* counters back through RenderingServer.nextgi_get_tier_info().
## run_src_cards_check.sh writes res://override.cfg (git ignores it) with nextgi/enabled=true and
## nextgi/debug/dump_material_atlas=true for the run and removes it afterwards:
##   godotnext/testbed/tests/src_cards/run_src_cards_check.sh -- <godot> --path godotnext/testbed \
##     --rendering-driver metal res://tests/src_cards/src_cards_check.tscn -- \
##     --scene=res://scenes/color_box_room/cbr_a.tscn --min-coverage=0.95
##
## On the HWRT tier it requires: at least one card set; at most 12 cards per set and 128x128 texels
## per card; every card captured exactly once over the run (src_capture_count equals
## src_card_count and no card is captured twice); no capture request inside a shadow batch; the
## material atlas logged at no more than 128 MiB; and, with the debug dump on, one coverage value per
## card, each at least --min-coverage when that is given. Card counts and per-card coverage go into
## the result line either way (the input to G1a's coverage row, #29 and #32).
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.

const MATERIAL_BUDGET := 128 * 1024 * 1024
const MAX_CARDS_PER_SET := 12
const MAX_CARD_TEXELS := 128
# The dump's downloads arrive a few frames after the last capture.
const DUMP_WAIT_FRAMES := 120

var _failures := PackedStringArray()


func _ready() -> void:
	var result := await run(OS.get_cmdline_user_args())
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _check(condition: bool, what: String) -> void:
	if not condition:
		_failures.append(what)


func run(args: PackedStringArray) -> Dictionary:
	var scene_path := "res://scenes/color_box_room/cbr_a.tscn"
	var frames := 600
	var min_coverage := -1.0
	for arg in args:
		if arg.begins_with("--scene="):
			scene_path = arg.trim_prefix("--scene=")
		elif arg.begins_with("--frames="):
			frames = int(arg.trim_prefix("--frames="))
		elif arg.begins_with("--min-coverage="):
			min_coverage = float(arg.trim_prefix("--min-coverage="))

	var enabled: bool = ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false)
	var dump: bool = ProjectSettings.get_setting("rendering/global_illumination/nextgi/debug/dump_material_atlas", false)

	var packed: PackedScene = load(scene_path)
	_check(packed != null, "scene loads: " + scene_path)
	if packed == null:
		return _result(scene_path, enabled, dump, {})

	var scene_root: Node3D = packed.instantiate()
	add_child(scene_root)
	_prepare(scene_root)

	var info: Dictionary = {}
	for frame in frames:
		await get_tree().process_frame
		info = RenderingServer.nextgi_get_tier_info()
	if dump and info.get("tier", "") == "HWRT":
		for frame in DUMP_WAIT_FRAMES:
			if bool(info.get("src_dump_done", false)):
				break
			await get_tree().process_frame
			info = RenderingServer.nextgi_get_tier_info()

	if not enabled or info.get("tier", "") != "HWRT":
		# No frame block runs outside the HWRT tier, so there is no SRC.
		_check(not info.has("src_card_count"), "outside the HWRT tier there is no SRC")
		return _result(scene_path, enabled, dump, info)

	var card_count := int(info.get("src_card_count", 0))
	_check(int(info.get("src_card_set_count", 0)) > 0, "at least one card set")
	_check(card_count > 0, "at least one card")
	_check(int(info.get("src_captured_card_count", -1)) == card_count, "every card is captured")
	_check(int(info.get("src_capture_count", -1)) == card_count, "%d captures for %d cards: each card exactly once" % [int(info.get("src_capture_count", -1)), card_count])
	_check(int(info.get("src_max_captures_per_card", -1)) == 1, "no card is captured twice")
	_check(int(info.get("src_shadow_batch_refusals", -1)) == 0, "no capture request inside a shadow batch")
	var memory := int(info.get("src_atlas_memory", 0))
	_check(memory > 0 and memory <= MATERIAL_BUDGET, "material atlas within 128 MiB (%d bytes)" % memory)

	var set_counts: PackedInt32Array = info.get("src_card_set_card_counts", PackedInt32Array())
	for count in set_counts:
		_check(count >= 1 and count <= MAX_CARDS_PER_SET, "a card set has %d cards" % count)
	var sizes: PackedInt32Array = info.get("src_card_sizes", PackedInt32Array())
	_check(sizes.size() == card_count * 2, "one size per card")
	for value in sizes:
		_check(value >= 4 and value <= MAX_CARD_TEXELS, "card side of %d texels" % value)

	if dump:
		_check(bool(info.get("src_dump_done", false)), "the material atlas dump finished")
		var coverage: PackedFloat32Array = info.get("src_card_coverage", PackedFloat32Array())
		_check(coverage.size() == card_count, "one coverage value per card")
		if min_coverage >= 0.0:
			for i in coverage.size():
				_check(coverage[i] >= min_coverage, "card %d coverage %.3f is under %.2f" % [i, coverage[i], min_coverage])
	return _result(scene_path, enabled, dump, info)


func _prepare(node: Node) -> void:
	var has_sun := false
	var stack: Array[Node] = [node]
	while not stack.is_empty():
		var current: Node = stack.pop_back()
		if current is WorldEnvironment:
			var world_environment := current as WorldEnvironment
			if world_environment.environment != null:
				var environment: Environment = world_environment.environment.duplicate()
				environment.nextgi_enabled = true
				world_environment.environment = environment
		elif current is OmniLight3D:
			(current as OmniLight3D).shadow_enabled = true
		elif current is DirectionalLight3D:
			(current as DirectionalLight3D).shadow_enabled = true
			has_sun = true
		elif current is Camera3D and current.name == "CaptureCam":
			(current as Camera3D).current = true
		for child in current.get_children():
			stack.append(child)
	# Captures must stay after _render_shadow_end with directional and omni shadows on.
	if not has_sun:
		var sun := DirectionalLight3D.new()
		sun.shadow_enabled = true
		sun.rotation_degrees = Vector3(-50.0, 30.0, 0.0)
		node.add_child(sun)


func _result(scene_path: String, enabled: bool, dump: bool, info: Dictionary) -> Dictionary:
	return {
		"pass": _failures.is_empty(),
		"scene": scene_path,
		"device": RenderingServer.get_video_adapter_name(),
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"nextgi_enabled": enabled,
		"dump_material_atlas": dump,
		"tier": info.get("tier", ""),
		"card_set_count": info.get("src_card_set_count", 0),
		"card_count": info.get("src_card_count", 0),
		"card_set_card_counts": info.get("src_card_set_card_counts", PackedInt32Array()),
		"capture_count": info.get("src_capture_count", 0),
		"capture_frames": info.get("src_capture_frames", 0),
		"max_captures_per_card": info.get("src_max_captures_per_card", 0),
		"shadow_batch_refusals": info.get("src_shadow_batch_refusals", 0),
		"atlas_memory": info.get("src_atlas_memory", 0),
		"atlas_used_texels": info.get("src_atlas_used_texels", 0),
		"card_sizes": info.get("src_card_sizes", PackedInt32Array()),
		"card_coverage": info.get("src_card_coverage", PackedFloat32Array()),
		"failures": _failures,
	}
