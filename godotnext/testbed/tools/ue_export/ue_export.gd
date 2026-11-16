extends SceneTree
## Exports a testbed scene for its UE 5.9 comparison port (#235, plan §23.7): every unique mesh
## once as glTF, and every placement, material, light, the environment, the sky and the camera
## path as JSON, CSV and a Radiance HDR panorama. ue_export_schema.md describes the files.
##
## Run it rendered, not with --headless: the dummy renderer keeps no MultiMesh instance
## transforms, so a headless export would lose every MultiMesh placement.
##
##   godot --path godotnext/testbed --script res://tools/ue_export/ue_export.gd -- \
##     --scene=res://scenes/swrt_kitbash/swrt_kitbash.tscn --camera=CaptureCam \
##     --warmup=120 --frames=600 --out=/absolute/dir [--fps=60] [--source-commit=<sha>]
##     [--approximate=<id>[,<id>]] [--camera-cuts=<frame>[,<frame>]]
##
## Other user arguments (for example --instances) reach the scene as in a harness run. The scene
## runs as in a timing run: every _ready runs, so seeded generators spawn their content, and the
## camera and every mesh instance are sampled while the harness publishes frame index N for row N
## (Harness.drive_frame), the pose a timing run shows at measured row N. A mesh instance that
## moves gets a per-frame transform track. Content the format does not carry yet (moving lights,
## MultiMeshes or other nodes, unsupported materials, other visual nodes) stops the export
## instead of being left out. Content the UE import builds only approximately
## (Lib.APPROXIMATIONS) is refused too, unless --approximate names it: scene.json then lists it.
## A scene that resets the temporal history (Viewport.reset_temporal_history) does so in its own
## script, which the export cannot see: --camera-cuts names those frames, and scene.json carries
## them so the UE Level Sequence cuts there too.
##
## Exit codes: 0 written, 1 the scene has content the format does not carry, 2 bad arguments.

const Lib := preload("res://tools/ue_export/ue_export_lib.gd")
const SceneArgs := preload("res://scenes/scene_args.gd")

const SETTLE_FRAMES := 2
## Project settings written to scene.json "rendering.project".
const PROJECT_PREFIXES: Array[String] = [
	"rendering/global_illumination/",
	"rendering/nextgeo/",
	"rendering/anti_aliasing/",
	"rendering/scaling_3d/",
	"rendering/lights_and_shadows/",
	"rendering/environment/",
	"rendering/renderer/rendering_method",
	"rendering/rendering_device/driver",
	"display/window/size/viewport_",
]
## Project settings and the row of docs/godotnext/ue-bar/settings-map.md that maps them to UE.
const RECORD_ROWS := {
	"rendering/global_illumination/nextgi/quality": "`quality = high`, HWRT / `quality = epic`, HWRT / `quality = medium`",
	"rendering/global_illumination/nextgi/hardware_ray_tracing": "`TIER_SOFTWARE`, forced (`hardware_ray_tracing = off`)",
	"rendering/global_illumination/nextgi/software_ray_tracing_mode": "`software_ray_tracing_mode = detail`",
	"rendering/global_illumination/nextgi/medium_gather": "`medium_gather = screen_probes`",
	"rendering/global_illumination/nextgi/mode": "`nextgi/mode = reflections_only` over LightmapGI",
	"rendering/global_illumination/nextgi/far_field": "Far field",
	"rendering/global_illumination/mesh_sdf/keep_when_hwrt": "`meshes/generate_distance_fields`; `mesh_sdf/keep_when_hwrt`",
	"rendering/scaling_3d/mode": "1080p internal, the line's output resolution, `scaling_3d_scale`",
	"rendering/scaling_3d/scale": "1080p internal, the line's output resolution, `scaling_3d_scale`",
}

var _scene_path := ""
var _camera_name := ""
var _out := ""
var _frames := 0
var _warmup := 0
var _fps := 60
var _source_commit := ""
var _approximate: Array[String] = []
var _cuts: Array[int] = []
var _scene: Node
var _camera: Camera3D
var _harness: Node
var _ticks := 0
var _row := -1
var _done := false
var _track := PackedStringArray()
var _baseline := {}

# Per-frame sampling (#232), filled from row 0 on. A slot is one MeshInstance3D's world transform
# (bind -1) or one rigid part of a skinned one (its skin bind).
## "<node path>|<bind>" -> slot index.
var _slots := {}
## Each slot's row-0 transform.
var _slot_row0: Array[Transform3D] = []
## Slot index -> Array of Transform3D, one per row so far, for the slots that moved.
var _slot_moves := {}
## [node, path, skeleton, skin, [[bind, bone], ...]] per MeshInstance3D (skeleton and skin null
## when it is not skinned).
var _sampled_nodes: Array = []
## [node, path, global transform, visible, light energy or -1, MultiMesh state or null] per light
## and per other geometry node, and the visibility of every mesh instance: compared with row 0 on
## every row. A MultiMesh's state is [its MultiMesh, its instance buffer, its visible instance count].
var _watched: Array = []
var _motion_errors: Array[String] = []
## The largest distance between a rigid part's vertex and the same vertex skinned by the renderer
## (MeshInstance3D.bake_mesh_from_current_skeleton_pose), over every row, in meters, and how many
## vertices that compared.
var _rigid_skin_error := 0.0
var _rigid_skin_compared := 0
## Track files to write: [file name, Array of Transform3D].
var _pending_tracks: Array = []
## "<mesh instance id>:<bind>" -> the ArrayMesh of that rigid part.
var _part_meshes := {}
var _movers := 0

# Filled by _walk().
var _mesh_ids := {}
var _meshes: Array = []
var _material_ids := {}
var _materials: Array = []
var _material_count := 0
var _texture_ids := {}
var _textures: Array = []
var _instances: Array = []
var _lights: Array = []
var _suns: Array = []
var _environment_node: WorldEnvironment
var _world_transforms: Array = []
var _triangles_total := 0
var _aabb := AABB()
var _aabb_set := false
var _node_instances := 0
var _multimesh_instances := 0
var _multimesh_nodes := 0
var _unsupported: Array[String] = []
## Lib.APPROXIMATIONS id -> the items (material or node names) that need it.
var _approximation_items := {}
## Light entries and their nodes, for the mask effects resolved once every layer is known.
var _light_nodes: Array = []
var _layers_in_use := 0


