extends SceneTree
## Compares renderer skinning of each source skeletal mesh with its re-imported glTF, on every
## exported row for one instance of each asset. check_ue_export.gd covers every clip and bone.
## Run rendered, forwarding the export's scene arguments (for example --variant=p3):
##
##   godot --path godotnext/testbed --rendering-driver vulkan \
##     --script res://tests/ue_export/check_ue_export_skinning.gd -- \
##     --export=/absolute/export --variant=p3

const SceneArgs := preload("res://scenes/scene_args.gd")
const MAX_FRAMES := 1000
const TOLERANCE_M := 0.0001

var _frames := 0


func _initialize() -> void:
	call_deferred("_run")


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames > MAX_FRAMES:
		_fail("the skinning comparison did not finish within %d frames" % MAX_FRAMES)
	return false


func _run() -> void:
	if DisplayServer.get_name() == "headless":
		_fail("the skinning comparison needs a real renderer")
		return
	var export_dir := SceneArgs.value("--export")
	if export_dir.is_empty() or not export_dir.is_absolute_path():
		_fail("pass the absolute export directory as --export=...")
		return
	var scene_data: Variant = JSON.parse_string(FileAccess.get_file_as_string(export_dir.path_join("scene.json")))
	if not scene_data is Dictionary:
		_fail("scene.json does not decode")
		return
	var packed := load(String(scene_data["scene"])) as PackedScene
	if packed == null:
		_fail("the source scene does not load")
		return
	var harness := get_root().get_node_or_null("Harness")
	if harness == null:
		harness = (load("res://harness/harness.gd") as GDScript).new()
		harness.name = "Harness"
		get_root().add_child(harness)
	var source := packed.instantiate()
	get_root().add_child(source)
	current_scene = source
	for frame in 2:
		await RenderingServer.frame_post_draw
	var assets: Array = scene_data["skeletal_meshes"]
	if assets.is_empty():
		_fail("the export has no skeletal meshes")
		return
	var failed := false
	for asset: Dictionary in assets:
		var instance := _first_instance(scene_data["instances"], String(asset["id"]))
		if instance.is_empty():
			_fail("asset %s has no instance" % asset["id"])
			return
		var source_mesh := source.get_node_or_null(NodePath(String(instance["node"]))) as MeshInstance3D
		if source_mesh == null:
			_fail("the source scene has no mesh at %s" % instance["node"])
			return
		var document := GLTFDocument.new()
		var state := GLTFState.new()
		if document.append_from_file(export_dir.path_join(String(asset["file"])), state) != OK:
			_fail("%s does not import" % asset["file"])
			return
		var imported := document.generate_scene(state, float(scene_data["fps"]))
		var meshes := imported.find_children("*", "MeshInstance3D", true, false)
		var players := imported.find_children("*", "AnimationPlayer", true, false)
		if meshes.size() != 1 or players.size() != 1:
			imported.free()
			_fail("%s needs one imported mesh and animation player" % asset["id"])
			return
		var imported_mesh := meshes[0] as MeshInstance3D
		var player := players[0] as AnimationPlayer
		get_root().add_child(imported)
		var clip := String(instance["clip"])
		if not player.has_animation(clip):
			_fail("%s has no clip %s" % [asset["id"], clip])
			return
		var worst := 0.0
		var compared := 0
		for row in int(scene_data["frames"]):
			player.play(clip)
			player.seek(float(row) / float(scene_data["fps"]), true)
			player.pause()
			harness.call("drive_frame", row)
			await RenderingServer.frame_post_draw
			var error := _compare_skinned(source_mesh, imported_mesh)
			worst = maxf(worst, error)
			compared += 1
		var ok := is_finite(worst) and worst <= TOLERANCE_M
		failed = failed or not ok
		print("GODOTNEXT_RESULT " + JSON.stringify({
			"test": "ue_export_skinning",
			"asset": asset["id"],
			"instance": instance["node"],
			"ok": ok,
			"rows": compared,
			"max_error_m": worst,
		}))
		imported.queue_free()
	harness.call("drive_frame", -1)
	quit(1 if failed else 0)


func _first_instance(p_instances: Array, p_asset: String) -> Dictionary:
	for instance: Dictionary in p_instances:
		if instance["kind"] == "skeletal" and String(instance["skeletal"]) == p_asset:
			return instance
	return {}


func _compare_skinned(p_source: MeshInstance3D, p_imported: MeshInstance3D) -> float:
	var source := p_source.bake_mesh_from_current_skeleton_pose()
	var imported := p_imported.bake_mesh_from_current_skeleton_pose()
	if source == null or imported == null or source.get_surface_count() != imported.get_surface_count():
		return INF
	var worst := 0.0
	for surface in source.get_surface_count():
		var source_rest := p_source.mesh.surface_get_arrays(surface)
		var imported_rest := p_imported.mesh.surface_get_arrays(surface)
		var source_baked := source.surface_get_arrays(surface)
		var imported_baked := imported.surface_get_arrays(surface)
		var source_corners := _corners(source_rest)
		var imported_corners := _corners(imported_rest)
		if source_corners.size() != imported_corners.size():
			return INF
		var source_vertices: PackedVector3Array = source_baked[Mesh.ARRAY_VERTEX]
		var imported_vertices: PackedVector3Array = imported_baked[Mesh.ARRAY_VERTEX]
		for corner in source_corners.size():
			var a := source_corners[corner]
			var b := imported_corners[corner]
			if a >= source_vertices.size() or b >= imported_vertices.size():
				return INF
			worst = maxf(worst, source_vertices[a].distance_to(imported_vertices[b]))
	return worst


func _corners(p_arrays: Array) -> PackedInt32Array:
	if p_arrays[Mesh.ARRAY_INDEX] != null:
		return p_arrays[Mesh.ARRAY_INDEX]
	var corners := PackedInt32Array()
	for index in (p_arrays[Mesh.ARRAY_VERTEX] as PackedVector3Array).size():
		corners.append(index)
	return corners


func _fail(p_message: String) -> void:
	printerr("check_ue_export_skinning.gd: " + p_message)
	quit(1)
