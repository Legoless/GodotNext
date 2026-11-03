extends SceneTree

## GPU round trip for a virtualized glTF and OBJ against a retain_authored plain import.
## Surface and trimesh failures exit non-zero. The FLIP line is reported and is not a gate.
## The lightmap bake is bake_editor.gd. LightmapperRD reads editor singletons this --script run does not create.

const _GLTF_VG := "res://tests/roundtrip/room_vg.gltf"
const _GLTF_PLAIN := "res://tests/roundtrip/room_plain.gltf"
const _GLTF_SINGLE := "res://tests/roundtrip/room_single.gltf"
const _OBJ_VG := "res://tests/roundtrip/prop_vg.obj"
const _OBJ_PLAIN := "res://tests/roundtrip/prop_plain.obj"

var _failures := 0
var _phase := 0
var _wait := 0
var _started := false
var _viewport: SubViewport
var _world: Node3D
var _lightmap: LightmapGI
var _captured: Image
var _pair_name := ""


func _initialize() -> void:
	_check_pair(_GLTF_VG, _GLTF_PLAIN, true)
	_check_pair(_OBJ_VG, _OBJ_PLAIN, false)
	_check_single_mesh()
	if _failures != 0:
		print("roundtrip: %d failure(s) before capture" % _failures)
		quit(_failures)
		return


func _process(_delta: float) -> bool:
	# SceneTree calls _initialize before the root enters the tree. look_at needs the root inside the tree.
	if not _started:
		_started = true
		_begin_pair("gltf", _GLTF_VG)
		return false
	if _viewport == null:
		quit(_failures)
		return true
	_wait += 1
	if _wait < 5:
		return false
	var image := _viewport.get_texture().get_image()
	if image == null or image.is_empty():
		_fail("capture produced an empty image for " + _pair_name)
		_advance_after_capture(null)
		return _phase < 0
	if _captured == null:
		_captured = image
		_wait = 0
		if _pair_name == "gltf":
			_begin_pair("gltf-plain", _GLTF_PLAIN)
		else:
			_begin_pair("obj-plain", _OBJ_PLAIN)
		return false
	_report_flip(_pair_name.trim_suffix("-plain"), _captured, image)
	_captured = null
	_advance_after_capture(image)
	if _phase < 0:
		print("roundtrip: %d failure(s)" % _failures)
		quit(_failures)
		return true
	return false


func _advance_after_capture(_image: Image) -> void:
	if _pair_name == "gltf-plain":
		_wait = 0
		_begin_pair("obj", _OBJ_VG)
	else:
		_phase = -1
		if _viewport != null:
			_viewport.free()
			_viewport = null


func _check_pair(vg_path: String, plain_path: String, scene_import: bool) -> void:
	var vg := _load_mesh(vg_path, scene_import)
	var plain := _load_mesh(plain_path, scene_import)
	if vg == null or plain == null:
		return
	_check_mesh(vg_path, vg, true)
	_check_mesh(plain_path, plain, false)
	_save_and_reload(vg_path, vg)


func _check_single_mesh() -> void:
	var loaded := ResourceLoader.load(_GLTF_SINGLE, "", ResourceLoader.CACHE_MODE_IGNORE)
	var mesh := loaded as ArrayMesh
	if mesh == null:
		_fail(_GLTF_SINGLE + " did not import as an ArrayMesh. Its .import must use importer=\"ArrayMesh\".")
		return
	_check_mesh(_GLTF_SINGLE, mesh, true)
	var cfg := ConfigFile.new()
	if cfg.load(_GLTF_SINGLE + ".import") != OK:
		_fail("missing .import for " + _GLTF_SINGLE)
		return
	var dest := str(cfg.get_value("remap", "path", ""))
	var manifest_path := dest.get_basename() + ".ngm_manifest.json"
	if not FileAccess.file_exists(manifest_path):
		_fail("single-mesh import wrote no manifest at " + manifest_path)
		return
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(manifest_path))
	if typeof(parsed) != TYPE_DICTIONARY:
		_fail("single-mesh manifest is not an object")
		return
	var entries: Array = parsed["entries"]
	if entries.size() != 1:
		_fail("single-mesh import produced %d .ngm entries" % entries.size())
		return
	var ngm := str(entries[0]["ngm"])
	if not FileAccess.file_exists(ngm):
		_fail("single-mesh .ngm is missing: " + ngm)
		return
	var folder := dest.get_base_dir()
	var prefix := dest.get_file().get_basename()
	var dir := DirAccess.open(folder)
	if dir == null:
		_fail("could not list " + folder)
		return
	dir.list_dir_begin()
	var file_name := dir.get_next()
	while file_name != "":
		if file_name.begins_with(prefix) and file_name.ends_with(".ngm") and file_name != ngm.get_file():
			_fail("orphaned .ngm next to the single mesh: " + file_name)
		file_name = dir.get_next()
	dir.list_dir_end()


func _load_mesh(path: String, scene_import: bool) -> ArrayMesh:
	var loaded := ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
	if loaded == null:
		_fail("failed to load " + path + " (run --import first)")
		return null
	if scene_import:
		var packed := loaded as PackedScene
		if packed == null:
			_fail(path + " did not import as a PackedScene")
			return null
		var root := packed.instantiate()
		var matches := root.find_children("*", "MeshInstance3D", true, false)
		var instance := matches[0] as MeshInstance3D if matches.size() > 0 else null
		var mesh: ArrayMesh = null
		if instance != null:
			mesh = instance.mesh as ArrayMesh
		root.free()
		if mesh == null:
			_fail(path + " has no ArrayMesh")
		return mesh
	var mesh := loaded as ArrayMesh
	if mesh == null:
		_fail(path + " did not import as an ArrayMesh")
	return mesh


