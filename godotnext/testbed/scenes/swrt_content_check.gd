extends SceneTree
## Headless checks for the #181 SWRT content contracts: the leak-room
## wall thicknesses, the kitbash layout (count, 30 m bound, overlap,
## determinism, spawn), the runtime-mesh build/move frames and its
## built triangle count, the SDF mesh set census, the OBJ manifest
## hashes, and analytic inside/outside samples of the 512-ray sign
## reference. No window and no external tool.
##
## [code]godot --headless --path godotnext/testbed --script res://scenes/swrt_content_check.gd[/code]

const LeakRoom := preload("res://generators/build_swrt_leak_room.gd")
const KitbashLayout = preload("res://scenes/swrt_kitbash/layout.gd")
const RuntimeSpec = preload("res://scenes/swrt_runtime_mesh/spec.gd")
const SdfMeshes := preload("res://generators/sdf_mesh_library.gd")

# Analytic sign samples for the closed meshes: [point, expected_inside].
const SIGN_SAMPLES := {
	"box": [[Vector3(0, 0, 0), true]],
	"sphere": [[Vector3(0, 0, 0), true]],
	"cylinder": [[Vector3(0, 0, 0), true]],
	"cone": [[Vector3(0, -0.3, 0), true]],
	"torus": [[Vector3(0.65, 0, 0), true], [Vector3(0, 0, 0), false]],
	"capsule": [[Vector3(0, 0, 0), true]],
	"tetrahedron": [[Vector3(0, 0, 0), true]],
	"octahedron": [[Vector3(0, 0, 0), true]],
	"icosahedron": [[Vector3(0, 0, 0), true]],
	"hexagonal_prism": [[Vector3(0, 0, 0), true]],
	"l_prism": [[Vector3(-0.4, 0.4, 0), true], [Vector3(0.4, 0.4, 0), false]],
	"wedge": [[Vector3(-0.3, -0.2, 0), true], [Vector3(0.4, 0.3, 0), false]],
}

var _failures: Array[String] = []


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	_check_leak_rooms()
	_check(SdfMeshes.contract_error().is_empty(), String(SdfMeshes.contract_error()))
	_check(KitbashLayout.contract_error().is_empty(), String(KitbashLayout.contract_error()))
	_check_kitbash_spawn()
	_check(RuntimeSpec.contract_error().is_empty(), String(RuntimeSpec.contract_error()))
	_check_runtime_mesh()
	_check_manifest()
	_check_sign_reference()
	if _failures.is_empty():
		print("swrt_content_check: ok")
		quit(0)
	else:
		for failure in _failures:
			push_error("swrt_content_check: " + failure)
		quit(1)


func _check_leak_rooms() -> void:
	for variant in ["10cm", "20cm"]:
		var built: Dictionary = LeakRoom.build(variant)
		var root: Node3D = built["root"]
		var expected := 0.10 if variant == "10cm" else 0.20
		_check(is_equal_approx(float(root.get_meta("wall_thickness_m", -1.0)), expected),
				"leak room %s wall metadata" % variant)
		_check(root.get_node_or_null("Sun") != null, "leak room %s has a sun" % variant)
		_check(root.get_node_or_null("EmissivePanel") != null, "leak room %s has an emissive panel" % variant)
		_check(root.get_node_or_null("ExteriorCam") != null, "leak room %s has ExteriorCam" % variant)
		root.free()


func _check_kitbash_spawn() -> void:
	var planned: Dictionary = KitbashLayout.plan(240, KitbashLayout.SEED)
	var host := Node3D.new()
	get_root().add_child(host)
	var spawned: Dictionary = KitbashLayout.spawn(host, planned)
	_check(int(spawned["mesh_nodes"]) == 120, "kitbash spawn mesh nodes at 240")
	_check(int(spawned["multimesh_instances"]) == 120, "kitbash spawn multimesh at 240")
	host.free()