func _initialize() -> void:
	var error := _parse_args()
	if not error.is_empty():
		_fail(2, error)
		return
	if DisplayServer.get_name() == "headless":
		_fail(2, "run it with a rendering driver, not --headless: the dummy renderer keeps no MultiMesh instance transforms")
		return
	var packed := load(_scene_path) as PackedScene
	if packed == null:
		_fail(2, "cannot load %s" % _scene_path)
		return
	_harness = get_root().get_node_or_null("Harness")
	if _harness == null:
		_harness = (load("res://harness/harness.gd") as GDScript).new()
		_harness.name = "Harness"
		get_root().add_child(_harness)
	if not _harness.has_method("drive_frame"):
		_fail(2, "the harness has no drive_frame(); this exporter needs the matching harness.gd")
		return
	_scene = packed.instantiate()
	get_root().add_child(_scene)
	current_scene = _scene
	var found := _scene.get_node_or_null(NodePath(_camera_name))
	if found == null:
		found = _scene.find_child(_camera_name, true, false)
	if not (found is Camera3D):
		_fail(2, "no Camera3D named %s in %s" % [_camera_name, _scene_path])
		return
	_camera = found


func _process(_delta: float) -> bool:
	if _done:
		return false
	_ticks += 1
	if _ticks < SETTLE_FRAMES:
		return false
	if _row < 0:
		_publish(0)
		return false
	# The nodes processed frame _row in the previous tick.
	_track.append(_camera_row(_row))
	if _row == 0:
		_baseline = _motion_state()
		_start_sampling()
	_sample_row()
	if _row + 1 < _frames:
		_publish(_row + 1)
		return false
	_finish()
	return false


func _parse_args() -> String:
	_scene_path = SceneArgs.value("--scene")
	_camera_name = SceneArgs.value("--camera")
	_out = SceneArgs.value("--out")
	_frames = SceneArgs.int_value("--frames", 0)
	_warmup = SceneArgs.int_value("--warmup", 0)
	_fps = SceneArgs.int_value("--fps", 60)
	_source_commit = SceneArgs.value("--source-commit")
	for id in SceneArgs.value("--approximate").split(",", false):
		if not Lib.APPROXIMATIONS.has(id):
			return "--approximate takes %s, not %s" % [", ".join(PackedStringArray(Lib.APPROXIMATIONS.keys())), id]
		_approximate.append(id)
	for cut in SceneArgs.value("--camera-cuts").split(",", false):
		if not cut.is_valid_int():
			return "--camera-cuts takes frame numbers, not %s" % cut
		_cuts.append(int(cut))
	_cuts.sort()
	if not _scene_path.begins_with("res://"):
		return "--scene=res://... is required"
	if _camera_name.is_empty():
		return "--camera=<name or path> is required"
	if _out.is_empty() or _out.begins_with("res://") or _out.begins_with("user://") or not _out.is_absolute_path():
		return "--out has to be an absolute filesystem path"
	if _frames < 1:
		return "--frames has to be at least 1"
	if _warmup < 0 or _fps < 1:
		return "--warmup has to be at least 0 and --fps at least 1"
	for index in _cuts.size():
		if _cuts[index] < 1 or _cuts[index] >= _frames or (index > 0 and _cuts[index] == _cuts[index - 1]):
			return "--camera-cuts takes distinct frames from 1 to --frames - 1"
	return ""


func _publish(p_frame: int) -> void:
	_row = p_frame
	_harness.call("drive_frame", p_frame)


func _finish() -> void:
	_done = true
	_harness.call("drive_frame", -1)
	var moved := _motion_error(_baseline, _motion_state())
	if moved.is_empty() and not _motion_errors.is_empty():
		moved = _motion_errors[0]
	if not moved.is_empty():
		_fail(1, moved)
		return
	var error := _export()
	if not error.is_empty():
		_fail(1, error)
		return
	quit(0)


## A run that ends before the export is written (a scene that quits the tree at its own --frames
## limit, for example) is a failure, not an empty success.
func _finalize() -> void:
	if not _done:
		printerr("ue_export: the tree quit at camera row %d of %d, before the export was written (a scene's own --frames limit, for example)" % [_row, _frames])
		quit(1)


func _fail(p_code: int, p_message: String) -> void:
	_done = true
	printerr("ue_export: " + p_message)
	quit(p_code)


func _camera_row(p_frame: int) -> String:
	var transform := _camera.global_transform.orthonormalized()
	var q := transform.basis.get_rotation_quaternion()
	var values := [transform.origin.x, transform.origin.y, transform.origin.z, q.x, q.y, q.z, q.w, _camera.fov, _camera.near, _camera.far]
	var cells := PackedStringArray([str(p_frame)])
	for value: float in values:
		cells.append("%.9f" % value)
	return ",".join(cells)


## Transforms of every geometry node other than a mesh instance (mesh instances are sampled on
## every row and exported as tracks when they move) and light, the visibility of every one of
## them, every MultiMesh's instances, and light energies: compared between the first and the last
## sampled frame. _sample_row() compares the same things with row 0 on every row.
func _motion_state() -> Dictionary:
	var state := {}
	var stack: Array[Node] = [_scene]
	while not stack.is_empty():
		var node: Node = stack.pop_back()
		if node is GeometryInstance3D or node is Light3D:
			var key := str(_scene.get_path_to(node))
			var placed: Variant = null if node is MeshInstance3D else (node as Node3D).global_transform
			var entry := [placed, (node as Node3D).is_visible_in_tree()]
			if node is Light3D:
				entry.append((node as Light3D).light_energy)
			if node is MultiMeshInstance3D and (node as MultiMeshInstance3D).multimesh != null:
				var multimesh := (node as MultiMeshInstance3D).multimesh
				var transforms: Array = []
				for index in multimesh.instance_count:
					transforms.append(multimesh.get_instance_transform(index))
				entry.append(Lib.transforms_sha256(transforms))
			state[key] = entry
		for child in node.get_children():
			stack.append(child)
	return state


