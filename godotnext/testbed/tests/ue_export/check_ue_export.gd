extends SceneTree
## Checks two exports of the overlap-heavy kitbash made by tools/ue_export/ue_export.gd (#235).
## run_ue_export_check.sh makes them; this script runs headless:
##
##   godot --headless --path godotnext/testbed --script res://tests/ue_export/check_ue_export.gd -- <export A> <export B>
##
## It checks that the two exports are byte-identical apart from export_manifest.json, that the
## manifest checksums match, that scene.json follows ue_export_schema.md, that every rock
## placement equals the seeded layout's plan (so the MultiMesh half kept its transforms), that
## library.glb and axes.glb re-import with the triangle counts, bounds and marker positions
## scene.json states, that every camera row is CaptureCam's pose, and that sky.hdr holds the
## procedural sky. Prints GODOTNEXT_RESULT and exits 1 on any failure.

const Lib := preload("res://tools/ue_export/ue_export_lib.gd")
const Layout := preload("res://scenes/swrt_kitbash/layout.gd")

const SCENE := "res://scenes/swrt_kitbash/swrt_kitbash.tscn"
const FILES: Array[String] = ["library.glb", "axes.glb", "scene.json", "camera_track.csv", "sky.hdr"]
const EPSILON := 1e-5

var _failures: Array[String] = []
var _checks := 0


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() != 2:
		printerr("check_ue_export.gd: pass two export directories after --")
		quit(2)
		return
	_test_lib()
	var first := String(args[0])
	var second := String(args[1])
	_check_determinism(first, second)
	var scene := _check_manifest_and_load(first)
	if not scene.is_empty():
		_check_schema(scene)
		_check_kitbash(scene)
		_check_library(first, scene)
		_check_axes(first)
		_check_camera_track(first, scene)
		_check_sky(first, scene)
	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({"test": "ue_export", "ok": ok, "checks": _checks, "failures": _failures.size()}))
	for failure in _failures:
		printerr("check_ue_export.gd: FAIL " + failure)
	quit(0 if ok else 1)


func _check(p_condition: bool, p_message: String) -> void:
	_checks += 1
	if not p_condition:
		_failures.append(p_message)


func _test_lib() -> void:
	_test_refusals()
	var transform := Transform3D(Basis.from_euler(Vector3(0.3, -1.2, 2.0)) * Basis.from_scale(Vector3.ONE * 0.75), Vector3(1.5, -2.0, 3.25))
	var back := Lib.rows_to_transform(Lib.transform_rows(transform))
	_check(back.is_equal_approx(transform), "transform rows round-trip")
	var rows := Lib.transform_rows(Transform3D(Basis(), Vector3(4.0, 5.0, 6.0)))
	_check(rows == [1.0, 0.0, 0.0, 4.0, 0.0, 1.0, 0.0, 5.0, 0.0, 0.0, 1.0, 6.0], "transform rows are row-major with the origin last")
	_check(Lib.json_value(Color(0.25, 0.5, 1.0, 1.0)) == [0.25, 0.5, 1.0, 1.0], "a color becomes four numbers")
	_check(Lib.json_value(Vector3i(1, 2, 3)) == [1, 2, 3], "a Vector3i becomes three numbers")
	_check(Lib.json_value(null) == null, "null stays null")
	var direction := Lib.panorama_direction(128, 0, 512, 256)
	_check(direction.y > 0.99, "the top panorama row looks up")
	direction = Lib.panorama_direction(0, 128, 512, 256)
	_check(absf(direction.y) < 0.02 and direction.z < -0.99, "the left edge of the horizon row looks down -Z")
	var pixels := PackedFloat32Array()
	for index in 16 * 2:
		pixels.append_array(PackedFloat32Array([0.001 * index, 0.5, 3.0 + index]))
	var decoded := Lib.decode_hdr(Lib.encode_hdr(16, 2, pixels))
	_check(not decoded.has("error") and int(decoded["width"]) == 16 and int(decoded["height"]) == 2, "an HDR round-trips its size")
	if not decoded.has("error"):
		var worst := 0.0
		var largest := 0.0
		for index in pixels.size():
			largest = maxf(largest, pixels[index])
		for index in pixels.size():
			# RGBE keeps 8 mantissa bits of the largest channel of each pixel.
			worst = maxf(worst, absf(decoded["pixels"][index] - pixels[index]) / largest)
		_check(worst < 0.01, "an HDR round-trips its values within RGBE precision (worst %f)" % worst)