func _check_runtime_mesh() -> void:
	var mesh := RuntimeSpec.build_mesh(null)
	var arrays: Array = mesh.surface_get_arrays(0)
	var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
	_check(indices.size() / 3 == 20000, "runtime mesh built triangle count is %d" % (indices.size() / 3))
	var aabb := mesh.get_aabb()
	_check(absf(aabb.size.x - 2.0) < 0.02 and absf(aabb.size.z - 2.0) < 0.02,
			"runtime mesh AABB is %s, expected about 2 m across" % str(aabb.size))


func _check_manifest() -> void:
	var path := "res://assets/sdf_import_set/manifest.json"
	_check(FileAccess.file_exists(path), "sdf manifest exists")
	if not FileAccess.file_exists(path):
		return
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(path))
	_check(typeof(parsed) == TYPE_DICTIONARY, "sdf manifest parses")
	if typeof(parsed) != TYPE_DICTIONARY:
		return
	var document: Dictionary = parsed
	var meshes: Array = document.get("meshes", [])
	_check(meshes.size() == 16, "sdf manifest lists %d meshes" % meshes.size())
	for entry_value in meshes:
		var entry: Dictionary = entry_value
		var obj_path := "res://assets/sdf_import_set/" + String(entry["file"])
		if not FileAccess.file_exists(obj_path):
			_check(false, "sdf asset missing: " + obj_path)
			continue
		var text := FileAccess.get_file_as_string(obj_path)
		_check(text.sha256_text() == String(entry["obj_sha256"]),
				"sdf asset hash matches manifest: " + String(entry["file"]))


func _check_sign_reference() -> void:
	var path := "res://assets/sdf_import_set/sign_reference.json"
	_check(FileAccess.file_exists(path), "sign reference exists")
	if not FileAccess.file_exists(path):
		return
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(path))
	_check(typeof(parsed) == TYPE_DICTIONARY, "sign reference parses")
	if typeof(parsed) != TYPE_DICTIONARY:
		return
	var document: Dictionary = parsed
	_check(int(document.get("rays_per_voxel", 0)) == 512, "sign reference ray count")
	var by_name := {}
	for entry_value in document.get("meshes", []):
		var entry: Dictionary = entry_value
		by_name[String(entry["name"])] = entry
		var dims: Array = entry["dims"]
		var inside: String = entry["inside"]
		var voxels := int(dims[0]) * int(dims[1]) * int(dims[2])
		_check(inside.length() == voxels, "%s sign grid length" % String(entry["name"]))
		# The padded corner voxel is outside every mesh, open or closed.
		_check(inside[0] == "0", "%s sign grid corner is outside" % String(entry["name"]))
	for name in SIGN_SAMPLES:
		_check(by_name.has(name), "sign reference has " + name)
		if not by_name.has(name):
			continue
		var entry: Dictionary = by_name[name]
		for sample_value in SIGN_SAMPLES[name]:
			var sample: Array = sample_value
			var point: Vector3 = sample[0]
			_check(_sample_inside(entry, point) == bool(sample[1]),
					"%s sign at %s" % [name, str(point)])


func _sample_inside(p_entry: Dictionary, p_point: Vector3) -> bool:
	var origin: Array = p_entry["grid_origin"]
	var dims: Array = p_entry["dims"]
	var voxel := float(p_entry["voxel_size_m"])
	var ix := int(floor((p_point.x - float(origin[0])) / voxel))
	var iy := int(floor((p_point.y - float(origin[1])) / voxel))
	var iz := int(floor((p_point.z - float(origin[2])) / voxel))
	ix = clampi(ix, 0, int(dims[0]) - 1)
	iy = clampi(iy, 0, int(dims[1]) - 1)
	iz = clampi(iz, 0, int(dims[2]) - 1)
	var index := ix + int(dims[0]) * (iy + int(dims[1]) * iz)
	return String(p_entry["inside"])[index] == "1"


func _check(p_ok: bool, p_message: String) -> void:
	if p_ok:
		return
	if p_message.is_empty():
		p_message = "check failed"
	_failures.append(p_message)
	print("FAIL: " + p_message)
