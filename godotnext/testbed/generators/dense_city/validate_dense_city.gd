extends SceneTree
## Dense City Blocks post-import validator (#63).
##
## Run after generating and importing, from the repository root:
## [code]godot --headless --path godotnext/testbed --import[/code]
## [code]godot --headless --path godotnext/testbed --script res://generators/dense_city/validate_dense_city.gd -- --variant=virtual[/code]
##
## Checks the generated output against the tracked [code]expected.json[/code]:
## output SHA-256 ([code].import[/code] files are checked by parameter
## instead, because the importer rewrites them), the imported scene's
## MeshInstance3D / light / camera / path counts, unique and instanced
## triangle recounts from the imported meshes, the reachable-cluster lower
## bound, kit materials and seams, sweep totals within 5%, baseline LOD
## presence versus virtual LOD absence, and — for [code]lod_baseline[/code] —
## that every instance transform in its scene text is byte-identical to the
## virtual scene's. Exits non-zero on any failure. This is a counter check
## (§22 build-first); it measures no timing or quality gate.

const EXPECTED_PATH := "res://generators/dense_city/expected.json"

var _variant := "virtual"
var _out := "res://generated/dense_city"
var _failures: Array[String] = []
var _expected := {}


func _init() -> void:
	_run.call_deferred()


func _fail(p_message: String) -> void:
	_failures.append(p_message)
	printerr("validate_dense_city FAIL: " + p_message)


func _check(p_condition: bool, p_message: String) -> bool:
	if not p_condition:
		_fail(p_message)
	return p_condition


func _run() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--variant="):
			_variant = arg.trim_prefix("--variant=")
		elif arg.begins_with("--out="):
			_out = arg.trim_prefix("--out=")
		else:
			_fail("unknown argument '%s'." % arg)
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(EXPECTED_PATH))
	if not _check(typeof(parsed) == TYPE_DICTIONARY, "expected.json does not parse."):
		quit(1)
		return
	_expected = parsed
	var variants: Dictionary = _expected.get("variants", {})
	if not _check(variants.has(_variant), "expected.json has no entry for variant '%s'." % _variant):
		quit(1)
		return
	var entry: Dictionary = variants[_variant]
	_check_output_hashes(entry)
	match _variant:
		"virtual", "lod_baseline", "micro_detail":
			_check_city(entry, _variant == "lod_baseline")
			if _variant == "lod_baseline":
				_check_baseline_transforms(variants)
			if _variant == "micro_detail":
				_check(int(entry.get("subdivide_facades", 0)) >= 2,
						"micro_detail subdivide_facades is %s, expected at least 2." % str(entry.get("subdivide_facades", 0)))
				_check(float(entry.get("subpixel_fraction_estimate", 0.0)) >= 0.30,
						"micro_detail sub-pixel fraction is %s, expected at least 0.30." % str(entry.get("subpixel_fraction_estimate", 0.0)))
		"sweep_10m", "sweep_100m", "sweep_1b":
			_check_city(entry, false)
		"kit":
			_check_kit(entry)
		"stress_10m":
			_check_stress(entry)
		_:
			_fail("unknown variant '%s'." % _variant)
	if _failures.is_empty():
		print("validate_dense_city: variant %s OK." % _variant)
		quit(0)
	else:
		for failure in _failures:
			push_error("validate_dense_city: " + failure)
		quit(1)


func _sha256(p_res_path: String) -> String:
	var file := FileAccess.open(p_res_path, FileAccess.READ)
	if file == null:
		return ""
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	while not file.eof_reached():
		context.update(file.get_buffer(1 << 20))
	file.close()
	return context.finish().hex_encode()


func _check_output_hashes(p_entry: Dictionary) -> void:
	var outputs: Dictionary = p_entry.get("outputs", {})
	var keys: Array = outputs.keys()
	keys.sort()
	for key in keys:
		var relative := String(key)
		var path := _out + "/" + relative
		if relative.ends_with(".import"):
			_check_import_params(path, relative)
			continue
		if not _check(FileAccess.file_exists(path), "%s: output missing." % relative):
			continue
		var actual := _sha256(path)
		_check(actual == String(outputs[key]),
				"%s: SHA-256 %s, expected %s (re-run the generator, then re-import)." % [relative, actual, String(outputs[key])])