## The content the format does not carry yet is refused, not dropped (ue_export_schema.md).
func _test_refusals() -> void:
	var plain := StandardMaterial3D.new()
	plain.diffuse_mode = BaseMaterial3D.DIFFUSE_LAMBERT
	_check(Lib.material_refusals(plain).is_empty(), "an opaque Lambert constant material is carried")
	var burley := StandardMaterial3D.new()
	_check(Lib.material_refusals(burley).size() == 1, "Burley diffuse (the StandardMaterial3D default) is refused")
	var vertex_albedo := plain.duplicate() as StandardMaterial3D
	vertex_albedo.vertex_color_use_as_albedo = true
	_check(Lib.material_refusals(vertex_albedo).size() == 1, "vertex colors as albedo are refused")
	var transparent := plain.duplicate() as StandardMaterial3D
	transparent.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_check(Lib.material_refusals(transparent).size() == 1, "transparency is refused")
	var scissor := plain.duplicate() as StandardMaterial3D
	scissor.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	_check(Lib.material_refusals(scissor).size() == 1, "alpha scissor is refused")
	var front := plain.duplicate() as StandardMaterial3D
	front.cull_mode = BaseMaterial3D.CULL_FRONT
	_check(Lib.material_refusals(front).size() == 1, "front-face culling is refused")
	var two_sided := plain.duplicate() as StandardMaterial3D
	two_sided.cull_mode = BaseMaterial3D.CULL_DISABLED
	_check(Lib.material_refusals(two_sided).is_empty(), "two-sided culling is carried as a property")
	var environment := Environment.new()
	environment.sky = Sky.new()
	var sky := ProceduralSkyMaterial.new()
	environment.sky.sky_material = sky
	var camera := Camera3D.new()
	_check(Lib.environment_refusals(environment, camera).is_empty(), "a procedural sky without a cover and a camera without an Environment are carried")
	sky.sky_cover = ImageTexture.create_from_image(Image.create_empty(4, 4, false, Image.FORMAT_RGBA8))
	_check(Lib.environment_refusals(environment, camera).size() == 1, "a sky cover texture is refused")
	sky.sky_cover = null
	camera.environment = Environment.new()
	_check(Lib.environment_refusals(environment, camera).size() == 1, "an Environment on the camera is refused")
	_check(Lib.environment_refusals(null, null).is_empty(), "no environment and no camera refuse nothing")
	camera.free()


func _check_determinism(p_first: String, p_second: String) -> void:
	for file_name in FILES:
		var a := FileAccess.get_sha256(p_first.path_join(file_name))
		var b := FileAccess.get_sha256(p_second.path_join(file_name))
		_check(not a.is_empty() and a == b, "%s is the same in both exports" % file_name)


func _check_manifest_and_load(p_dir: String) -> Dictionary:
	var manifest: Variant = _read_json(p_dir.path_join("export_manifest.json"))
	_check(manifest is Dictionary, "export_manifest.json parses")
	if not (manifest is Dictionary):
		return {}
	_check(int(manifest["schema"]) == Lib.SCHEMA, "manifest schema %d" % Lib.SCHEMA)
	_check(String(manifest["scene"]) == SCENE, "manifest names the kitbash scene")
	_check(String(manifest["scene_sha256"]) == FileAccess.get_sha256(SCENE), "manifest has the scene's SHA-256")
	var files: Dictionary = manifest["files"]
	for file_name in FILES:
		_check(files.has(file_name) and String(files[file_name]) == FileAccess.get_sha256(p_dir.path_join(file_name)), "manifest checksum of %s" % file_name)
	var scene: Variant = _read_json(p_dir.path_join("scene.json"))
	_check(scene is Dictionary, "scene.json parses")
	return scene if scene is Dictionary else {}


