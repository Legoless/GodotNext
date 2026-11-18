extends Node

## The material hit groups on a smoke scene (#130).
##
## Loads the scene named by --scene=res://... (the P0 testbed's Color Box Room or Concave Courtyard,
## the PL phase's smoke scenes), turns rendering/global_illumination/nextgi/debug/hit_material_check on
## and renders until the hit groups' check has run, or for --frames=N frames (default 600). Prints one
## "GODOTNEXT_RESULT <json>" line with the tier, the hit groups' availability and their counters. It
## passes when the hardware tier is active and the check ran on its material groups; with
## --expect=no-pipeline (Metal until #172) it passes when the hit groups report NO_RAYTRACING_PIPELINE, and
## with --expect=not-hwrt (the software tier) when they report NOT_HWRT.
## With rendering/global_illumination/nextgi/hardware_ray_tracing_mode (#286) at Inline, --expect=inline passes
## when the hardware tier is active, the hit groups and every feature report HWRT_MODE_INLINE and no stream
## exists; at Simplified Material, --expect=simplified-material passes when the check ran with no material
## group, every hit coming from the simplified hit shader. Without either, the mode must be FULL.
## The match shares on authored content are reported, not checked (hit_groups_check.tscn checks them).

const SETTING := "rendering/global_illumination/nextgi/debug/hit_material_check"

var _scene_path := "res://scenes/color_box_room/cbr_a.tscn"
var _frames := 600
var _expect_pipeline := true
var _expect_tier := "HWRT"
var _expect_mode := "FULL"


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
		elif arg == "--expect=inline":
			_expect_pipeline = false
			_expect_mode = "INLINE"
		elif arg == "--expect=simplified-material":
			_expect_mode = "SIMPLIFIED_MATERIAL"
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
		"hardware_ray_tracing_mode": String(tier.get("hardware_ray_tracing_mode", "")),
		"hit_groups_by_consumer": tier.get("hit_groups_by_consumer", {}),
	}
	for key in counters.keys():
		if String(key).begins_with("hit_") or String(key).begins_with("rt_attribute"):
			result[key] = counters[key]
	var failures := PackedStringArray()
	if not p_loaded:
		failures.append("the scene did not load")
	if result["tier"] != _expect_tier:
		failures.append("the tier is %s, not %s" % [result["tier"], _expect_tier])
	if result["hardware_ray_tracing_mode"] != _expect_mode:
		failures.append("the Hardware Ray Tracing Mode is %s, not %s" % [result["hardware_ray_tracing_mode"], _expect_mode])
	var by_consumer: Dictionary = result["hit_groups_by_consumer"]
	if _expect_mode == "INLINE":
		if result["hit_groups"] != "HWRT_MODE_INLINE":
			failures.append("the hit groups report %s, not HWRT_MODE_INLINE" % result["hit_groups"])
		for feature in ["hit_lighting", "translucency", "far_field", "material_retrace", "check"]:
			if String(by_consumer.get(feature, "")) != "HWRT_MODE_INLINE":
				failures.append("%s reports %s, not HWRT_MODE_INLINE" % [feature, by_consumer.get(feature, "")])
		if int(counters.get("rt_attribute_bytes", 0)) != 0 or counters.has("hit_check_runs"):
			failures.append("a hit pipeline or an attribute stream exists in Inline")
	elif _expect_mode == "SIMPLIFIED_MATERIAL":
		if result["hit_groups"] != "AVAILABLE":
			failures.append("the simplified hit pipeline is not available (%s)" % result["hit_groups"])
		for feature in ["hit_lighting", "translucency", "far_field", "material_retrace"]:
			if String(by_consumer.get(feature, "")) != "HWRT_MODE_SIMPLIFIED_MATERIAL":
				failures.append("%s reports %s, not HWRT_MODE_SIMPLIFIED_MATERIAL" % [feature, by_consumer.get(feature, "")])
		if int(counters.get("hit_check_runs", 0)) == 0:
			failures.append("the check did not run")
		if int(counters.get("hit_material_groups", 1)) != 0 or int(counters.get("hit_shaders", -1)) != 0 or int(counters.get("hit_groups_linked", -1)) != 0:
			failures.append("a material hit group exists in Simplified Material")
		if int(counters.get("rt_attribute_bytes", 0)) != 0:
			failures.append("an attribute stream exists in Simplified Material")
		if int(counters.get("hit_check_fallback", -1)) != int(counters.get("hit_check_hits", 0)) or int(counters.get("hit_check_hits", 0)) == 0:
			failures.append("not every hit comes from the simplified hit shader")
	elif _expect_pipeline:
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