## The importer rewrites [.import] files (adding uid/remap and dropping
## unknown keys), so hash equality cannot hold after an import; the
## generated parameters that must survive are checked directly instead.
func _check_import_params(p_path: String, p_relative: String) -> void:
	if not _check(FileAccess.file_exists(p_path), "%s: import file missing." % p_relative):
		return
	var text := FileAccess.get_file_as_string(p_path)
	var baseline := p_relative.begins_with("kit_lod/")
	if p_relative.ends_with(".glb.import"):
		_check(text.contains("meshes/generate_lods=" + ("true" if baseline else "false")),
				"%s: generate_lods mismatch." % p_relative)
		_check(text.contains("meshes/create_shadow_meshes=" + ("true" if baseline else "false")),
				"%s: create_shadow_meshes mismatch." % p_relative)
	elif p_relative.ends_with(".obj.import"):
		_check(text.contains("generate_lods=false"), "%s: generate_lods mismatch." % p_relative)


## Imported ArrayMesh LODs have no script getter; they are visible through
## the serialized "_surfaces" property (each surface dictionary gains a
## non-empty "lods" list when the importer generated discrete LODs).
func _lod_surface_count(p_mesh: Mesh) -> int:
	if not (p_mesh is ArrayMesh):
		return 0
	var surfaces_prop = (p_mesh as ArrayMesh).get("_surfaces")
	if typeof(surfaces_prop) != TYPE_ARRAY:
		_fail("ArrayMesh %s: cannot inspect LODs (_surfaces unavailable)." % p_mesh.resource_name)
		return 0
	var count := 0
	for surface_value in surfaces_prop:
		var surface: Dictionary = surface_value
		if surface.has("lods") and not (surface["lods"] as Array).is_empty():
			count += 1
	return count


func _mesh_triangles(p_mesh: Mesh) -> Dictionary:
	var triangles := 0
	var vertices := 0
	for surface in range(p_mesh.get_surface_count()):
		var arrays := p_mesh.surface_get_arrays(surface)
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		triangles += indices.size() / 3 if not indices.is_empty() else verts.size() / 3
		vertices += verts.size()
	return {"triangles": triangles, "vertices": vertices}


func _collect(p_node: Node, p_out: Dictionary) -> void:
	if p_node is MeshInstance3D:
		(p_out["mesh_instances"] as Array).append(p_node)
	elif p_node is DirectionalLight3D:
		(p_out["directional"] as Array).append(p_node)
	elif p_node is OmniLight3D:
		(p_out["omni"] as Array).append(p_node)
	elif p_node is SpotLight3D:
		(p_out["spot"] as Array).append(p_node)
	elif p_node is Camera3D:
		(p_out["cameras"] as Array).append(p_node)
	elif p_node is Path3D:
		(p_out["paths"] as Array).append(p_node)
	for child in p_node.get_children():
		_collect(child, p_out)


func _load_scene(p_relative: String) -> Node:
	var path := _out + "/" + p_relative
	var packed := load(path) as PackedScene
	if not _check(packed != null, "%s: cannot load (did the import run?)." % p_relative):
		return null
	return packed.instantiate()