## The keys and types ue_export_schema.md promises.
func _check_schema(p_scene: Dictionary) -> void:
	var top := {
		"schema": TYPE_FLOAT, "scene": TYPE_STRING, "units": TYPE_DICTIONARY, "fps": TYPE_FLOAT,
		"warmup": TYPE_FLOAT, "frames": TYPE_FLOAT, "camera": TYPE_DICTIONARY, "meshes": TYPE_ARRAY,
		"materials": TYPE_ARRAY, "instances": TYPE_ARRAY, "lights": TYPE_ARRAY,
		"environment": TYPE_DICTIONARY, "rendering": TYPE_DICTIONARY, "checks": TYPE_DICTIONARY,
	}
	var before := _failures.size()
	for key: String in top:
		_check(p_scene.has(key) and typeof(p_scene[key]) == top[key], "scene.json has %s of the right type" % key)
	if _failures.size() > before:
		return
	_check(p_scene["units"] == {"length": "m", "up": "+Y", "handedness": "right", "camera_forward": "-Z", "light_forward": "-Z"}, "units")
	var camera: Dictionary = p_scene["camera"]
	for key in ["node", "track", "fov_deg", "fov_axis", "near", "far", "projection"]:
		_check(camera.has(key), "camera has %s" % key)
	var mesh_ids := {}
	for mesh: Dictionary in p_scene["meshes"]:
		for key in ["id", "gltf_node", "surfaces", "triangles", "vertices", "aabb", "nextgeo"]:
			_check(mesh.has(key), "mesh has %s" % key)
		mesh_ids[String(mesh["id"])] = mesh
	var material_ids := {}
	for material: Dictionary in p_scene["materials"]:
		for key in ["id", "class", "albedo_linear", "emission_linear", "properties"]:
			_check(material.has(key), "material has %s" % key)
		material_ids[String(material["id"])] = true
	var placements: Array = []
	var triangles := 0
	var node_count := 0
	var multimesh_count := 0
	for instance: Dictionary in p_scene["instances"]:
		var kind := String(instance["kind"])
		_check(kind == "node" or kind == "multimesh", "instance kind is node or multimesh")
		_check(mesh_ids.has(String(instance["mesh"])), "instance mesh %s exists" % instance["mesh"])
		for material_id: Variant in instance["materials"]:
			_check(material_ids.has(String(material_id)), "instance material %s exists" % material_id)
		_check((instance["transform"] as Array).size() == 12, "instance transform has 12 numbers")
		var mesh_triangles := int(mesh_ids[String(instance["mesh"])]["triangles"]) if mesh_ids.has(String(instance["mesh"])) else 0
		if kind == "node":
			placements.append(Lib.rows_to_transform(instance["transform"]))
			triangles += mesh_triangles
			node_count += 1
		else:
			_check((instance["instances"] as Array).size() == int(instance["count"]), "multimesh count matches its rows")
			for rows: Array in instance["instances"]:
				placements.append(Lib.rows_to_transform(rows))
				triangles += mesh_triangles
				multimesh_count += 1
	for light: Dictionary in p_scene["lights"]:
		for key in ["node", "type", "transform", "direction", "color_linear", "properties"]:
			_check(light.has(key), "light has %s" % key)
	var checks: Dictionary = p_scene["checks"]
	_check(int(checks["node_instances"]) == node_count, "checks.node_instances")
	_check(int(checks["multimesh_instances"]) == multimesh_count, "checks.multimesh_instances")
	_check(int(checks["instances_total"]) == placements.size(), "checks.instances_total")
	_check(int(checks["triangles_total"]) == triangles, "checks.triangles_total")
	_check(int(checks["meshes"]) == mesh_ids.size() and int(checks["materials"]) == material_ids.size(), "checks.meshes and checks.materials")
	_check(String(checks["instances_sha256"]) == Lib.transforms_sha256(placements), "checks.instances_sha256 matches the instance rows")
	_check(int(checks["camera_rows"]) == int(p_scene["frames"]), "one camera row per frame")
	var rows: Dictionary = p_scene["rendering"]["record_rows"]
	_check(rows.has("rendering/global_illumination/nextgi/hardware_ray_tracing"), "rendering names the settings-record row of hardware_ray_tracing")