func _motion_error(p_first: Dictionary, p_last: Dictionary) -> String:
	if p_first.size() != p_last.size():
		return "the scene added or removed geometry or lights while the camera path ran; that is not exported yet"
	for key: String in p_first:
		if not p_last.has(key) or p_first[key] != p_last[key]:
			return "%s moved or changed while the camera path ran; only mesh instance transforms are exported as tracks" % key
	return ""


## Row 0: the mesh instances to sample and the other nodes to watch.
func _start_sampling() -> void:
	var stack: Array[Node] = [_scene]
	while not stack.is_empty():
		var node: Node = stack.pop_back()
		for child in node.get_children():
			stack.append(child)
		if not (node is GeometryInstance3D or node is Light3D):
			continue
		var path := str(_scene.get_path_to(node))
		var node_3d := node as Node3D
		if node is MeshInstance3D:
			var instance := node as MeshInstance3D
			var skeleton: Skeleton3D = null
			var binds: Array = []
			_slots["%s|-1" % path] = _slots.size()
			if instance.skin != null and instance.mesh != null:
				skeleton = instance.get_node_or_null(instance.skeleton) as Skeleton3D
				if skeleton != null:
					for bind in instance.skin.get_bind_count():
						var bone := instance.skin.get_bind_bone(bind)
						if instance.skin.get_bind_name(bind) != StringName():
							bone = skeleton.find_bone(instance.skin.get_bind_name(bind))
						binds.append([bind, bone])
						_slots["%s|%d" % [path, bind]] = _slots.size()
			_sampled_nodes.append([instance, path, skeleton, instance.skin if skeleton != null else null, binds])
			_watched.append([node_3d, path, null, node_3d.is_visible_in_tree(), -1.0, null])
		else:
			var energy := (node as Light3D).light_energy if node is Light3D else -1.0
			var multimesh_state: Variant = null
			if node is MultiMeshInstance3D and (node as MultiMeshInstance3D).multimesh != null:
				var multimesh := (node as MultiMeshInstance3D).multimesh
				multimesh_state = [multimesh, multimesh.buffer, multimesh.visible_instance_count]
			_watched.append([node_3d, path, node_3d.global_transform, node_3d.is_visible_in_tree(), energy, multimesh_state])


## One row: every mesh instance's world transform (and its rigid parts when skinned), and the
## other nodes against row 0.
func _sample_row() -> void:
	var slot := 0
	for entry: Array in _sampled_nodes:
		var instance: MeshInstance3D = entry[0]
		var world := instance.global_transform
		_sample_slot(slot, world)
		slot += 1
		var skeleton: Skeleton3D = entry[2]
		if skeleton == null:
			continue
		var skin: Skin = entry[3]
		var part_transforms := {}
		for pair: Array in entry[4]:
			var bind: int = pair[0]
			var bone: int = pair[1]
			var part := world
			if bone >= 0 and bone < skeleton.get_bone_count():
				part = world * skeleton.get_bone_global_pose(bone) * skin.get_bind_pose(bind)
				part_transforms[bind] = part
			_sample_slot(slot, part)
			slot += 1
		_check_rigid_skin(instance, world, part_transforms)
	for entry: Array in _watched:
		var node: Node3D = entry[0]
		var changed: bool = node.is_visible_in_tree() != bool(entry[3])
		if entry[2] != null and node.global_transform != entry[2]:
			changed = true
		if float(entry[4]) >= 0.0 and (node as Light3D).light_energy != float(entry[4]):
			changed = true
		if entry[5] != null:
			var state: Array = entry[5]
			var multimesh := (node as MultiMeshInstance3D).multimesh
			if multimesh != state[0] or multimesh.visible_instance_count != int(state[2]) or multimesh.buffer != state[1]:
				changed = true
		if changed and _motion_errors.is_empty():
			_motion_errors.append("%s moved or changed at row %d; only mesh instance transforms are exported as tracks" % [entry[1], _row])


## Row _row of one slot. Only slots that move keep every row; a still slot keeps its row 0.
func _sample_slot(p_slot: int, p_transform: Transform3D) -> void:
	if _row == 0:
		_slot_row0.append(p_transform)
		return
	if _slot_moves.has(p_slot):
		(_slot_moves[p_slot] as Array).append(p_transform)
	elif p_transform != _slot_row0[p_slot]:
		var rows: Array = []
		for row in _row:
			rows.append(_slot_row0[p_slot])
		rows.append(p_transform)
		_slot_moves[p_slot] = rows


## The rigid parts against the renderer's own skinning of the same pose (Lib.rigid_skin_error):
## every vertex of a part, moved by its part transform, must sit where the skinned mesh puts it.
func _check_rigid_skin(p_instance: MeshInstance3D, p_world: Transform3D, p_parts: Dictionary) -> void:
	var split := Lib.rigid_skin_parts(p_instance.mesh)
	if split.has("error"):
		return
	var to_local := p_world.affine_inverse()
	var local_parts := {}
	for bind: int in p_parts:
		local_parts[bind] = to_local * (p_parts[bind] as Transform3D)
	var result := Lib.rigid_skin_error(split, p_instance.bake_mesh_from_current_skeleton_pose(), local_parts)
	_rigid_skin_error = maxf(_rigid_skin_error, float(result["max_m"]))
	_rigid_skin_compared += int(result["compared"])


