@tool
extends Node

## Editor-hint lightmap bake for the virtual geometry round trip.
## LightmapperRD reads EditorSettings and EditorPaths. A --script run does not create them.
## Launch with GODOTNEXT_ROUNDTRIP_BAKE=1, --editor, and this scene. Without that variable this node does nothing.

const _SOURCES: Array[String] = [
	"res://tests/roundtrip/room_vg.gltf",
	"res://tests/roundtrip/room_plain.gltf",
	"res://tests/roundtrip/prop_vg.obj",
	"res://tests/roundtrip/prop_plain.obj",
]
const _BAKE_DIR := "res://tests/roundtrip/bake_out"

static var _started_once := false

var _index := 0
var _wait := 0
var _phase := "setup"
var _baking := false
var _viewport: SubViewport
var _world: Node3D
var _lightmap: LightmapGI
var _failures := 0


func _ready() -> void:
	if not Engine.is_editor_hint():
		return
	if OS.get_environment("GODOTNEXT_ROUNDTRIP_BAKE") != "1":
		return
	# The editor can instance this scene twice (session restore and the command-line scene).
	if _started_once:
		return
	_started_once = true
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(_BAKE_DIR))
	set_process(true)
	_begin_source()


func _process(_delta: float) -> void:
	# LightmapGI.bake reimports the EXR through the editor file system, and that pumps
	# the main loop. Ignore those frames so a second bake cannot start on this scene.
	if _baking:
		return
	_wait += 1
	if _phase == "setup":
		# One rendered frame so the material uniform set exists before the UV2 bake.
		if _wait < 8:
			return
		var filesystem := EditorInterface.get_resource_filesystem()
		if filesystem == null or filesystem.is_scanning() or filesystem.is_importing():
			return
		_baking = true
		_bake_current()
		_baking = false
	elif _phase == "capture":
		if _wait < 2:
			return
		_recapture()


func _begin_source() -> void:
	var source := _SOURCES[_index]
	if _viewport != null:
		_viewport.free()
	_viewport = SubViewport.new()
	_viewport.own_world_3d = true
	_viewport.size = Vector2i(256, 256)
	_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(_viewport)

	_world = Node3D.new()
	_viewport.add_child(_world)

	var loaded := ResourceLoader.load(source, "", ResourceLoader.CACHE_MODE_IGNORE)
	var packed := loaded as PackedScene
	if packed != null:
		_world.add_child(packed.instantiate())
	else:
		var mesh := loaded as ArrayMesh
		var instance := MeshInstance3D.new()
		instance.mesh = mesh
		_world.add_child(instance)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.8, 0.75, 0.7)
	for node in _world.find_children("*", "MeshInstance3D", true, false):
		var instance := node as MeshInstance3D
		instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
		instance.material_override = material

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-50, 30, 0)
	light.shadow_enabled = false
	light.light_bake_mode = Light3D.BAKE_STATIC
	_world.add_child(light)

	var camera := Camera3D.new()
	camera.position = Vector3(1.6, 1.4, 2.4)
	_world.add_child(camera)
	camera.look_at(Vector3(0, 0, 0))
	camera.current = true

	_lightmap = LightmapGI.new()
	_lightmap.quality = LightmapGI.BAKE_QUALITY_LOW
	_lightmap.bounces = 1
	_lightmap.use_denoiser = false
	_lightmap.supersampling = false
	_lightmap.environment_mode = LightmapGI.ENVIRONMENT_MODE_DISABLED
	_lightmap.generate_probes_subdiv = LightmapGI.GENERATE_PROBES_DISABLED
	_world.add_child(_lightmap)
	_ensure_owners(_world, _world)
	_phase = "setup"
	_wait = 0
	print("ROUNDTRIP_BAKE_BEGIN %s" % source)


func _bake_current() -> void:
	var source := _SOURCES[_index]
	var bake_path := _BAKE_DIR + "/roundtrip_lm_" + source.get_file().get_basename() + ".res"
	var bake_error: int = NextGeoTestBridge.bake_lightmap(_lightmap, _world, bake_path)
	print("ROUNDTRIP_BAKE_RESULT %s %d" % [source, bake_error])
	if bake_error != LightmapGI.BAKE_ERROR_OK:
		_failures += 1
		print("FAIL: lightmap bake failed for " + source + " (BakeError %d)" % bake_error)
		_advance()
		return
	_phase = "capture"
	_wait = 0


func _recapture() -> void:
	var source := _SOURCES[_index]
	var image := _viewport.get_texture().get_image()
	if image == null or image.is_empty():
		_failures += 1
		print("FAIL: recapture empty for " + source)
	else:
		image.convert(Image.FORMAT_RGBA8)
		var lit := 0
		for y in range(0, image.get_height(), 8):
			for x in range(0, image.get_width(), 8):
				var sample := image.get_pixel(x, y)
				if sample.r > 0.02 or sample.g > 0.02 or sample.b > 0.02:
					lit += 1
		print("ROUNDTRIP_RECAPTURE %s lit_samples %d" % [source, lit])
	_advance()


func _advance() -> void:
	_index += 1
	if _index >= _SOURCES.size():
		print("roundtrip bake: %d failure(s)" % _failures)
		get_tree().quit(_failures)
		return
	_begin_source()


func _ensure_owners(node: Node, owner: Node) -> void:
	for child in node.get_children():
		if child.owner == null:
			child.owner = owner
		_ensure_owners(child, owner)