## The kitbash's own numbers: 2,400 rocks from the seeded plan plus the ground, a sun and a spot.
func _check_kitbash(p_scene: Dictionary) -> void:
	var plan: Dictionary = Layout.plan(Layout.DEFAULT_INSTANCES, Layout.SEED)
	var positions: PackedVector3Array = plan["positions"]
	var rotations: PackedVector3Array = plan["rotations"]
	var scales: PackedFloat32Array = plan["scales"]
	var variants: PackedInt32Array = plan["variants"]
	var count := positions.size()
	var expected: Array[Transform3D] = []
	for index in count:
		var basis := Basis.from_euler(rotations[index]) * Basis.from_scale(Vector3.ONE * float(scales[index]))
		expected.append(Transform3D(basis, positions[index]))
	var checks: Dictionary = p_scene["checks"]
	_check(int(checks["instances_total"]) == count + 1, "2,400 rocks and the ground (%d)" % int(checks["instances_total"]))
	_check(int(checks["multimesh_instances"]) == count / 2 and int(checks["multimesh_nodes"]) == Layout.ROCK_VARIANTS, "1,200 MultiMesh rocks in 4 MultiMeshes")
	_check(int(checks["lights"]) == 2, "a sun and the off-screen spot")
	var exported: Array = []
	exported.resize(count)
	var mismatched := 0
	var cursors := {}
	var by_variant := {}
	for index in range(count / 2, count):
		var variant := int(variants[index])
		if not by_variant.has(variant):
			by_variant[variant] = []
		(by_variant[variant] as Array).append(index)
	var ground := 0
	for instance: Dictionary in p_scene["instances"]:
		var node := String(instance["node"])
		if instance["kind"] == "node" and node.begins_with("Instances/Meshes/Rock"):
			var index := int(node.trim_prefix("Instances/Meshes/Rock"))
			exported[index] = Lib.rows_to_transform(instance["transform"])
		elif instance["kind"] == "multimesh":
			var variant := int(node.trim_prefix("Instances/MultiMeshes/RockVariant"))
			var indices: Array = by_variant.get(variant, [])
			_check(indices.size() == int(instance["count"]), "MultiMesh variant %d has the plan's count" % variant)
			for row in mini(indices.size(), (instance["instances"] as Array).size()):
				exported[int(indices[row])] = Lib.rows_to_transform(instance["instances"][row])
		elif node == "Ground":
			ground += 1
			_check(Lib.rows_to_transform(instance["transform"]).is_equal_approx(Transform3D(Basis(), Vector3(0.0, -0.1, 0.0))), "the ground sits at y = -0.1")
	_check(ground == 1, "one ground instance")
	var placed := PackedVector3Array()
	for index in count:
		if not (exported[index] is Transform3D):
			mismatched += 1
			placed.append(Vector3())
			continue
		var transform: Transform3D = exported[index]
		placed.append(transform.origin)
		if not _transforms_close(transform, expected[index]):
			mismatched += 1
	_check(mismatched == 0, "every rock placement equals the seeded plan (%d differ)" % mismatched)
	_check(Layout.positions_sha256(placed) == Layout.positions_sha256(positions), "the placements hash to the scene's positions_sha256")