func _check_city(p_entry: Dictionary, p_baseline: bool) -> void:
	var root := _load_scene(String(p_entry["scene"]))
	if root == null:
		return
	var found := {"mesh_instances": [], "directional": [], "omni": [], "spot": [], "cameras": [], "paths": []}
	_collect(root, found)
	var instances: Array = found["mesh_instances"]
	# Every placement becomes one MeshInstance3D, plus the ground plane.
	_check(instances.size() == int(p_entry["instance_count"]) + 1,
			"%s: %d MeshInstance3D, expected %d placements + ground." % [_variant, instances.size(), int(p_entry["instance_count"])])
	var sun_count := 0
	for light_value in found["directional"]:
		var light := light_value as DirectionalLight3D
		if light.shadow_enabled and light.directional_shadow_mode == DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS:
			sun_count += 1
	_check(sun_count == 1, "%s: %d shadowed 4-split DirectionalLight3D, expected 1." % [_variant, sun_count])
	var omni_shadow := 0
	for light_value in found["omni"]:
		if (light_value as OmniLight3D).shadow_enabled:
			omni_shadow += 1
	var spot_shadow := 0
	for light_value in found["spot"]:
		if (light_value as SpotLight3D).shadow_enabled:
			spot_shadow += 1
	_check(omni_shadow == 4, "%s: %d shadowed OmniLight3D, expected 4." % [_variant, omni_shadow])
	_check(spot_shadow == 4, "%s: %d shadowed SpotLight3D, expected 4." % [_variant, spot_shadow])
	var camera_names: Array[String] = []
	for camera_value in found["cameras"]:
		camera_names.append(String((camera_value as Camera3D).name))
	_check(camera_names.has("cam_main") and camera_names.has("cam_aerial"),
			"%s: cameras %s, expected cam_main and cam_aerial." % [_variant, str(camera_names)])
	var paths: Array = found["paths"]
	_check(paths.size() == 1 and (paths[0] as Path3D).curve.point_count >= 2,
			"%s: expected one FlightPath Path3D with a curve." % _variant)

	# Recount geometry from the imported meshes.
	var unique := {}
	var instanced_triangles := 0
	var clusters := 0
	var lod_surfaces := 0
	for instance_value in instances:
		var instance := instance_value as MeshInstance3D
		if instance.name == &"Ground" or instance.mesh == null:
			continue
		var mesh := instance.mesh
		var id := mesh.get_instance_id()
		if not unique.has(id):
			unique[id] = _mesh_triangles(mesh)
			lod_surfaces += _lod_surface_count(mesh)
		var tris := int(unique[id]["triangles"])
		instanced_triangles += tris
		clusters += (tris + 127) / 128
	var unique_triangles := 0
	var unique_vertices := 0
	for id in unique:
		unique_triangles += int(unique[id]["triangles"])
		unique_vertices += int(unique[id]["vertices"])
	if p_entry.has("target_instanced_triangles"):
		# A sweep scene instances a subset of the kit, so its referenced
		# unique pool is at most the kit total recorded in expected.json.
		_check(unique_triangles <= int(p_entry["unique_triangles"]),
				"%s: imported unique triangles %d exceed kit total %d." % [_variant, unique_triangles, int(p_entry["unique_triangles"])])
	else:
		_check(unique_triangles == int(p_entry["unique_triangles"]),
				"%s: imported unique triangles %d, expected %d." % [_variant, unique_triangles, int(p_entry["unique_triangles"])])
	_check(instanced_triangles == int(p_entry["instanced_triangles"]),
			"%s: imported instanced triangles %d, expected %d." % [_variant, instanced_triangles, int(p_entry["instanced_triangles"])])
	_check(clusters == int(p_entry["reachable_clusters_lower_bound"]),
			"%s: imported cluster bound %d, expected %d." % [_variant, clusters, int(p_entry["reachable_clusters_lower_bound"])])
	if p_entry.has("target_instanced_triangles"):
		var target := float(p_entry["target_instanced_triangles"])
		_check(absf(float(instanced_triangles) / target - 1.0) <= 0.05,
				"%s: instanced triangles %d vs target %d, outside 5%%." % [_variant, instanced_triangles, int(target)])
	if p_baseline:
		_check(lod_surfaces > 0, "lod_baseline: no imported surface carries discrete LODs.")
	else:
		_check(lod_surfaces == 0, "%s: %d surfaces carry LODs, expected 0 authored/imported LODs." % [_variant, lod_surfaces])
	print("validate_dense_city: %s mesh_instances=%d unique_triangles=%d unique_vertices=%d instanced=%d clusters>=%d lod_surfaces=%d" % [
		_variant, instances.size(), unique_triangles, unique_vertices, instanced_triangles, clusters, lod_surfaces])
	root.free()


func _scene_transforms(p_relative: String) -> Dictionary:
	var text := FileAccess.get_file_as_string(_out + "/" + p_relative)
	var pattern := RegEx.new()
	pattern.compile("(?m)^\\[node name=\"(Inst\\d+_m\\d+)\"[^\\]]*\\]\ntransform = (Transform3D\\([^)]*\\))")
	var transforms := {}
	for result in pattern.search_all(text):
		transforms[result.get_string(1)] = result.get_string(2)
	return transforms