func _export() -> String:
	_walk(_scene)
	_resolve_light_masks()
	if Lib.mask_effect(_camera.cull_mask, _layers_in_use) != "all":
		_unsupported.append("a camera cull mask that hides some placed layers (not exported yet)")
	var environment_refusals := Lib.environment_refusals(_environment_node.environment if _environment_node != null else null, _camera)
	for refusal in environment_refusals:
		_unsupported.append(refusal)
	var approximations: Array = []
	var ids: Array = _approximation_items.keys()
	ids.sort()
	for id: String in ids:
		var items: Array = _approximation_items[id]
		if id in _approximate:
			approximations.append({"id": id, "description": Lib.APPROXIMATIONS[id], "items": items})
		else:
			_unsupported.append("%s on %s (pass --approximate=%s to export it as a recorded approximation)" % [id, ", ".join(PackedStringArray(items)), id])
	if not _unsupported.is_empty():
		return "not exported yet: " + "; ".join(_unsupported)
	DirAccess.make_dir_recursive_absolute(_out)
	var files: Array[String] = []

	var library: Array = []
	for mesh_entry: Dictionary in _meshes:
		library.append({"name": mesh_entry["id"], "mesh": mesh_entry["library_mesh"]})
	var error := _write_glb(_out.path_join("library.glb"), library)
	if not error.is_empty():
		return error
	files.append("library.glb")
	error = _write_glb(_out.path_join("axes.glb"), _axes_fixture())
	if not error.is_empty():
		return error
	files.append("axes.glb")
	var texture_entries: Array = []
	for texture_entry: Dictionary in _textures:
		var entry := texture_entry.duplicate()
		DirAccess.make_dir_recursive_absolute(_out.path_join("textures"))
		error = _write_bytes(String(entry["file"]), entry["png"])
		if not error.is_empty():
			return error
		files.append(String(entry["file"]))
		entry.erase("png")
		texture_entries.append(entry)
	for pending: Array in _pending_tracks:
		DirAccess.make_dir_recursive_absolute(_out.path_join("tracks"))
		error = _write_text(String(pending[0]), _track_csv(pending[1]))
		if not error.is_empty():
			return error
		files.append(String(pending[0]))

	var csv := "frame,px,py,pz,qx,qy,qz,qw,fov_deg,near,far\n" + "\n".join(_track) + "\n"
	error = _write_text("camera_track.csv", csv)
	if not error.is_empty():
		return error
	files.append("camera_track.csv")

	var environment: Variant = null
	if _environment_node != null and _environment_node.environment != null:
		var sky_entry: Variant = null
		var env := _environment_node.environment
		if env.sky != null:
			var material := env.sky.sky_material
			sky_entry = {
				"properties": Lib.storage_properties(env.sky),
				"material": {"class": material.get_class() if material != null else "", "properties": Lib.storage_properties(material) if material != null else {}},
			}
			if material is ProceduralSkyMaterial:
				var pixels := Lib.procedural_sky_panorama(material, _suns, Lib.SKY_WIDTH, Lib.SKY_HEIGHT)
				error = _write_bytes("sky.hdr", Lib.encode_hdr(Lib.SKY_WIDTH, Lib.SKY_HEIGHT, pixels))
				if not error.is_empty():
					return error
				files.append("sky.hdr")
				sky_entry["panorama"] = "sky.hdr"
				sky_entry["panorama_size"] = [Lib.SKY_WIDTH, Lib.SKY_HEIGHT]
				sky_entry["panorama_projection"] = "godot_equirect"
			elif material != null:
				return "not exported yet: sky material %s (only ProceduralSkyMaterial is baked)" % material.get_class()
		environment = {
			"node": str(_scene.get_path_to(_environment_node)),
			"properties": Lib.storage_properties(env),
			"sky": sky_entry,
			"camera_attributes": Lib.storage_properties(_environment_node.camera_attributes) if _environment_node.camera_attributes != null else null,
		}

	var aabb_end := _aabb.end
	var mesh_entries: Array = []
	for mesh_entry: Dictionary in _meshes:
		var entry := mesh_entry.duplicate()
		entry.erase("library_mesh")
		mesh_entries.append(entry)
	var scene := {
		"schema": Lib.SCHEMA,
		"scene": _scene_path,
		"units": {"length": "m", "up": "+Y", "handedness": "right", "camera_forward": "-Z", "light_forward": "-Z"},
		"fps": _fps,
		"warmup": _warmup,
		"frames": _frames,
		"camera": {
			"node": str(_scene.get_path_to(_camera)),
			"track": "camera_track.csv",
			"fov_deg": _camera.fov,
			"fov_axis": "vertical" if _camera.keep_aspect == Camera3D.KEEP_HEIGHT else "horizontal",
			"near": _camera.near,
			"far": _camera.far,
			"projection": "perspective" if _camera.projection == Camera3D.PROJECTION_PERSPECTIVE else "unsupported",
			"attributes": Lib.storage_properties(_camera.attributes) if _camera.attributes != null else null,
			"cuts": _cuts,
		},
		"meshes": mesh_entries,
		"materials": _materials,
		"textures": texture_entries,
		"instances": _instances,
		"lights": _lights,
		"approximations": approximations,
		"environment": environment,
		"rendering": _rendering(),
		"checks": {
			"instances_total": _node_instances + _multimesh_instances,
			"node_instances": _node_instances,
			"multimesh_instances": _multimesh_instances,
			"multimesh_nodes": _multimesh_nodes,
			"meshes": _meshes.size(),
			"materials": _materials.size(),
			"lights": _lights.size(),
			"light_types": _light_types(),
			"textures": _textures.size(),
			"triangles_total": _triangles_total,
			"aabb": {"min": Lib.json_value(_aabb.position), "max": Lib.json_value(aabb_end)},
			"instances_sha256": Lib.transforms_sha256(_world_transforms),
			"positions_sha256": Lib.positions_sha256(_world_transforms),
			"camera_rows": _track.size(),
			"movers": _movers,
			"rigid_skin_max_error_m": Lib.json_value(_rigid_skin_error),
			"rigid_skin_vertices_compared": _rigid_skin_compared,
		},
	}
	if _camera.projection != Camera3D.PROJECTION_PERSPECTIVE:
		return "not exported yet: a camera projection other than perspective"
	error = _write_text("scene.json", JSON.stringify(scene, "\t", true, true) + "\n")
	if not error.is_empty():
		return error
	files.append("scene.json")

	var checksums := {}
	for file_name in files:
		checksums[file_name] = FileAccess.get_sha256(_out.path_join(file_name))
	var version := Engine.get_version_info()
	var manifest := {
		"schema": Lib.SCHEMA,
		"tool": "res://tools/ue_export/ue_export.gd",
		"engine": {"version": String(version["string"]), "hash": String(version["hash"])},
		"source_commit": _source_commit,
		"scene": _scene_path,
		"scene_sha256": FileAccess.get_sha256(_scene_path),
		"user_args": Array(OS.get_cmdline_user_args()),
		"files": checksums,
	}
	error = _write_text("export_manifest.json", JSON.stringify(manifest, "\t", true, true) + "\n")
	if not error.is_empty():
		return error
	print("GODOTNEXT_UE_EXPORT " + JSON.stringify({
		"scene": _scene_path,
		"instances": _node_instances + _multimesh_instances,
		"meshes": _meshes.size(),
		"materials": _materials.size(),
		"lights": _lights.size(),
		"textures": _textures.size(),
		"approximations": approximations.map(func(entry: Dictionary) -> String: return String(entry["id"])),
		"camera_rows": _track.size(),
		"movers": _movers,
		"files": files.size() + 1,
	}, "", true))
	return ""