func _check_library(p_dir: String, p_scene: Dictionary) -> void:
	var nodes := _gltf_meshes(p_dir.path_join("library.glb"))
	_check(nodes.size() == (p_scene["meshes"] as Array).size(), "library.glb has one mesh node per scene mesh (%d)" % nodes.size())
	for mesh: Dictionary in p_scene["meshes"]:
		var name := String(mesh["gltf_node"])
		_check(nodes.has(name), "library.glb has node %s" % name)
		if not nodes.has(name):
			continue
		var imported: Dictionary = nodes[name]
		_check(int(imported["triangles"]) == int(mesh["triangles"]), "%s keeps %d triangles (%d)" % [name, int(mesh["triangles"]), int(imported["triangles"])])
		var aabb: AABB = imported["aabb"]
		var stated := AABB(_vector(mesh["aabb"]["position"]), _vector(mesh["aabb"]["size"]))
		_check(aabb.position.is_equal_approx(stated.position) and aabb.size.is_equal_approx(stated.size), "%s keeps its bounds" % name)
		_check(Transform3D(imported["transform"]).is_equal_approx(Transform3D.IDENTITY), "%s sits at the origin" % name)


func _check_axes(p_dir: String) -> void:
	var nodes := _gltf_meshes(p_dir.path_join("axes.glb"))
	var expected := {"axis_o": Vector3.ZERO, "axis_x": Vector3.RIGHT, "axis_y": Vector3.UP, "axis_z": Vector3.BACK}
	for name: String in expected:
		_check(nodes.has(name), "axes.glb has %s" % name)
		if nodes.has(name):
			var center: Vector3 = (nodes[name]["aabb"] as AABB).get_center()
			_check(center.is_equal_approx(expected[name]), "%s is centered at %s (%s)" % [name, expected[name], center])


func _check_camera_track(p_dir: String, p_scene: Dictionary) -> void:
	var shell := (load(SCENE) as PackedScene).instantiate()
	shell.set_meta("skip_runtime", true)
	var camera := shell.get_node("CaptureCam") as Camera3D
	var pose := camera.transform.orthonormalized()
	var file := FileAccess.open(p_dir.path_join("camera_track.csv"), FileAccess.READ)
	_check(file != null, "camera_track.csv opens")
	if file == null:
		shell.free()
		return
	_check(file.get_line() == "frame,px,py,pz,qx,qy,qz,qw,fov_deg,near,far", "camera_track.csv header")
	var rows := 0
	var off := 0
	while not file.eof_reached():
		var line := file.get_line()
		if line.is_empty():
			continue
		var cells := line.split(",")
		if cells.size() != 11 or int(cells[0]) != rows:
			off += 1
			rows += 1
			continue
		var q := Quaternion(float(cells[4]), float(cells[5]), float(cells[6]), float(cells[7]))
		var row_pose := Transform3D(Basis(q), Vector3(float(cells[1]), float(cells[2]), float(cells[3])))
		if not _transforms_close(row_pose, pose, 1e-4) or absf(float(cells[8]) - camera.fov) > 1e-6:
			off += 1
		rows += 1
	_check(rows == int(p_scene["frames"]), "camera_track.csv has %d rows (%d)" % [int(p_scene["frames"]), rows])
	_check(off == 0, "every camera row is CaptureCam's pose and fov (%d differ)" % off)
	shell.free()


