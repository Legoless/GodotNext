extends SceneTree

## Checks that an export pack keeps the legacy VoxelGI scene's baked GI data (#242).
##
## Loads res://scenes/legacy/legacy_voxelgi.tscn from the project, mounts the pack over res://,
## loads the scene again (now the pack's converted copy) and compares the VoxelGIData:
##   <godot> --headless --path godotnext/testbed --script res://tests/export/check_voxelgi_pack.gd -- <pack>
## run_export_check.sh runs it for every pack it exports. It prints one "GODOTNEXT_RESULT <json>"
## line and exits 0 when every check passes, 1 otherwise.

const SCENE := "res://scenes/legacy/legacy_voxelgi.tscn"

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
	var source := _gi_data(SCENE)
	_ok(not source.is_empty(), "The project scene must have baked VoxelGIData.")

	_ok(ProjectSettings.load_resource_pack(pack, true), "The pack must mount.")
	_ok(FileAccess.file_exists(SCENE + ".remap"), "The pack must remap the scene to its converted copy.")
	var exported := _gi_data(SCENE)
	_ok(not exported.is_empty(), "The exported scene must have baked VoxelGIData.")

	if not source.is_empty() and not exported.is_empty():
		for key: String in source:
			_ok(exported[key] == source[key], "The exported VoxelGIData %s must match the project's." % key)
		result["octree_size"] = str(exported["octree_size"])
		result["octree_cells_bytes"] = exported["octree_cells"].size()
		result["data_cells_bytes"] = exported["data_cells"].size()
		result["distance_field_bytes"] = exported["distance_field"].size()

	result["errors"] = _errors
	result["pass"] = _errors.is_empty()
	return result


func _gi_data(path: String) -> Dictionary:
	var packed := ResourceLoader.load(path, "PackedScene", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as PackedScene
	if packed == null:
		_errors.append("%s did not load." % path)
		return {}
	var root := packed.instantiate()
	var nodes := root.find_children("*", "VoxelGI", true, false)
	var data: VoxelGIData = null
	if nodes.size() == 1:
		data = (nodes[0] as VoxelGI).data
	else:
		_errors.append("%s must hold exactly one VoxelGI, found %d." % [path, nodes.size()])
	root.free()
	if data == null:
		return {}
	# get_distance_field() is not bound; _data carries the distance field as a Lum8 PNG.
	var stored: Dictionary = data.get("_data")
	var distance_field := PackedByteArray()
	if stored.has("octree_df_png"):
		var image := Image.new()
		if image.load_png_from_buffer(stored["octree_df_png"]) == OK:
			distance_field = image.get_data()
	var out := {
		"bounds": data.get_bounds(),
		"octree_size": data.get_octree_size(),
		"to_cell_xform": data.get_to_cell_xform(),
		"octree_cells": data.get_octree_cells(),
		"data_cells": data.get_data_cells(),
		"distance_field": distance_field,
		"level_counts": data.get_level_counts(),
	}
	if out["octree_size"] == Vector3() or out["octree_cells"].is_empty() or out["data_cells"].is_empty() or out["distance_field"].is_empty():
		_errors.append("%s VoxelGIData is empty." % path)
		return {}
	return out


func _ok(condition: bool, message: String) -> void:
	if not condition:
		_errors.append(message)