func _check_mesh(path: String, mesh: ArrayMesh, expect_virtual: bool) -> void:
	if mesh.get_surface_count() == 0:
		_fail(path + " has no surfaces")
		return
	var arrays: Array = mesh.surface_get_arrays(0)
	var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	if vertices.is_empty():
		_fail(path + " surface 0 has no vertices")
	var shape := mesh.create_trimesh_shape()
	if shape == null or shape.get_faces().is_empty():
		_fail(path + " trimesh is empty")
	var virtual := mesh.virtual_geometry != null
	if virtual != expect_virtual:
		_fail(path + " virtual geometry presence is %s" % virtual)


func _save_and_reload(path: String, mesh: ArrayMesh) -> void:
	var save_path := "res://.godot/roundtrip_" + path.get_file().get_basename() + ".res"
	var copy := mesh.duplicate() as ArrayMesh
	if copy == null:
		_fail("could not duplicate " + path)
		return
	var err := ResourceSaver.save(copy, save_path)
	if err != OK:
		_fail("could not save " + path + " (%d)" % err)
		return
	var reloaded := ResourceLoader.load(save_path, "", ResourceLoader.CACHE_MODE_IGNORE) as ArrayMesh
	if reloaded == null:
		_fail("could not reload " + save_path)
		return
	var vertices: PackedVector3Array = reloaded.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
	if vertices.is_empty():
		_fail(save_path + " reloaded with an empty surface")
	var shape := reloaded.create_trimesh_shape()
	if shape == null or shape.get_faces().is_empty():
		_fail(save_path + " reloaded with an empty trimesh")


func _begin_pair(pair_name: String, source: String) -> void:
	_pair_name = pair_name
	_phase += 1
	_wait = 0
	if _viewport != null:
		_viewport.free()
	_viewport = SubViewport.new()
	_viewport.size = Vector2i(256, 256)
	_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(_viewport)

	_world = Node3D.new()
	_world.name = "RoundTrip"
	_viewport.add_child(_world)
	var world := _world

	var loaded := ResourceLoader.load(source, "", ResourceLoader.CACHE_MODE_IGNORE)
	var packed := loaded as PackedScene
	if packed != null:
		var scene := packed.instantiate()
		world.add_child(scene)
	else:
		var mesh := loaded as ArrayMesh
		var instance := MeshInstance3D.new()
		instance.mesh = mesh
		world.add_child(instance)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.8, 0.75, 0.7)
	for node in world.find_children("*", "MeshInstance3D", true, false):
		var instance := node as MeshInstance3D
		instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
		# UV2 albedo capture needs a material bound. Imported boxes have none.
		instance.material_override = material

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-50, 30, 0)
	light.shadow_enabled = false
	light.light_bake_mode = Light3D.BAKE_STATIC
	world.add_child(light)

	var camera := Camera3D.new()
	camera.position = Vector3(1.6, 1.4, 2.4)
	world.add_child(camera)
	camera.look_at(Vector3(0, 0, 0))
	camera.current = true

	_lightmap = LightmapGI.new()
	_lightmap.quality = LightmapGI.BAKE_QUALITY_LOW
	_lightmap.bounces = 1
	_lightmap.use_denoiser = false
	_lightmap.supersampling = false
	_lightmap.environment_mode = LightmapGI.ENVIRONMENT_MODE_DISABLED
	_lightmap.generate_probes_subdiv = LightmapGI.GENERATE_PROBES_DISABLED
	world.add_child(_lightmap)
	_ensure_owners(world, world)


func _report_flip(label: String, virtual_image: Image, plain_image: Image) -> void:
	if virtual_image.get_width() != plain_image.get_width() or virtual_image.get_height() != plain_image.get_height():
		_fail(label + " captures differ in size")
		print("FLIP mean: not measured at PR time (G4-7); %s mean absolute error unavailable" % label)
		return
	virtual_image.convert(Image.FORMAT_RGBA8)
	plain_image.convert(Image.FORMAT_RGBA8)
	var sum := 0.0
	var pixels := virtual_image.get_width() * virtual_image.get_height()
	for y in virtual_image.get_height():
		for x in virtual_image.get_width():
			var a := virtual_image.get_pixel(x, y)
			var b := plain_image.get_pixel(x, y)
			sum += absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b)
	var mean := sum / float(pixels * 3)
	# G4-7's FLIP threshold is a PQ measurement. This number is mean absolute error.
	var lit := 0
	for y in range(0, virtual_image.get_height(), 8):
		for x in range(0, virtual_image.get_width(), 8):
			var sample := virtual_image.get_pixel(x, y)
			if sample.r > 0.02 or sample.g > 0.02 or sample.b > 0.02:
				lit += 1
	print("FLIP mean: not measured at PR time (G4-7); %s mean absolute error %.6f lit_samples %d" % [label, mean, lit])


func _ensure_owners(node: Node, owner: Node) -> void:
	for child in node.get_children():
		if child.owner == null:
			child.owner = owner
		_ensure_owners(child, owner)


func _fail(message: String) -> void:
	_failures += 1
	print("FAIL: " + message)
