extends SceneTree
## PN content post-generation validator (#148).
##
## Run after [code]build_pn.gd[/code], from the repository root:
## [code]godot --headless --path godotnext/testbed --script res://generators/validate_pn_content.gd[/code]
##
## Checks the generated scenes structurally against the §21.4.3 content
## contract: the two Foliage Field MultiMeshes (200k trees and grass in
## total), the three Foliage Field variants, the 200 m cliff triangle count,
## the 2 km Spline Road curve and its road/rail surfaces, the four static
## cameras and the 60 s flight path on every scene. Byte-identity of a
## generator run is checked by running [code]build_pn.gd[/code] three times
## and comparing the output hashes; this script checks the result, not the
## hashes, so it does not need a tracked expected.json.

const OUT := "res://generated/pn_content"
const FOLIAGE_SCENES: Array[String] = ["foliage_field.tscn", "foliage_field_assemblies.tscn", "foliage_field_voxels.tscn"]
const TREE_GRASS_TOTAL := 200000
const CLIFF_MIN_TRIANGLES := 60000
const ROAD_MIN_TRIANGLES := 1000

var _failures: Array[String] = []


func _init() -> void:
	_run.call_deferred()


func _fail(p_message: String) -> void:
	_failures.append(p_message)
	printerr("validate_pn_content FAIL: " + p_message)


func _check(p_condition: bool, p_message: String) -> bool:
	if not p_condition:
		_fail(p_message)
	return p_condition


func _run() -> void:
	for scene in FOLIAGE_SCENES:
		_check_foliage(scene)
	_check_cliff()
	_check_road()
	if _failures.is_empty():
		print("validate_pn_content: all scenes verified.")
		quit(0)
	else:
		quit(1)


func _load_scene(p_name: String) -> Node3D:
	var path := OUT + "/" + p_name
	if not ResourceLoader.exists(path):
		_fail("%s is missing; run generators/build_pn.gd first." % path)
		return null
	var packed: PackedScene = load(path)
	if packed == null:
		_fail("%s does not load." % path)
		return null
	var root := packed.instantiate() as Node3D
	if root == null:
		_fail("%s does not instantiate as a Node3D." % path)
	return root


func _check_cameras_and_path(p_root: Node3D, p_scene: String) -> void:
	var cameras := 0
	for child in p_root.get_children():
		if child is Camera3D:
			cameras += 1
	_check(cameras >= 4, "%s has %d cameras, expected at least 4." % [p_scene, cameras])
	var path := p_root.get_node_or_null("FlightPath") as Path3D
	_check(path != null and path.curve != null and path.curve.get_baked_length() >= 20.0,
			"%s has no usable 60 s flight path." % p_scene)


func _check_foliage(p_scene: String) -> void:
	var root := _load_scene(p_scene)
	if root == null:
		return
	var grass := root.get_node_or_null("Grass") as MultiMeshInstance3D
	_check(grass != null and grass.multimesh != null, "%s has no Grass MultiMesh." % p_scene)
	var trees := root.get_node_or_null("Trees") as MultiMeshInstance3D
	var assemblies := root.get_node_or_null("Assemblies") as Node3D
	var tree_count := 0
	if trees != null and trees.multimesh != null:
		tree_count = trees.multimesh.instance_count
	elif assemblies != null:
		var trunk := assemblies.get_node_or_null("Trunk") as MultiMeshInstance3D
		var leaves := assemblies.get_node_or_null("Leaves") as MultiMeshInstance3D
		_check(trunk != null and leaves != null, "%s assemblies are missing trunk or leaves." % p_scene)
		if trunk != null and trunk.multimesh != null:
			tree_count = trunk.multimesh.instance_count
	var grass_count := grass.multimesh.instance_count if grass != null and grass.multimesh != null else 0
	_check(tree_count + grass_count == TREE_GRASS_TOTAL,
			"%s has %d trees and %d grass, expected %d in total." % [p_scene, tree_count, grass_count, TREE_GRASS_TOTAL])
	var base := p_scene.get_basename()
	var expected_variant := "plain" if base == "foliage_field" else base.trim_prefix("foliage_field_")
	_check(String(root.get_meta("variant", "")) == expected_variant,
			"%s variant metadata is '%s', expected '%s'." % [p_scene, String(root.get_meta("variant", "")), expected_variant])
	_check_cameras_and_path(root, p_scene)
	root.free()


func _check_cliff() -> void:
	var root := _load_scene("displacement_cliff.tscn")
	if root == null:
		return
	_check(float(root.get_meta("length_m", 0.0)) == 200.0, "displacement_cliff length is not 200 m.")
	var cliff := root.get_node_or_null("Cliff") as MeshInstance3D
	var triangles := cliff.mesh.get_faces().size() / 3 if cliff != null and cliff.mesh != null else 0
	_check(triangles >= CLIFF_MIN_TRIANGLES,
			"displacement_cliff has %d triangles, expected at least %d." % [triangles, CLIFF_MIN_TRIANGLES])
	var material := cliff.material_override as StandardMaterial3D if cliff != null else null
	_check(material != null and material.albedo_texture is NoiseTexture2D,
			"displacement_cliff has no tiling noise height texture.")
	_check_cameras_and_path(root, "displacement_cliff.tscn")
	root.free()


func _check_road() -> void:
	var root := _load_scene("spline_road.tscn")
	if root == null:
		return
	var path := root.get_node_or_null("RoadPath") as Path3D
	var length := path.curve.get_baked_length() if path != null and path.curve != null else 0.0
	_check(length >= 1990.0, "spline_road curve is %.1f m, expected 2 km." % length)
	var road := root.get_node_or_null("Road") as MeshInstance3D
	_check(road != null and road.mesh != null, "spline_road has no Road mesh.")
	if road != null and road.mesh != null:
		_check(road.mesh.get_surface_count() == 2,
				"spline_road has %d surfaces, expected road and guard rails." % road.mesh.get_surface_count())
		var triangles := road.mesh.get_faces().size() / 3
		_check(triangles >= ROAD_MIN_TRIANGLES,
				"spline_road has %d triangles, expected at least %d." % [triangles, ROAD_MIN_TRIANGLES])
	_check_cameras_and_path(root, "spline_road.tscn")
	root.free()