func _walk(p_node: Node) -> void:
	if p_node is Node3D and not (p_node as Node3D).is_visible_in_tree():
		return
	if p_node is GeometryInstance3D and (p_node as GeometryInstance3D).material_overlay != null:
		_unsupported.append("%s has a material overlay" % _scene.get_path_to(p_node))
	elif p_node is MeshInstance3D:
		_add_mesh_instance(p_node)
	elif p_node is MultiMeshInstance3D:
		_add_multimesh(p_node)
	elif p_node is DirectionalLight3D or p_node is OmniLight3D or p_node is SpotLight3D or p_node is AreaLight3D:
		_add_light(p_node)
	elif p_node is GPUParticles3D or p_node is CPUParticles3D:
		_need_approximation("particles", str(_scene.get_path_to(p_node)))
	elif p_node is VisualInstance3D:
		_unsupported.append("%s (%s)" % [_scene.get_path_to(p_node), p_node.get_class()])
	elif p_node is WorldEnvironment:
		if _environment_node != null:
			_unsupported.append("a second WorldEnvironment at %s" % _scene.get_path_to(p_node))
		_environment_node = p_node
	for child in p_node.get_children():
		_walk(child)


func _add_mesh_instance(p_node: MeshInstance3D) -> void:
	var mesh := p_node.mesh
	if mesh == null:
		return
	var path := str(_scene.get_path_to(p_node))
	if p_node.skin != null:
		_add_skinned(p_node, path)
		return
	var id := _mesh_id(mesh)
	if id.is_empty():
		return
	var materials: Array = []
	var sources: Array = []
	for surface in mesh.get_surface_count():
		sources.append(p_node.get_active_material(surface))
		materials.append(_material_id(sources[-1]))
	_check_translucent_shadow(p_node, sources)
	_add_node_instance(p_node, path, id, materials, "%s|-1" % path, mesh, null)


## One placed mesh: at its row-0 transform, with a per-frame track when it moves.
func _add_node_instance(p_node: MeshInstance3D, p_path: String, p_id: String, p_materials: Array, p_slot_key: String, p_mesh: Mesh, p_skin_part: Variant) -> void:
	var slot := int(_slots.get(p_slot_key, -1))
	var transform := p_node.global_transform
	if slot >= 0 and slot < _slot_row0.size():
		transform = _slot_row0[slot]
	var entry := {
		"kind": "node",
		"node": p_path,
		"mesh": p_id,
		"materials": p_materials,
		"transform": Lib.transform_rows(transform),
		"track": _track_for(slot),
		"cast_shadow": p_node.cast_shadow,
		"gi_mode": p_node.gi_mode,
		"visibility_range": [p_node.visibility_range_begin, p_node.visibility_range_end],
		"layers": p_node.layers,
	}
	if p_skin_part != null:
		entry["skin_part"] = p_skin_part
	_instances.append(entry)
	_layers_in_use |= p_node.layers
	_add_placement(transform, p_mesh)
	_node_instances += 1


## The track file of a mover (its transform differs from row 0 on some row), queued for _export;
## null for an instance that holds still.
func _track_for(p_slot: int) -> Variant:
	if not _slot_moves.has(p_slot):
		return null
	var rows: Array = _slot_moves[p_slot]
	if rows.size() != _frames:
		_unsupported.append("a mover's track has %d rows, not %d" % [rows.size(), _frames])
		return null
	var file := "tracks/t%04d.csv" % _pending_tracks.size()
	_pending_tracks.append([file, rows])
	_movers += 1
	return file


## A track as CSV: frame, then the 12 numbers of the world transform (row-major 3x4, as scene.json).
func _track_csv(p_rows: Array) -> String:
	var lines := PackedStringArray(["frame,m00,m01,m02,m03,m10,m11,m12,m13,m20,m21,m22,m23"])
	for row in p_rows.size():
		var cells := PackedStringArray([str(row)])
		for value: float in Lib.transform_rows(p_rows[row]):
			cells.append("%.9f" % value)
		lines.append(",".join(cells))
	return "\n".join(lines) + "\n"