func _check_baseline_transforms(p_variants: Dictionary) -> void:
	if not _check(p_variants.has("virtual"), "expected.json has no virtual entry to diff transforms against."):
		return
	var virtual_entry: Dictionary = p_variants["virtual"]
	var baseline_entry: Dictionary = p_variants["lod_baseline"]
	_check(String(baseline_entry.get("transform_fingerprint", "")) == String(virtual_entry.get("transform_fingerprint", "")),
			"transform fingerprints differ between virtual and lod_baseline.")
	var a := _scene_transforms(String(virtual_entry["scene"]))
	var b := _scene_transforms(String(baseline_entry["scene"]))
	_check(a.size() == b.size(), "transform counts differ: virtual %d, baseline %d." % [a.size(), b.size()])
	var different := 0
	var keys: Array = a.keys()
	keys.sort()
	for key in keys:
		if not b.has(key) or String(b[key]) != String(a[key]):
			different += 1
	_check(different == 0, "%d instance transforms differ between virtual and lod_baseline scenes." % different)
	print("validate_dense_city: baseline transforms compared=%d different=%d" % [a.size(), different])


func _check_kit(p_entry: Dictionary) -> void:
	var root := _load_scene(String(p_entry["asset"]))
	if root == null:
		return
	var found := {"mesh_instances": [], "directional": [], "omni": [], "spot": [], "cameras": [], "paths": []}
	_collect(root, found)
	var instances: Array = found["mesh_instances"]
	if not _check(instances.size() == 1, "kit: %d MeshInstance3D, expected 1." % instances.size()):
		root.free()
		return
	var mesh := (instances[0] as MeshInstance3D).mesh
	var stats := _mesh_triangles(mesh)
	_check(int(stats["triangles"]) == int(p_entry["triangles"]),
			"kit: imported triangles %d, expected %d." % [int(stats["triangles"]), int(p_entry["triangles"])])
	_check(mesh.get_surface_count() == int(p_entry["surfaces"]),
			"kit: %d surfaces, expected %d." % [mesh.get_surface_count(), int(p_entry["surfaces"])])
	var materials := {}
	var surfaces: Array = []
	for surface in range(mesh.get_surface_count()):
		var material := mesh.surface_get_material(surface)
		if material != null:
			materials[material.get_instance_id()] = true
		surfaces.append(mesh.surface_get_arrays(surface))
	_check(materials.size() == mesh.get_surface_count(),
			"kit: %d distinct materials on %d surfaces." % [materials.size(), mesh.get_surface_count()])
	var owner := {}
	var shared := {}
	for surface in range(surfaces.size()):
		var verts: PackedVector3Array = surfaces[surface][Mesh.ARRAY_VERTEX]
		for vertex in verts:
			var key := Vector3i(roundi(vertex.x * 10000.0), roundi(vertex.y * 10000.0), roundi(vertex.z * 10000.0))
			if owner.has(key):
				if int(owner[key]) != surface:
					shared[int(owner[key])] = true
					shared[surface] = true
			else:
				owner[key] = surface
	_check(shared.size() == surfaces.size(),
			"kit: seams on %d of %d imported surfaces." % [shared.size(), surfaces.size()])
	print("validate_dense_city: kit triangles=%d surfaces=%d materials=%d seams=%d/%d" % [
		int(stats["triangles"]), mesh.get_surface_count(), materials.size(), shared.size(), surfaces.size()])
	root.free()


func _check_stress(p_entry: Dictionary) -> void:
	# The stress import itself is the measured G4-2 run (PQ), so this check
	# verifies the asset files and their recorded counters without importing:
	# hashes already ran above; here the OBJ shape is spot-checked.
	var obj_path := _out + "/stress/stress_10m.obj"
	var file := FileAccess.open(obj_path, FileAccess.READ)
	if not _check(file != null, "stress_10m.obj missing."):
		return
	var header := file.get_line()
	var object_line := file.get_line()
	var first_vertex := file.get_line()
	_check(header.begins_with("# Dense City Blocks stress_10m"), "stress OBJ header mismatch: %s" % header)
	_check(object_line == "o stress_10m", "stress OBJ object line mismatch: %s" % object_line)
	_check(first_vertex.begins_with("v "), "stress OBJ first vertex mismatch: %s" % first_vertex)
	file.close()
	var triangles := int(p_entry["triangles"])
	_check(absf(float(triangles) / 10000000.0 - 1.0) <= 0.01,
			"stress: recorded triangles %d outside 1%% of 10M." % triangles)
	print("validate_dense_city: stress triangles=%d (import deferred to G4-2 in PQ)" % triangles)
