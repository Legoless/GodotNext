extends Node3D
## #183: Medium irradiance-field gather dispatches no screen-probe or reflection rays.
## The folder override forces the software tier, Medium quality and the default gather.


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
	file.store_string("{\n\t\"source\": \"lumen_lite_check\"\n}\n")


func _fail(reason: String) -> void:
	push_error(reason)
	print("LITE_DISPATCH_FAIL %s" % reason)
	_write_manifest()
	get_tree().quit(1)


func _process(_delta: float) -> void:
	_frames += 1
	if _frames < 240:
		return
	var counters: Dictionary = RenderingServer.nextgi_get_counters()
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var required: PackedStringArray = PackedStringArray([
		"lite_gather_dispatches",
		"lite_reflection_dispatches",
		"lite_probe_traces",
	])
	for key in required:
		if not counters.has(key):
			_fail("missing counter %s" % key)
			return
	if str(info.get("tier", "")) != "SWRT":
		_fail("tier is %s" % str(info.get("tier", "")))
		return
	var gather_rays := int(counters["lite_gather_dispatches"])
	var reflection_rays := int(counters["lite_reflection_dispatches"])
	var probe_traces := int(counters["lite_probe_traces"])
	print("LITE_DISPATCH gather_rays=%d reflection_rays=%d probe_traces=%d" % [gather_rays, reflection_rays, probe_traces])
	if gather_rays != 0 or reflection_rays != 0:
		_fail("lite dispatched screen or reflection rays")
		return
	if probe_traces <= 0:
		_fail("lite probe traces are 0")
		return
	_write_manifest()
	get_tree().quit(0)