## A skinned mesh as rigid parts, one per skin bind (APPROXIMATIONS.rigid_skin): each part is the
## triangles bound to that bind, placed and tracked with the bone's per-frame transform.
func _add_skinned(p_node: MeshInstance3D, p_path: String) -> void:
	var skeleton := p_node.get_node_or_null(p_node.skeleton) as Skeleton3D
	if skeleton == null:
		_unsupported.append("skinned mesh %s without a Skeleton3D" % p_path)
		return
	if not skeleton.global_transform.is_equal_approx(p_node.global_transform):
		_unsupported.append("skinned mesh %s whose Skeleton3D is not at the mesh instance's transform (not exported yet)" % p_path)
		return
	var split := Lib.rigid_skin_parts(p_node.mesh)
	if split.has("error"):
		_unsupported.append("skinned mesh %s: %s (a skinned mesh is exported only as rigid parts: triangle lists without blend shapes, every vertex on one bone at full weight)" % [p_path, split["error"]])
		return
	if _rigid_skin_error > 1e-4:
		_unsupported.append("skinned mesh %s: its rigid parts are up to %s m off the renderer's skinning" % [p_path, _rigid_skin_error])
		return
	_need_approximation("rigid_skin", p_path)
	var bones := {}
	for pair: Array in _sampled_nodes:
		if pair[1] == p_path:
			for bind_bone: Array in pair[4]:
				bones[int(bind_bone[0])] = int(bind_bone[1])
	for part: Dictionary in split["parts"]:
		var bind := int(part["bind"])
		var key := "%d:%d" % [p_node.mesh.get_instance_id(), bind]
		if not _part_meshes.has(key):
			var part_mesh := ArrayMesh.new()
			part_mesh.resource_name = "%s bind %d" % [p_node.mesh.resource_name, bind]
			for arrays: Array in part["arrays"]:
				part_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
			_part_meshes[key] = part_mesh
		var mesh: ArrayMesh = _part_meshes[key]
		var id := _mesh_id(mesh)
		if id.is_empty():
			return
		var materials: Array = []
		var sources: Array = []
		for surface: int in part["surfaces"]:
			sources.append(p_node.get_active_material(surface))
			materials.append(_material_id(sources[-1]))
		_check_translucent_shadow(p_node, sources)
		var bone := int(bones.get(bind, -1))
		var skin_part := {
			"bind": bind,
			"bone": skeleton.get_bone_name(bone) if bone >= 0 and bone < skeleton.get_bone_count() else "",
			"surfaces": part["surfaces"],
		}
		if bone < 0 or bone >= skeleton.get_bone_count():
			_unsupported.append("skinned mesh %s: skin bind %d has no bone" % [p_path, bind])
			return
		_add_node_instance(p_node, p_path, id, materials, "%s|%d" % [p_path, bind], mesh, skin_part)


func _add_multimesh(p_node: MultiMeshInstance3D) -> void:
	var multimesh := p_node.multimesh
	if multimesh == null or multimesh.mesh == null:
		return
	var path := str(_scene.get_path_to(p_node))
	if multimesh.transform_format != MultiMesh.TRANSFORM_3D:
		_unsupported.append("2D MultiMesh %s" % path)
		return
	var id := _mesh_id(multimesh.mesh)
	if id.is_empty():
		return
	var materials: Array = []
	var sources: Array = []
	for surface in multimesh.mesh.get_surface_count():
		var material := p_node.material_override if p_node.material_override != null else multimesh.mesh.surface_get_material(surface)
		sources.append(material)
		materials.append(_material_id(material))
	_check_translucent_shadow(p_node, sources)
	var count := multimesh.instance_count
	if multimesh.visible_instance_count >= 0:
		count = mini(count, multimesh.visible_instance_count)
	var rows: Array = []
	var distinct := {}
	for index in count:
		var world := p_node.global_transform * multimesh.get_instance_transform(index)
		rows.append(Lib.transform_rows(world))
		distinct[world] = true
		_add_placement(world, multimesh.mesh)
	if count > 1 and distinct.size() == 1:
		_unsupported.append("%s: every MultiMesh instance has the same transform (the renderer kept no instance data)" % path)
		return
	if multimesh.use_colors:
		_unsupported.append("%s: per-instance MultiMesh colors (not exported yet)" % path)
		return
	_instances.append({
		"kind": "multimesh",
		"node": path,
		"mesh": id,
		"materials": materials,
		"transform": Lib.transform_rows(p_node.global_transform),
		"count": count,
		"instances": rows,
		"use_colors": multimesh.use_colors,
		"use_custom_data": multimesh.use_custom_data,
		"cast_shadow": p_node.cast_shadow,
		"gi_mode": p_node.gi_mode,
		"visibility_range": [p_node.visibility_range_begin, p_node.visibility_range_end],
		"layers": p_node.layers,
	})
	_layers_in_use |= p_node.layers
	_multimesh_instances += count
	_multimesh_nodes += 1


func _add_placement(p_world: Transform3D, p_mesh: Mesh) -> void:
	_world_transforms.append(p_world)
	_triangles_total += int(_meshes[_mesh_ids[p_mesh]["index"]]["triangles"])
	var box := p_world * p_mesh.get_aabb()
	if _aabb_set:
		_aabb = _aabb.merge(box)
	else:
		_aabb = box
		_aabb_set = true


func _mesh_id(p_mesh: Mesh) -> String:
	if _mesh_ids.has(p_mesh):
		return String(_mesh_ids[p_mesh]["id"])
	var id := "m%04d" % _meshes.size()
	var library_mesh := ArrayMesh.new()
	library_mesh.resource_name = id
	var surfaces: Array = []
	var triangles := 0
	var vertices := 0
	if not (p_mesh is ArrayMesh or p_mesh is PrimitiveMesh):
		_unsupported.append("%s mesh %s" % [p_mesh.get_class(), id])
		return ""
	if p_mesh is ArrayMesh and (p_mesh as ArrayMesh).get_blend_shape_count() > 0:
		_unsupported.append("blend shapes on mesh %s" % id)
		return ""
	for surface in p_mesh.get_surface_count():
		if p_mesh is ArrayMesh and (p_mesh as ArrayMesh).surface_get_primitive_type(surface) != Mesh.PRIMITIVE_TRIANGLES:
			_unsupported.append("a non-triangle surface on mesh %s" % id)
			return ""
		var arrays := p_mesh.surface_get_arrays(surface)
		var surface_vertices := (arrays[Mesh.ARRAY_VERTEX] as PackedVector3Array).size()
		var indices: Variant = arrays[Mesh.ARRAY_INDEX]
		var surface_triangles := (indices as PackedInt32Array).size() / 3 if indices != null else surface_vertices / 3
		if arrays[Mesh.ARRAY_BONES] != null:
			_unsupported.append("skinned surface on mesh %s" % id)
			return ""
		library_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
		library_mesh.surface_set_material(surface, p_mesh.surface_get_material(surface))
		surfaces.append({"triangles": surface_triangles, "vertices": surface_vertices})
		triangles += surface_triangles
		vertices += surface_vertices
	var nextgeo := false
	if p_mesh.has_method("get_virtual_geometry"):
		nextgeo = p_mesh.call("get_virtual_geometry") != null
	var source := p_mesh.resource_path if not p_mesh.resource_path.is_empty() and not p_mesh.resource_path.contains("::") else ""
	_mesh_ids[p_mesh] = {"id": id, "index": _meshes.size()}
	_meshes.append({
		"id": id,
		"gltf_node": id,
		"class": p_mesh.get_class(),
		"name": String(p_mesh.resource_name),
		"source": source,
		"surfaces": surfaces,
		"triangles": triangles,
		"vertices": vertices,
		"aabb": Lib.json_value(p_mesh.get_aabb()),
		"nextgeo": nextgeo,
		"library_mesh": library_mesh,
	})
	return id


