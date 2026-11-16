extends Node
## Native Nanite ray tracing on a content scene (#156; plan §21.4 N13, §22.3). By default the PN
## phase smoke scene, Micro Detail, which is generated: --headless --script
## res://generators/dense_city/dense_city_generator.gd -- --seed=1 --variant=micro_detail, then
## --import. NextGI runs on the hardware tier with the scene's environment switched on, and
## rendering/nextgeo/ray_tracing_mode is Native (EXPERIMENTAL) for --frames frames. Pass
## --gpu-validation for the PR basics; the run fails when the scene does not load, the tier is not
## HWRT, no instance traced a native set, a frame broke the N7 or MN4 cap, a traced set had fewer
## triangles than its mesh's proxy, or the AS line missed the native BLASes. User arguments
## (after --): --scene=res://... and --frames=N (default 400).
## Run it with NextGI on, for example through an override.cfg holding
## [rendering] global_illumination/nextgi/enabled=true.

const MODE_SETTING := "rendering/nextgeo/ray_tracing_mode"
const DEFAULT_SCENE := "res://generated/dense_city/micro_detail.tscn"

var _failures := PackedStringArray()


func _ready() -> void:
	var result := await run()
	ProjectSettings.set_setting(MODE_SETTING, 0)
	result["failures"] = _failures
	result["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if _failures.is_empty() else 1)


func _check(p_condition: bool, p_what: String) -> void:
	if not p_condition:
		_failures.append(p_what)


func _enable_nextgi(p_root: Node) -> int:
	var environments := 0
	var stack: Array[Node] = [p_root]
	while not stack.is_empty():
		var node: Node = stack.pop_back()
		if node is WorldEnvironment and (node as WorldEnvironment).environment != null:
			var env: Environment = (node as WorldEnvironment).environment.duplicate()
			env.nextgi_enabled = true
			(node as WorldEnvironment).environment = env
			environments += 1
		for child in node.get_children():
			stack.append(child)
	return environments


func run() -> Dictionary:
	var scene_path := DEFAULT_SCENE
	var frames := 400
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--scene="):
			scene_path = arg.trim_prefix("--scene=")
		elif arg.begins_with("--frames="):
			frames = maxi(int(arg.trim_prefix("--frames=")), 10)
	var result := {"scene": scene_path, "frames": frames}
	var enabled: bool = ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false)
	_check(enabled, "nextgi/enabled must be true (override.cfg)")
	var packed := load(scene_path) as PackedScene
	_check(packed != null, "%s loads" % scene_path)
	if not enabled or packed == null:
		return result
	var scene := packed.instantiate()
	add_child(scene)
	result["environments"] = _enable_nextgi(scene)
	ProjectSettings.set_setting(MODE_SETTING, 1)

	var info: Dictionary = {}
	var native_frames := 0
	var max_native_instances := 0
	var max_proxy_instances := 0
	var max_buckets := 0
	var max_triangles := 0
	var max_build := 0
	var cap_breaks := 0
	var coarser_frames := 0
	var as_line_breaks := 0
	for _i in range(frames):
		await RenderingServer.frame_post_draw
		info = RenderingServer.nextgi_get_tier_info()
		if String(info.get("nextgeo_ray_tracing_mode", "")) != "native":
			continue
		var native_instances := int(info.get("rt_native_instances", 0)) + int(info.get("rt_native_previous_instances", 0))
		native_frames += 1 if native_instances > 0 else 0
		max_native_instances = maxi(max_native_instances, native_instances)
		max_proxy_instances = maxi(max_proxy_instances, int(info.get("rt_native_proxy_instances", 0)))
		max_buckets = maxi(max_buckets, int(info.get("rt_native_buckets", 0)))
		max_triangles = maxi(max_triangles, int(info.get("rt_native_triangles", 0)))
		var built := int(info.get("rt_native_build_triangles", 0))
		max_build = maxi(max_build, built)
		if built > int(info.get("rt_native_rebuild_cap", 0)) or int(info.get("rt_native_build_bytes", 0)) > int(info.get("rt_native_memory_cap_bytes", 0)):
			cap_breaks += 1
		if int(info.get("rt_native_coarser_sets", -1)) != 0:
			coarser_frames += 1
		# The AS line (line 1b of this frame) holds the proxies and the native BLASes.
		if int(info.get("rt_instance_count", 0)) > 0 and int(info.get("rt_as_bytes", -1)) != int(info.get("rt_native_as_proxy_bytes", -1)) + int(info.get("rt_native_blas_bytes", -1)):
			as_line_breaks += 1
	_check(String(info.get("tier", "")) == "HWRT", "expected tier == 'HWRT', got '%s'" % String(info.get("tier", "")))
	_check(String(info.get("nextgeo_ray_tracing_mode", "")) == "native", "the tier info says native")
	_check(native_frames > 0, "an instance traced a native set")
	_check(cap_breaks == 0, "every frame kept the N7 and MN4 caps (%d frames broke one)" % cap_breaks)
	_check(coarser_frames == 0, "no traced set had fewer triangles than its proxy (%d frames had one)" % coarser_frames)
	_check(as_line_breaks == 0, "the AS line held the proxies and the native BLASes (%d frames did not)" % as_line_breaks)
	result.merge({
		"tier": String(info.get("tier", "")),
		"rt_instance_count": int(info.get("rt_instance_count", 0)),
		"native_frames": native_frames,
		"max_native_instances": max_native_instances,
		"max_proxy_instances": max_proxy_instances,
		"max_buckets": max_buckets,
		"max_native_triangles": max_triangles,
		"max_build_triangles": max_build,
		"coarse_buckets": int(info.get("rt_native_coarse_buckets", 0)),
		"memory_refusals": int(info.get("rt_native_memory_refusals", 0)),
		"cap_releases": int(info.get("rt_native_cap_releases", 0)),
		"shared_buckets": int(info.get("rt_native_shared_buckets", 0)),
		"traced_triangles_min": int(info.get("rt_native_traced_triangles_min", 0)),
		"traced_triangles_max": int(info.get("rt_native_traced_triangles_max", 0)),
		"proxy_card_triangles": int(info.get("rt_native_proxy_card_triangles", 0)),
		"chosen_card_triangles": int(info.get("rt_native_chosen_card_triangles", 0)),
		"no_card_triangles": int(info.get("rt_native_no_card_triangles", 0)),
	})

	ProjectSettings.set_setting(MODE_SETTING, 0)
	remove_child(scene)
	scene.free()
	for _i in range(4):
		await RenderingServer.frame_post_draw
	return result