func _check_sky(p_dir: String, p_scene: Dictionary) -> void:
	var sky_entry: Dictionary = p_scene["environment"]["sky"]
	_check(String(sky_entry.get("panorama", "")) == "sky.hdr" and String(sky_entry.get("panorama_projection", "")) == "godot_equirect", "the environment names sky.hdr")
	var decoded := Lib.decode_hdr(FileAccess.get_file_as_bytes(p_dir.path_join("sky.hdr")))
	_check(not decoded.has("error"), "sky.hdr decodes")
	if decoded.has("error"):
		return
	var width := int(decoded["width"])
	var height := int(decoded["height"])
	_check(width == Lib.SKY_WIDTH and height == Lib.SKY_HEIGHT, "sky.hdr is %dx%d" % [Lib.SKY_WIDTH, Lib.SKY_HEIGHT])
	var shell := (load(SCENE) as PackedScene).instantiate()
	var environment := (shell.get_node("WorldEnvironment") as WorldEnvironment).environment
	var sun := shell.get_node("Sun") as DirectionalLight3D
	var suns := [{
		"direction": sun.transform.basis.z.normalized(),
		"color": sun.light_color.srgb_to_linear(),
		"energy": sun.light_energy,
		"size": deg_to_rad(sun.light_angular_distance),
	}]
	var sky := environment.sky.sky_material as ProceduralSkyMaterial
	var worst := 0.0
	for sample: Vector2i in [Vector2i(0, 0), Vector2i(100, 60), Vector2i(256, 127), Vector2i(300, 128), Vector2i(511, 255), Vector2i(64, 90)]:
		var expected := Lib.procedural_sky_radiance(sky, suns, Lib.panorama_direction(sample.x, sample.y, width, height))
		var base := (sample.y * width + sample.x) * 3
		var pixels: PackedFloat32Array = decoded["pixels"]
		var peak := maxf(expected.r, maxf(expected.g, expected.b))
		for channel in 3:
			worst = maxf(worst, absf(pixels[base + channel] - [expected.r, expected.g, expected.b][channel]) / peak)
	_check(worst < 0.01, "sky.hdr holds the procedural sky (worst relative error %f)" % worst)
	var zenith := Lib.procedural_sky_radiance(sky, [], Vector3.UP)
	_check(zenith.is_equal_approx(sky.sky_top_color.srgb_to_linear()), "the zenith is the linear sky top color")
	shell.free()


## Mesh nodes of a glTF file: name -> {triangles, aabb, transform}.
func _gltf_meshes(p_path: String) -> Dictionary:
	var document := GLTFDocument.new()
	var state := GLTFState.new()
	var result := {}
	if document.append_from_file(p_path, state) != OK:
		_check(false, "%s imports" % p_path.get_file())
		return result
	var root_node := document.generate_scene(state)
	var stack: Array[Node] = [root_node]
	while not stack.is_empty():
		var node: Node = stack.pop_back()
		if node is MeshInstance3D and (node as MeshInstance3D).mesh != null:
			var mesh := (node as MeshInstance3D).mesh
			var triangles := 0
			for surface in mesh.get_surface_count():
				var arrays := mesh.surface_get_arrays(surface)
				var indices: Variant = arrays[Mesh.ARRAY_INDEX]
				triangles += (indices as PackedInt32Array).size() / 3 if indices != null else (arrays[Mesh.ARRAY_VERTEX] as PackedVector3Array).size() / 3
			result[String(node.name)] = {"triangles": triangles, "aabb": mesh.get_aabb(), "transform": (node as Node3D).transform}
		for child in node.get_children():
			stack.append(child)
	root_node.free()
	return result


func _transforms_close(p_a: Transform3D, p_b: Transform3D, p_epsilon: float = EPSILON) -> bool:
	for axis in 3:
		if not (p_a.basis[axis] - p_b.basis[axis]).length() <= p_epsilon:
			return false
	return (p_a.origin - p_b.origin).length() <= p_epsilon


func _vector(p_value: Array) -> Vector3:
	return Vector3(float(p_value[0]), float(p_value[1]), float(p_value[2]))


func _read_json(p_path: String) -> Variant:
	var text := FileAccess.get_file_as_string(p_path)
	if text.is_empty():
		return null
	return JSON.parse_string(text)
