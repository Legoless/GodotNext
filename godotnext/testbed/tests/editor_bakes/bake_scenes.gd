@tool
extends Node

## Editor-hint bake for the testbed scenes that carry a LightmapGI or
## VoxelGI node (#238). The generators cannot bake: LightmapGI.bake() is
## not bound to scripts and a --script run has no editor singletons, so
## this scene runs inside --editor and bakes through NextGeoTestBridge
## (the #67 path) for lightmaps and the bound VoxelGI.bake() for voxels.
##
## Launch with GODOTNEXT_BAKE_SCENES=1, --editor, and this scene:
##   GODOTNEXT_BACKGROUND=1 GODOTNEXT_BAKE_SCENES=1 <godot> --path godotnext/testbed \
##       --rendering-driver metal --editor res://tests/editor_bakes/bake_scenes.tscn
## Without that variable this node does nothing. Set
## GODOTNEXT_BAKE_SCENES_FILTER to a substring to bake only matching
## scenes. Each baked scene is packed and saved over its own .tscn, the
## way saving in the editor after a GUI bake would leave it.
##
## Lightmap targets first get UV2 the way the editor GUI flow does it:
## PrimitiveMesh resources get add_uv2 checked, ArrayMesh resources
## without UV2 get lightmap_unwrap() at the editor's default texel size.
## Bake settings are never touched; they are whatever the generators
## recorded in each scene.

const _TARGETS: Array[Dictionary] = [
	{"scene": "res://scenes/color_box_room/cbr_b_lightmap.tscn", "kind": "lightmap"},
	{"scene": "res://scenes/legacy/legacy_lightmap.tscn", "kind": "lightmap"},
	{"scene": "res://scenes/legacy/legacy_voxelgi.tscn", "kind": "voxel"},
	{"scene": "res://scenes/feature_parity_room/feature_parity_room.tscn", "kind": "lightmap"},
]

static var _started_once := false

var _queue: Array[Dictionary] = []
var _index := 0
var _wait := 0
var _phase := "setup"
var _baking := false
var _root: Node = null
var _failures := 0


func _ready() -> void:
	if not Engine.is_editor_hint():
		return
	if OS.get_environment("GODOTNEXT_BAKE_SCENES") != "1":
		return
	# The editor can instance this scene twice (session restore and the command-line scene).
	if _started_once:
		return
	_started_once = true
	var filter := OS.get_environment("GODOTNEXT_BAKE_SCENES_FILTER")
	for target in _TARGETS:
		if filter.is_empty() or String(target["scene"]).contains(filter):
			_queue.append(target)
	if _queue.is_empty():
		print("editor bakes: no scene matches filter '" + filter + "'")
		get_tree().quit(1)
		return
	set_process(true)
	_begin_target()


func _process(_delta: float) -> void:
	# A lightmap bake reimports its EXR textures through the editor file
	# system, and that pumps the main loop. Ignore those frames so a
	# second bake cannot start while one is running.
	if _baking:
		return
	_wait += 1
	if _phase == "setup":
		# A few rendered frames so mesh and material RIDs exist, and the
		# editor file system must be idle before a bake reimports files.
		if _wait < 8:
			return
		var filesystem := EditorInterface.get_resource_filesystem()
		if filesystem == null or filesystem.is_scanning() or filesystem.is_importing():
			return
		_baking = true
		_bake_current()
		_baking = false
		_phase = "settle"
		_wait = 0
	elif _phase == "settle":
		# Let any file-system scan the bake triggered finish before the
		# scene save reads resource paths.
		if _wait < 4:
			return
		var filesystem := EditorInterface.get_resource_filesystem()
		if filesystem != null and (filesystem.is_scanning() or filesystem.is_importing()):
			return
		_save_current()
		_advance()


func _begin_target() -> void:
	var target := _queue[_index]
	var scene_path := String(target["scene"])
	var loaded := ResourceLoader.load(scene_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	var packed := loaded as PackedScene
	if packed == null:
		_failures += 1
		print("FAIL: cannot load " + scene_path)
		_advance()
		return
	_root = packed.instantiate()
	add_child(_root)
	if String(target["kind"]) == "lightmap":
		_ensure_uv2(_root)
	_phase = "setup"
	_wait = 0
	print("EDITOR_BAKE_BEGIN %s" % scene_path)


func _ensure_uv2(root: Node) -> void:
	var done: Dictionary = {}
	for node in root.find_children("*", "MeshInstance3D", true, false):
		var instance := node as MeshInstance3D
		var mesh := instance.mesh
		if mesh == null or done.has(mesh.get_instance_id()):
			continue
		done[mesh.get_instance_id()] = true
		if mesh is PrimitiveMesh:
			(mesh as PrimitiveMesh).add_uv2 = true
		elif mesh is ArrayMesh:
			var array_mesh := mesh as ArrayMesh
			var has_uv2 := array_mesh.get_surface_count() > 0
			for surface in array_mesh.get_surface_count():
				var arrays := array_mesh.surface_get_arrays(surface)
				if arrays[Mesh.ARRAY_TEX_UV2] == null:
					has_uv2 = false
			if not has_uv2:
				var err := array_mesh.lightmap_unwrap(instance.global_transform, 0.05)
				if err != OK:
					_failures += 1
					print("FAIL: lightmap_unwrap failed (%d) for %s" % [err, instance.name])


func _bake_current() -> void:
	var target := _queue[_index]
	var scene_path := String(target["scene"])
	if String(target["kind"]) == "lightmap":
		var lightmaps := _root.find_children("*", "LightmapGI", true, false)
		if lightmaps.size() != 1:
			_failures += 1
			print("FAIL: %s has %d LightmapGI nodes, expected 1" % [scene_path, lightmaps.size()])
			return
		var bake_path := scene_path.get_basename() + ".lmbake"
		var bake_error: int = NextGeoTestBridge.bake_lightmap(lightmaps[0], _root, bake_path)
		print("EDITOR_BAKE_RESULT %s BakeError %d" % [scene_path, bake_error])
		if bake_error != LightmapGI.BAKE_ERROR_OK:
			_failures += 1
	else:
		var voxels := _root.find_children("*", "VoxelGI", true, false)
		if voxels.size() != 1:
			_failures += 1
			print("FAIL: %s has %d VoxelGI nodes, expected 1" % [scene_path, voxels.size()])
			return
		var voxel := voxels[0] as VoxelGI
		voxel.bake(_root)
		if voxel.data == null:
			_failures += 1
			print("EDITOR_BAKE_RESULT %s VoxelGI data missing after bake" % scene_path)
		else:
			print("EDITOR_BAKE_RESULT %s VoxelGI data baked" % scene_path)


func _save_current() -> void:
	var scene_path := String(_queue[_index]["scene"])
	var packed := PackedScene.new()
	var pack_err := packed.pack(_root)
	if pack_err != OK:
		_failures += 1
		print("FAIL: pack failed (%d) for %s" % [pack_err, scene_path])
		return
	var save_err := ResourceSaver.save(packed, scene_path)
	if save_err != OK:
		_failures += 1
		print("FAIL: save failed (%d) for %s" % [save_err, scene_path])
		return
	print("EDITOR_BAKE_SAVED %s" % scene_path)


func _advance() -> void:
	if _root != null:
		remove_child(_root)
		_root.free()
		_root = null
	_index += 1
	if _index >= _queue.size():
		print("editor bakes: %d failure(s)" % _failures)
		get_tree().quit(_failures)
		return
	_begin_target()
