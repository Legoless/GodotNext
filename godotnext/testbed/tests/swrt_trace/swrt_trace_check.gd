extends Node3D
## #186: the software tracer must march the kitbash and dispatch shadow rays.
## nextgi/enabled and hardware_ray_tracing=Off are set by this folder's override.cfg
## before launch. The tier stays disabled while the project setting is off.


var _frames := 0


func _ready() -> void:
	var kitbash: PackedScene = load("res://scenes/swrt_kitbash/swrt_kitbash.tscn")
	var root := kitbash.instantiate()
	var world := root.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		world.environment.nextgi_enabled = true
	add_child(root)
	var camera := Camera3D.new()
	add_child(camera)
	camera.position = Vector3(0.0, 2.0, 8.0)
	camera.look_at(Vector3(0.0, 1.0, 0.0))
	camera.current = true


func _write_manifest() -> void:
	var args := OS.get_cmdline_user_args()
	var out_dir := ""
	for index in args.size():
		var arg := str(args[index])
		if arg.begins_with("--out="):
			out_dir = arg.substr("--out=".length())
		elif arg == "--out" and index + 1 < args.size():
			out_dir = str(args[index + 1])
	if out_dir.is_empty():
		return
	DirAccess.make_dir_recursive_absolute(out_dir)
	var file := FileAccess.open(out_dir.path_join("manifest.json"), FileAccess.WRITE)
	if file == null:
		return
	file.store_string("{\n\t\"source\": \"swrt_trace_check\"\n}\n")


func _fail(reason: String) -> void:
	push_error(reason)
	print("SWRT_TRACE_FAIL %s" % reason)
	_write_manifest()
	get_tree().quit(1)


func _process(_delta: float) -> void:
	_frames += 1
	if _frames < 240:
		return
	var counters: Dictionary = RenderingServer.nextgi_get_counters()
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var required: PackedStringArray = PackedStringArray([
		"swrt_trace_steps",
		"swrt_trace_hits",
		"swrt_trace_misses",
		"swrt_shadow_rays",
		"swrt_mode",
	])
	for key in required:
		if not counters.has(key):
			_fail("missing counter %s" % key)
			return
	if not info.has("tier"):
		_fail("missing tier")
		return
	var steps := int(counters["swrt_trace_steps"])
	var hits := int(counters["swrt_trace_hits"])
	var misses := int(counters["swrt_trace_misses"])
	var shadow_rays := int(counters["swrt_shadow_rays"])
	var mode := int(counters["swrt_mode"])
	var tier := str(info["tier"])
	print("SWRT_TRACE steps=%d hits=%d misses=%d shadow_rays=%d mode=%d tier=%s" % [steps, hits, misses, shadow_rays, mode, tier])
	if steps <= 0:
		_fail("trace steps are 0")
		return
	if hits <= 0:
		_fail("trace hits are 0")
		return
	if shadow_rays <= 0:
		_fail("shadow rays are 0")
		return
	_write_manifest()
	get_tree().quit(0)