func _material_id(p_material: Material) -> String:
	var key: Variant = p_material if p_material != null else "default"
	if _material_ids.has(key):
		return String(_material_ids[key])
	var material := p_material
	var id := "default"
	if material == null:
		material = StandardMaterial3D.new()
	else:
		id = "mat%04d" % _material_count
		_material_count += 1
	_material_ids[key] = id
	var label := id if material.resource_name.is_empty() else "%s (%s)" % [id, material.resource_name]
	if material is ShaderMaterial:
		_add_shader_material(material as ShaderMaterial, id, label)
		return id
	if not (material is BaseMaterial3D):
		_unsupported.append("%s material %s" % [material.get_class(), label])
		return id
	var base := material as BaseMaterial3D
	if base.next_pass != null:
		_unsupported.append("material %s has a next pass" % label)
		return id
	var refusals := Lib.material_refusals(base)
	if not refusals.is_empty():
		_unsupported.append("material %s: %s (not exported yet)" % [label, ", ".join(refusals)])
		return id
	for approximation in Lib.material_approximations(base):
		_need_approximation(approximation, label)
	var albedo_texture: Variant = null
	if base.albedo_texture != null:
		albedo_texture = _texture_id(base.albedo_texture, "albedo")
	var emission := base.emission.srgb_to_linear() * base.emission_energy_multiplier if base.emission_enabled else Color(0, 0, 0)
	_materials.append({
		"id": id,
		"name": String(base.resource_name),
		"class": base.get_class(),
		"blend": Lib.material_blend(base),
		"alpha_scissor_threshold": base.alpha_scissor_threshold,
		"two_sided": base.cull_mode == BaseMaterial3D.CULL_DISABLED,
		"albedo_linear": Lib.json_value(base.albedo_color.srgb_to_linear()),
		"albedo_texture": albedo_texture,
		"uv1_scale": Lib.json_value(base.uv1_scale),
		"uv1_offset": Lib.json_value(base.uv1_offset),
		"texture_filter": Lib.texture_filter_name(base),
		"texture_repeat": base.texture_repeat,
		"emission_linear": [emission.r, emission.g, emission.b],
		"properties": Lib.storage_properties(base),
	})
	return id


## A ShaderMaterial (APPROXIMATIONS.shader_material): its code, the code's SHA-256 and every
## uniform's value (the material's own, or the shader's default when the material leaves it
## unset); texture uniforms are written as textures. The UE import builds only shaders it knows.
func _add_shader_material(p_material: ShaderMaterial, p_id: String, p_label: String) -> void:
	var shader := p_material.shader
	if shader == null or shader.get_mode() != Shader.MODE_SPATIAL:
		_unsupported.append("ShaderMaterial %s without a spatial shader" % p_label)
		return
	if p_material.next_pass != null or p_material.render_priority != 0:
		_unsupported.append("ShaderMaterial %s with a next pass or a render priority" % p_label)
		return
	_need_approximation("shader_material", p_label)
	var uniforms := {}
	for info: Dictionary in shader.get_shader_uniform_list():
		var name := String(info["name"])
		var value: Variant = p_material.get_shader_parameter(name)
		var is_set := value != null
		if not is_set:
			value = RenderingServer.shader_get_parameter_default(shader.get_rid(), name)
		var entry := {"type": int(info["type"]), "set": is_set}
		if value is Texture2D:
			entry["texture"] = _texture_id(value as Texture2D, "shader")
		elif value is Object:
			_unsupported.append("ShaderMaterial %s uniform %s holds a %s" % [p_label, name, (value as Object).get_class()])
			continue
		else:
			entry["value"] = Lib.json_value(value)
		uniforms[name] = entry
	_materials.append({
		"id": p_id,
		"name": String(p_material.resource_name),
		"class": p_material.get_class(),
		"blend": "shader",
		"shader": {"code": shader.code, "sha256": shader.code.sha256_text(), "uniforms": uniforms},
		"properties": Lib.storage_properties(p_material),
	})


## A texture written as textures/<id>.png (RGBA8, the pixels as stored).
func _texture_id(p_texture: Texture2D, p_usage: String) -> Variant:
	if _texture_ids.has(p_texture):
		var known: Dictionary = _textures[int(_texture_ids[p_texture])]
		if not (p_usage in known["usages"]):
			(known["usages"] as Array).append(p_usage)
		return known["id"]
	var png := Lib.texture_png(p_texture)
	if png.is_empty():
		_unsupported.append("a %s texture (%s) without image data" % [p_usage, p_texture.get_class()])
		return null
	var id := "tex%04d" % _textures.size()
	_texture_ids[p_texture] = _textures.size()
	_textures.append({
		"id": id,
		"file": "textures/%s.png" % id,
		"class": p_texture.get_class(),
		"name": String(p_texture.resource_name),
		"size": [p_texture.get_width(), p_texture.get_height()],
		"mipmaps": p_texture.get_image() != null and p_texture.get_image().has_mipmaps(),
		"usages": [p_usage],
		"png": png,
	})
	return id


