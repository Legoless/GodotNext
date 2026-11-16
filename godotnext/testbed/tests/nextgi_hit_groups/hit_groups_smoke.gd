extends Node

## The material hit groups on a smoke scene (#130).
##
## Loads the scene named by --scene=res://... (the P0 testbed's Color Box Room or Concave Courtyard,
## the PL phase's smoke scenes), turns rendering/global_illumination/nextgi/debug/hit_material_check on
## and renders until the hit groups' check has run, or for --frames=N frames (default 600). Prints one
## "GODOTNEXT_RESULT <json>" line with the tier, the hit groups' availability and their counters. It
## passes when the hardware tier is active and the check ran on its material groups; with
## --expect=no-pipeline (D3D12 until #121) it passes when the hit groups report NO_RAYTRACING_PIPELINE, and
## with --expect=not-hwrt (the software tier) when they report NOT_HWRT.
## The match shares on authored content are reported, not checked (hit_groups_check.tscn checks them).

const SETTING := "rendering/global_illumination/nextgi/debug/hit_material_check"

var _scene_path := "res://scenes/color_box_room/cbr_a.tscn"
var _frames := 600
var _expect_pipeline := true
var _expect_tier := "HWRT"


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--scene="):
			_scene_path = arg.trim_prefix("--scene=")
		elif arg.begins_with("--frames="):
			_frames = int(arg.trim_prefix("--frames="))
		elif arg == "--expect=no-pipeline":
			_expect_pipeline = false
		elif arg == "--expect=not-hwrt":
			_expect_pipeline = false
			_expect_tier = "SWRT"
	var packed: PackedScene = load(_scene_path)
	if packed != null:
		var scene := packed.instantiate()
		add_child(scene)
		for node in scene.find_children("*", "WorldEnvironment", true, false):
			var world := node as WorldEnvironment
			if world.environment != null:
				var environment := world.environment.duplicate()
				environment.nextgi_enabled = true
				world.environment = environment
	ProjectSettings.set_setting(SETTING, true)
	_run(packed != null)


func _run(p_loaded: bool) -> void:
	var counters := {}
	for i in _frames:
		await RenderingServer.frame_post_draw
		counters = RenderingServer.nextgi_get_counters()
		if _expect_pipeline and int(counters.get("hit_check_runs", 0)) > 0:
			break
	var tier: Dictionary = RenderingServer.nextgi_get_tier_info()
	var result := {
		"scene": _scene_path,
		"tier": String(tier.get("tier", "")),
		"hit_groups": String(tier.get("hit_groups", "")),
	}
	for key in counters.keys():
		if String(key).begins_with("hit_") or String(key).begins_with("rt_attribute"):
			result[key] = counters[key]
	var failures := PackedStringArray()
	if not p_loaded:
		failures.append("the scene did not load")
	if result["tier"] != _expect_tier:
		failures.append("the tier is %s, not %s" % [result["tier"], _expect_tier])
	if _expect_pipeline:
		if result["hit_groups"] != "AVAILABLE":
			failures.append("the hit groups are not available (%s)" % result["hit_groups"])
		if int(counters.get("hit_check_runs", 0)) == 0:
			failures.append("the check did not run")
		if int(counters.get("hit_groups_linked", 0)) == 0:
			failures.append("no material group was linked")
		var shaded := int(counters.get("hit_check_hits", 0)) - int(counters.get("hit_check_fallback", 0))
		result["share_all"] = float(counters.get("hit_check_all", 0)) / float(max(shaded, 1))
	else:
		var expected := "NO_RAYTRACING_PIPELINE" if _expect_tier == "HWRT" else "NOT_HWRT"
		if result["hit_groups"] != expected:
			failures.append("the hit groups report %s, not %s" % [result["hit_groups"], expected])
	ProjectSettings.set_setting(SETTING, false)
	result["pass"] = failures.is_empty()
	result["failures"] = failures
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if failures.is_empty() else 1)
