extends SceneTree

## Checks that an export pack keeps an embedded LightmapGIData's probe data (#244).
##
## Loads res://tests/export/embedded_lightmap.tscn from the project, mounts the pack over res://,
## loads the scene again (now the pack's converted copy) and compares the probe data:
##   <godot> --headless --path godotnext/testbed --script res://tests/export/check_lightmap_pack.gd -- <pack>
## run_export_check.sh runs it for every pack it exports. It prints one "GODOTNEXT_RESULT <json>"
## line and exits 0 when every check passes, 1 otherwise.

const SCENE := "res://tests/export/embedded_lightmap.tscn"

var _errors: Array[String] = []


func _initialize() -> void:
	var result := run(OS.get_cmdline_user_args())
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	quit(0 if result["pass"] else 1)


func run(args: PackedStringArray) -> Dictionary:
	var result := {"pass": false, "scene": SCENE}
	if args.size() != 1:
		_errors.append("Pass the pack path after --.")
		result["errors"] = _errors
		return result
	var pack := args[0]
	result["pack"] = pack

	_ok(not FileAccess.file_exists(SCENE + ".remap"), "The project must not carry the export remap before the pack is mounted.")
	var source := _probe_data(SCENE)
	_ok(not source.is_empty(), "The project scene must have embedded LightmapGIData probe data.")

	_ok(ProjectSettings.load_resource_pack(pack, true), "The pack must mount.")
	_ok(FileAccess.file_exists(SCENE + ".remap"), "The pack must remap the scene to its converted copy.")
	var exported := _probe_data(SCENE)
	_ok(not exported.is_empty(), "The exported scene must have embedded LightmapGIData probe data.")

	if not source.is_empty() and not exported.is_empty():
		for key: String in source:
			_ok(exported[key] == source[key], "The exported LightmapGIData %s must match the project's." % key)
		result["points"] = exported["points"].size()
		result["sh_colors"] = exported["sh"].size()
		result["tetrahedra"] = exported["tetrahedra"].size()
		result["bsp_entries"] = exported["bsp"].size()

	result["errors"] = _errors
	result["pass"] = _errors.is_empty()
	return result


func _probe_data(path: String) -> Dictionary:
	var packed := ResourceLoader.load(path, "PackedScene", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as PackedScene
	if packed == null:
		_errors.append("%s did not load." % path)
		return {}
	var root := packed.instantiate()
	var nodes := root.find_children("*", "LightmapGI", true, false)
	var data: LightmapGIData = null
	if nodes.size() == 1:
		data = (nodes[0] as LightmapGI).light_data
	else:
		_errors.append("%s must hold exactly one LightmapGI, found %d." % [path, nodes.size()])
	root.free()
	if data == null:
		return {}
	# The capture getters are not bound; probe_data carries what a re-save writes.
	var stored: Dictionary = data.get("probe_data")
	var out := {
		"bounds": stored.get("bounds"),
		"points": stored.get("points", PackedVector3Array()),
		"tetrahedra": stored.get("tetrahedra", PackedInt32Array()),
		"bsp": stored.get("bsp", PackedInt32Array()),
		"sh": stored.get("sh", PackedColorArray()),
		"interior": stored.get("interior"),
		"baked_exposure": stored.get("baked_exposure"),
		"lightprobe_hash": stored.get("lightprobe_hash"),
	}
	if out["points"].is_empty() or out["sh"].is_empty() or out["tetrahedra"].is_empty() or out["bsp"].is_empty():
		_errors.append("%s LightmapGIData probe data is empty." % path)
		return {}
	return out


func _ok(condition: bool, message: String) -> void:
	if not condition:
		_errors.append(message)