## An instance that casts shadows with an alpha-blended surface needs translucent_shadow: Godot
## draws no shadow for it, and the UE import keeps the instance's Cast Shadow on.
func _check_translucent_shadow(p_node: GeometryInstance3D, p_materials: Array) -> void:
	for material: Variant in p_materials:
		if Lib.translucent_shadow(material as Material, p_node.cast_shadow):
			_need_approximation("translucent_shadow", str(_scene.get_path_to(p_node)))
			return


func _need_approximation(p_id: String, p_item: String) -> void:
	if not _approximation_items.has(p_id):
		_approximation_items[p_id] = []
	(_approximation_items[p_id] as Array).append(p_item)


func _add_light(p_light: Light3D) -> void:
	var path := str(_scene.get_path_to(p_light))
	var type := "directional"
	if p_light is OmniLight3D:
		type = "omni"
	elif p_light is SpotLight3D:
		type = "spot"
	elif p_light is AreaLight3D:
		type = "area"
	var transform := p_light.global_transform
	var color := p_light.light_color.srgb_to_linear()
	var entry := {
		"node": path,
		"type": type,
		"transform": Lib.transform_rows(transform),
		"direction": Lib.json_value(-transform.basis.z.normalized()),
		"color_linear": [color.r, color.g, color.b],
		"projector": null,
		"properties": Lib.storage_properties(p_light),
	}
	if p_light.light_projector != null:
		_need_approximation("light_projector", path)
		entry["projector"] = _texture_id(p_light.light_projector, "light_projector")
	if p_light is AreaLight3D:
		var area := p_light as AreaLight3D
		entry["area"] = {
			"size": Lib.json_value(area.area_size),
			"texture": _texture_id(area.area_texture, "area_light") if area.area_texture != null else null,
			"normalize_energy": area.area_normalize_energy,
		}
	_lights.append(entry)
	_light_nodes.append([entry, p_light])
	if p_light is DirectionalLight3D and (p_light as DirectionalLight3D).sky_mode != DirectionalLight3D.SKY_MODE_LIGHT_ONLY:
		var energy := p_light.light_energy
		if bool(ProjectSettings.get_setting("rendering/lights_and_shadows/use_physical_light_units", false)):
			energy *= p_light.light_intensity_lux
		_suns.append({
			"direction": transform.basis.z.normalized(),
			"color": color,
			"energy": energy,
			"size": deg_to_rad(p_light.light_angular_distance),
		})


## The light masks against the layers the placed instances use. A cull mask that lights every
## placed layer is carried. One that lights none of them is an approximation
## (APPROXIMATIONS.light_cull_mask): the UE import's lighting channel does not keep it off the
## Static meshes. One that lights some of them is not exported yet. A shadow caster mask that
## leaves out a placed layer has no UE equivalent (APPROXIMATIONS.shadow_caster_mask).
func _resolve_light_masks() -> void:
	for pair: Array in _light_nodes:
		var entry: Dictionary = pair[0]
		var light: Light3D = pair[1]
		var cull := Lib.mask_effect(light.light_cull_mask, _layers_in_use)
		entry["cull_mask_effect"] = cull
		if cull == "some":
			_unsupported.append("%s: a light cull mask that lights some of the placed layers but not all (not exported yet)" % entry["node"])
		elif cull == "none":
			_need_approximation("light_cull_mask", String(entry["node"]))
		var caster := Lib.mask_effect(light.shadow_caster_mask, _layers_in_use) if light.shadow_enabled else "all"
		entry["caster_mask_effect"] = caster
		if caster != "all":
			_need_approximation("shadow_caster_mask", String(entry["node"]))


func _light_types() -> Dictionary:
	var counts := {}
	for light: Dictionary in _lights:
		counts[light["type"]] = int(counts.get(light["type"], 0)) + 1
	return counts


func _rendering() -> Dictionary:
	var project := {}
	for property: Dictionary in ProjectSettings.get_property_list():
		var name := String(property["name"])
		for prefix in PROJECT_PREFIXES:
			if name.begins_with(prefix):
				project[name] = Lib.json_value(ProjectSettings.get_setting(name))
				break
	var rows := {}
	for key: String in RECORD_ROWS:
		if project.has(key):
			rows[key] = RECORD_ROWS[key]
	return {"project": project, "record_rows": rows}


## Calibration markers for the UE import: small boxes whose vertices sit at the origin and 1 m
## along +X, +Y and +Z, so the importer's axis and unit conversion can be read from their bounds.
func _axes_fixture() -> Array:
	var markers := {"axis_o": Vector3.ZERO, "axis_x": Vector3.RIGHT, "axis_y": Vector3.UP, "axis_z": Vector3.BACK}
	var entries: Array = []
	for name: String in markers:
		var box := BoxMesh.new()
		box.size = Vector3(0.1, 0.1, 0.1)
		var arrays := box.get_mesh_arrays()
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		for index in vertices.size():
			vertices[index] += markers[name]
		arrays[Mesh.ARRAY_VERTEX] = vertices
		var mesh := ArrayMesh.new()
		mesh.resource_name = name
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
		entries.append({"name": name, "mesh": mesh})
	return entries


func _write_glb(p_path: String, p_entries: Array) -> String:
	var root_node := Node3D.new()
	root_node.name = "library"
	for entry: Dictionary in p_entries:
		var instance := MeshInstance3D.new()
		instance.name = String(entry["name"])
		instance.mesh = entry["mesh"]
		root_node.add_child(instance)
		instance.owner = root_node
	var document := GLTFDocument.new()
	var state := GLTFState.new()
	var error := document.append_from_scene(root_node, state)
	if error == OK:
		error = document.write_to_filesystem(state, p_path)
	root_node.free()
	if error != OK:
		return "glTF export of %s failed (%s)" % [p_path.get_file(), error_string(error)]
	return ""


func _write_text(p_name: String, p_text: String) -> String:
	return _write_bytes(p_name, p_text.to_utf8_buffer())


func _write_bytes(p_name: String, p_bytes: PackedByteArray) -> String:
	var file := FileAccess.open(_out.path_join(p_name), FileAccess.WRITE)
	if file == null:
		return "cannot write %s (%s)" % [p_name, error_string(FileAccess.get_open_error())]
	file.store_buffer(p_bytes)
	file.close()
	return ""
