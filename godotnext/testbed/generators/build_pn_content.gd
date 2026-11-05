extends RefCounted
## PN content scenes (#148, plan §21.4.3): Foliage Field, Displacement Cliff
## and Spline Road. Original procedural geometry and generated textures only.
##
## Micro Detail is the Dense City generator's `micro_detail` variant
## (dense_city_generator.gd). Character Crowd is built here from the P3 rig.
##
## Run from build_all or
## `godot --headless --path godotnext/testbed --script res://generators/build_pn.gd`.
##
## Outputs go to `res://generated/pn_content/` (gitignored), like the Dense
## City generator: the heavy geometry and MultiMesh transforms are not
## committed, only this seeded generator is. Two runs produce byte-identical
## `.tscn`, `.res` and `.json` files on one machine.

const Scaffold := preload("res://generators/scaffold.gd")
const Character = preload("res://scenes/character_walk/character.gd")

const OUT := "res://generated/pn_content"

# Streams are independent of build order and of each other.
const FIELD_STREAM := 71
const CLIFF_STREAM := 72
const ROAD_STREAM := 73

const FIELD_HALF_M := 200.0
const TREE_COUNT := 100000
const GRASS_COUNT := 100000
const TREE_TRUNK_SIDES := 6
const TREE_TRUNK_RINGS := 2
const TREE_LEAF_CARDS := 3

const CLIFF_LENGTH_M := 200.0
const CLIFF_DEPTH_M := 60.0
const CLIFF_HEIGHT_M := 42.0
const CLIFF_GRID := Vector2i(256, 128)

const ROAD_LENGTH_M := 2000.0
const ROAD_HALF_WIDTH_M := 3.5


static func contract_error() -> String:
	if TREE_COUNT + GRASS_COUNT != 200000:
		return "Foliage Field needs 200k trees and grass, got %d." % (TREE_COUNT + GRASS_COUNT)
	if CLIFF_LENGTH_M != 200.0:
		return "Displacement Cliff must be 200 m long."
	if ROAD_LENGTH_M != 2000.0:
		return "Spline Road must be 2 km long."
	var character_err := String(Character.contract_error())
	if not character_err.is_empty():
		return character_err
	return ""


static func save_all(p_host: SceneTree) -> Array[String]:
	var failures: Array[String] = []
	var contract := contract_error()
	if not contract.is_empty():
		failures.append(contract)
		return failures
	var jobs: Array[Dictionary] = [
		{"built": build_foliage_field("plain"), "scene": OUT + "/foliage_field.tscn"},
		{"built": build_foliage_field("assemblies"), "scene": OUT + "/foliage_field_assemblies.tscn"},
		{"built": build_foliage_field("voxels"), "scene": OUT + "/foliage_field_voxels.tscn"},
		{"built": build_displacement_cliff(), "scene": OUT + "/displacement_cliff.tscn"},
		{"built": build_spline_road(), "scene": OUT + "/spline_road.tscn"},
	]
	for job in jobs:
		var built: Dictionary = job["built"]
		if built.is_empty() or not built.has("root"):
			failures.append("%s: the builder returned no scene." % String(job["scene"]))
			continue
		var root: Node3D = built["root"]
		var scene_path: String = job["scene"]
		p_host.get_root().add_child(root)
		await p_host.physics_frame
		await p_host.physics_frame
		if not _verify(root, built, failures, scene_path):
			p_host.get_root().remove_child(root)
			root.free()
			continue
		p_host.get_root().remove_child(root)
		var err := Scaffold.save_scene(root, scene_path)
		if err != OK:
			failures.append("%s: save failed with error %d." % [scene_path, err])
		else:
			print("build_pn: wrote " + scene_path)
		root.free()
	return failures


# --- Foliage Field -----------------------------------------------------------

## [param p_variant] is `plain`, `assemblies` or `voxels`. All three share the
## same procedural trees; the assembly variant keeps the trunk and leaves as
## separate MultiMeshInstances, the voxel variant tags the root for the GN-7
## voxel LOD comparison.
static func build_foliage_field(p_variant: String) -> Dictionary:
	var rng := Scaffold.make_rng(FIELD_STREAM)
	var root := Node3D.new()
	root.name = "FoliageField"
	root.set_meta("seed", Scaffold.stream_seed(FIELD_STREAM))
	root.set_meta("variant", p_variant)
	root.set_meta("tree_count", TREE_COUNT)
	root.set_meta("grass_count", GRASS_COUNT)
	if p_variant == "voxels":
		root.set_meta("voxel_lod", true)
	_add_world(root, Color(0.62, 0.74, 0.87), 0.9)
	_add_sun(root, Vector3(-0.4, -0.85, -0.25), Vector3(0.0, 30.0, 0.0))

	var ground_mat := Scaffold.make_constant_material("FieldGround", Color(0.19, 0.26, 0.13), 0.95)
	var ground_mesh := PlaneMesh.new()
	ground_mesh.size = Vector2(FIELD_HALF_M * 2.0 + 40.0, FIELD_HALF_M * 2.0 + 40.0)
	ground_mesh.material = ground_mat
	var ground := MeshInstance3D.new()
	ground.name = "Ground"
	ground.mesh = ground_mesh
	Scaffold.add_owned(root, ground, root)

	var tree := _tree_mesh()
	var trunk_surface := _surface_mesh(tree, 0, "TreeTrunk")
	var leaf_surface := _surface_mesh(tree, 1, "TreeLeaves")
	var grass := _grass_mesh()

	var tree_transforms := _field_transforms(FIELD_STREAM + 1, TREE_COUNT, 0.85, 1.45)
	var grass_transforms := _field_transforms(FIELD_STREAM + 2, GRASS_COUNT, 0.7, 1.3)
	if p_variant == "assemblies":
		var assemblies := Node3D.new()
		assemblies.name = "Assemblies"
		Scaffold.add_owned(root, assemblies, root)
		Scaffold.add_owned(assemblies, _multi_mesh_instance("Trunk", trunk_surface, trunk_material(), tree_transforms, OUT + "/foliage_trunk.res"), root)
		Scaffold.add_owned(assemblies, _multi_mesh_instance("Leaves", leaf_surface, _leaf_material(), tree_transforms, OUT + "/foliage_leaves.res"), root)
	else:
		Scaffold.add_owned(root, _multi_mesh_instance("Trees", tree, tree_material(), tree_transforms, OUT + "/foliage_trees.res"), root)
	Scaffold.add_owned(root, _multi_mesh_instance("Grass", grass, grass_material(), grass_transforms, OUT + "/foliage_grass.res"), root)

	# 60 s flight through the field, plus four static gate cameras.
	var flight := Path3D.new()
	flight.name = "FlightPath"
	var curve := Curve3D.new()
	for point in [Vector3(-170.0, 26.0, -170.0), Vector3(-90.0, 12.0, -60.0), Vector3(0.0, 6.0, 10.0),
			Vector3(80.0, 9.0, 70.0), Vector3(160.0, 18.0, 150.0), Vector3(150.0, 30.0, -120.0),
			Vector3(-40.0, 34.0, -160.0)]:
		curve.add_point(point)
	flight.curve = curve
	Scaffold.add_owned(root, flight, root)
	Scaffold.make_camera(root, root, "cam_main", Vector3(0.0, 6.0, 10.0), Vector3(0.0, 4.0, -30.0), 600.0, 70.0, true)
	Scaffold.make_camera(root, root, "cam_canopy", Vector3(-60.0, 24.0, -60.0), Vector3(40.0, 2.0, 40.0), 600.0, 60.0)
	Scaffold.make_camera(root, root, "cam_ground", Vector3(20.0, 1.6, 20.0), Vector3(60.0, 6.0, 60.0), 400.0, 70.0)
	Scaffold.make_camera(root, root, "cam_far", Vector3(180.0, 40.0, 180.0), Vector3(0.0, 0.0, 0.0), 900.0, 55.0)

	return {
		"root": root,
		"trees": TREE_COUNT,
		"grass": GRASS_COUNT,
		"tree_triangles": int(tree.get_faces().size() / 3),
		"check": "foliage_field",
	}


static func _field_transforms(p_stream: int, p_count: int, p_min_scale: float, p_max_scale: float) -> PackedFloat32Array:
	var rng := Scaffold.make_rng(p_stream)
	var out := PackedFloat32Array()
	out.resize(p_count * 12)
	for i in range(p_count):
		var pos := Vector3(rng.randf_range(-FIELD_HALF_M, FIELD_HALF_M), 0.0, rng.randf_range(-FIELD_HALF_M, FIELD_HALF_M))
		# Keep the two entry lanes clear for the flight path.
		if absf(pos.x) < 12.0 and pos.z > -190.0:
			pos.x += 24.0
		var scale := rng.randf_range(p_min_scale, p_max_scale)
		var basis := Basis(Vector3.UP, rng.randf_range(0.0, TAU)).scaled(Vector3(scale, scale, scale))
		var base := i * 12
		# MultiMesh 3D layout: 3x4 row-major basis rows, then the origin component.
		out[base + 0] = basis.x.x
		out[base + 1] = basis.y.x
		out[base + 2] = basis.z.x
		out[base + 3] = pos.x
		out[base + 4] = basis.x.y
		out[base + 5] = basis.y.y
		out[base + 6] = basis.z.y
		out[base + 7] = pos.y
		out[base + 8] = basis.x.z
		out[base + 9] = basis.y.z
		out[base + 10] = basis.z.z
		out[base + 11] = pos.z
	return out


static func _multi_mesh_instance(p_name: String, p_mesh: Mesh, p_material: Material, p_transforms: PackedFloat32Array, p_resource_path: String) -> MultiMeshInstance3D:
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = p_mesh
	mm.instance_count = p_transforms.size() / 12
	# Assign the buffer directly: the headless dummy renderer has no MultiMesh
	# buffer readback, but the buffer itself is what the resource stores.
	mm.buffer = p_transforms
	_ensure_dir(OUT)
	var err := ResourceSaver.save(mm, p_resource_path)
	if err == OK:
		mm = load(p_resource_path)
	var node := MultiMeshInstance3D.new()
	node.name = p_name
	node.multimesh = mm
	node.material_override = p_material
	return node


## One tree: a tapered 6-sided trunk and three crossed masked leaf cards.
static func _tree_mesh() -> ArrayMesh:
	var mesh := ArrayMesh.new()
	var rng := Scaffold.make_rng(FIELD_STREAM + 3)
	var trunk := SurfaceTool.new()
	trunk.begin(Mesh.PRIMITIVE_TRIANGLES)
	for ring in range(TREE_TRUNK_RINGS):
		var y0 := 1.6 + float(ring) * 1.5
		var y1 := y0 + 1.5
		var r0 := 0.24 - float(ring) * 0.06
		var r1 := 0.24 - float(ring + 1) * 0.06
		for side in range(TREE_TRUNK_SIDES):
			var a0 := TAU * float(side) / float(TREE_TRUNK_SIDES)
			var a1 := TAU * float(side + 1) / float(TREE_TRUNK_SIDES)
			var p0 := Vector3(cos(a0) * r0, y0, sin(a0) * r0)
			var p1 := Vector3(cos(a1) * r0, y0, sin(a1) * r0)
			var p2 := Vector3(cos(a1) * r1, y1, sin(a1) * r1)
			var p3 := Vector3(cos(a0) * r1, y1, sin(a0) * r1)
			_add_quad(trunk, p0, p1, p2, p3, Vector3.UP * 0.2)
	trunk.generate_normals()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, trunk.commit_to_arrays())

	var leaves := SurfaceTool.new()
	leaves.begin(Mesh.PRIMITIVE_TRIANGLES)
	for card in range(TREE_LEAF_CARDS):
		var yaw := TAU * float(card) / float(TREE_LEAF_CARDS) + rng.randf_range(-0.2, 0.2)
		var height := 3.0 + rng.randf_range(0.1, 0.6)
		var half := 1.25
		var basis := Basis(Vector3.UP, yaw)
		var u := basis * Vector3.RIGHT * half
		var v := Vector3.UP * half
		var center := Vector3(0.0, height, 0.0)
		var p0 := center - u - v
		var p1 := center + u - v
		var p2 := center + u + v
		var p3 := center - u + v
		var normal := basis * Vector3.BACK
		var uv0 := Vector2(0.0, 1.0)
		var uv1 := Vector2(1.0, 1.0)
		var uv2 := Vector2(1.0, 0.0)
		var uv3 := Vector2(0.0, 0.0)
		_add_quad_uv(leaves, p0, p1, p2, p3, normal, uv0, uv1, uv2, uv3)
	leaves.generate_normals()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, leaves.commit_to_arrays())
	mesh.resource_name = "FoliageTree"
	return mesh


static func _grass_mesh() -> ArrayMesh:
	var tool := SurfaceTool.new()
	tool.begin(Mesh.PRIMITIVE_TRIANGLES)
	var u := Vector3(0.28, 0.0, 0.0)
	var v := Vector3(0.0, 0.62, 0.0)
	_add_quad_uv(tool, -u - v * 0.5, u - v * 0.5, u + v * 0.5, -u + v * 0.5, Vector3.BACK,
			Vector2(0.0, 1.0), Vector2(1.0, 1.0), Vector2(1.0, 0.0), Vector2(0.0, 0.0))
	_add_quad_uv(tool, Vector3(0.0, 0.0, -0.28), Vector3(0.0, 0.0, 0.28), Vector3(0.0, 0.62, 0.28), Vector3(0.0, 0.62, -0.28), Vector3.RIGHT,
			Vector2(0.0, 1.0), Vector2(1.0, 1.0), Vector2(1.0, 0.0), Vector2(0.0, 0.0))
	tool.generate_normals()
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, tool.commit_to_arrays())
	mesh.resource_name = "FoliageGrass"
	return mesh


static func _add_quad(p_tool: SurfaceTool, p0: Vector3, p1: Vector3, p2: Vector3, p3: Vector3, p_normal: Vector3) -> void:
	_add_quad_uv(p_tool, p0, p1, p2, p3, p_normal, Vector2(0.0, 1.0), Vector2(1.0, 1.0), Vector2(1.0, 0.0), Vector2(0.0, 0.0))


static func _add_quad_uv(p_tool: SurfaceTool, p0: Vector3, p1: Vector3, p2: Vector3, p3: Vector3, p_normal: Vector3,
		p_uv0: Vector2, p_uv1: Vector2, p_uv2: Vector2, p_uv3: Vector2) -> void:
	p_tool.set_normal(p_normal)
	p_tool.set_uv(p_uv0)
	p_tool.add_vertex(p0)
	p_tool.set_uv(p_uv1)
	p_tool.add_vertex(p1)
	p_tool.set_uv(p_uv2)
	p_tool.add_vertex(p2)
	p_tool.set_uv(p_uv0)
	p_tool.add_vertex(p0)
	p_tool.set_uv(p_uv2)
	p_tool.add_vertex(p2)
	p_tool.set_uv(p_uv3)
	p_tool.add_vertex(p3)


static func _surface_mesh(p_mesh: ArrayMesh, p_surface: int, p_name: String) -> ArrayMesh:
	var out := ArrayMesh.new()
	out.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, p_mesh.surface_get_arrays(p_surface))
	out.resource_name = p_name
	return out


## A 64x64 alpha silhouette so leaves and grass use alpha scissor, not
## transparent sorting.
static func _leaf_texture() -> ImageTexture:
	var image := Image.create_empty(64, 64, false, Image.FORMAT_RGBA8)
	for y in range(64):
		for x in range(64):
			var dx := (float(x) + 0.5 - 32.0) / 31.0
			var dy := (float(y) + 0.5 - 32.0) / 31.0
			# A pointed leaf: an ellipse pinched at the top and bottom.
			var width := 1.0 - absf(dy) * 0.85
			var inside := absf(dx) < width * 0.9 and absf(dy) < 0.98
			image.set_pixel(x, y, Color(0.25, 0.55, 0.2, 1.0) if inside else Color(0.0, 0.0, 0.0, 0.0))
	return ImageTexture.create_from_image(image)


static func _leaf_material() -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_texture = _leaf_texture()
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	material.alpha_scissor_threshold = 0.5
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.roughness = 0.85
	return material


static func tree_material() -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_texture = _leaf_texture()
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	material.alpha_scissor_threshold = 0.5
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.roughness = 0.85
	return material


static func trunk_material() -> StandardMaterial3D:
	return Scaffold.make_constant_material("TreeTrunk", Color(0.24, 0.16, 0.09), 0.95)


static func grass_material() -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.32, 0.5, 0.18)
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	material.alpha_scissor_threshold = 0.5
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.roughness = 0.9
	return material


# --- Displacement Cliff ------------------------------------------------------

## A 200 m heightfield cliff with a tiling noise texture. The grid is dense
## enough that the finest cut has sub-pixel triangles at 1080p in the
## distance, which is what the tessellation row measures.
static func build_displacement_cliff() -> Dictionary:
	var root := Node3D.new()
	root.name = "DisplacementCliff"
	root.set_meta("seed", Scaffold.stream_seed(CLIFF_STREAM))
	root.set_meta("length_m", CLIFF_LENGTH_M)
	root.set_meta("height_m", CLIFF_HEIGHT_M)
	_add_world(root, Color(0.58, 0.68, 0.78), 0.8)
	_add_sun(root, Vector3(-0.55, -0.6, -0.35), Vector3(-20.0, 60.0, 20.0))

	var noise := FastNoiseLite.new()
	noise.seed = Scaffold.stream_seed(CLIFF_STREAM)
	noise.frequency = 0.012
	noise.fractal_octaves = 5
	noise.fractal_gain = 0.55

	var mesh := _cliff_mesh(noise)
	var material := Scaffold.make_constant_material("CliffRock", Color(0.38, 0.35, 0.31), 0.92)
	var detail := NoiseTexture2D.new()
	detail.width = 512
	detail.height = 512
	detail.seamless = true
	detail.noise = noise
	detail.color_ramp = _rock_ramp()
	material.albedo_texture = detail
	material.uv1_scale = Vector3(24.0, 24.0, 1.0)
	var cliff := MeshInstance3D.new()
	cliff.name = "Cliff"
	cliff.mesh = mesh
	cliff.material_override = material
	Scaffold.add_owned(root, cliff, root)

	var floor_mesh := PlaneMesh.new()
	floor_mesh.size = Vector2(CLIFF_LENGTH_M * 2.0, CLIFF_DEPTH_M * 4.0)
	floor_mesh.material = Scaffold.make_constant_material("CliffFloor", Color(0.22, 0.25, 0.19), 0.95)
	var floor_node := MeshInstance3D.new()
	floor_node.name = "Floor"
	floor_node.position = Vector3(0.0, -0.05, -CLIFF_DEPTH_M * 1.2)
	floor_node.mesh = floor_mesh
	Scaffold.add_owned(root, floor_node, root)

	var flight := Path3D.new()
	flight.name = "FlightPath"
	var curve := Curve3D.new()
	for point in [Vector3(-110.0, 18.0, 70.0), Vector3(-60.0, 12.0, 50.0), Vector3(0.0, 22.0, 40.0),
			Vector3(60.0, 14.0, 55.0), Vector3(110.0, 26.0, 80.0)]:
		curve.add_point(point)
	flight.curve = curve
	Scaffold.add_owned(root, flight, root)
	Scaffold.make_camera(root, root, "cam_main", Vector3(0.0, 18.0, 70.0), Vector3(0.0, 18.0, 0.0), 500.0, 65.0, true)
	Scaffold.make_camera(root, root, "cam_detail", Vector3(30.0, 8.0, 26.0), Vector3(10.0, 20.0, -10.0), 300.0, 80.0)
	Scaffold.make_camera(root, root, "cam_wide", Vector3(-120.0, 70.0, 140.0), Vector3(0.0, 10.0, 0.0), 900.0, 50.0)
	Scaffold.make_camera(root, root, "cam_ground", Vector3(70.0, 1.7, 30.0), Vector3(-20.0, 25.0, -20.0), 400.0, 75.0)

	return {
		"root": root,
		"triangles": int(mesh.get_faces().size() / 3),
		"check": "displacement_cliff",
	}


static func _rock_ramp() -> Gradient:
	var ramp := Gradient.new()
	ramp.set_color(0, Color(0.24, 0.22, 0.2))
	ramp.set_color(1, Color(0.62, 0.58, 0.5))
	return ramp


static func _cliff_mesh(p_noise: FastNoiseLite) -> ArrayMesh:
	var tool := SurfaceTool.new()
	tool.begin(Mesh.PRIMITIVE_TRIANGLES)
	var step_x := CLIFF_LENGTH_M / float(CLIFF_GRID.x)
	var step_z := CLIFF_DEPTH_M / float(CLIFF_GRID.y)
	for z in range(CLIFF_GRID.y):
		for x in range(CLIFF_GRID.x):
			var x0 := -CLIFF_LENGTH_M * 0.5 + float(x) * step_x
			var x1 := x0 + step_x
			var z0 := -CLIFF_DEPTH_M * 0.5 + float(z) * step_z
			var z1 := z0 + step_z
			var p0 := Vector3(x0, _cliff_height(x0, z0, p_noise), z0)
			var p1 := Vector3(x1, _cliff_height(x1, z0, p_noise), z0)
			var p2 := Vector3(x1, _cliff_height(x1, z1, p_noise), z1)
			var p3 := Vector3(x0, _cliff_height(x0, z1, p_noise), z1)
			tool.set_uv(Vector2(x0, z0) * 0.01)
			tool.add_vertex(p0)
			tool.set_uv(Vector2(x1, z0) * 0.01)
			tool.add_vertex(p1)
			tool.set_uv(Vector2(x1, z1) * 0.01)
			tool.add_vertex(p2)
			tool.set_uv(Vector2(x0, z0) * 0.01)
			tool.add_vertex(p0)
			tool.set_uv(Vector2(x1, z1) * 0.01)
			tool.add_vertex(p2)
			tool.set_uv(Vector2(x0, z1) * 0.01)
			tool.add_vertex(p3)
	tool.generate_normals()
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, tool.commit_to_arrays())
	mesh.resource_name = "CliffHeightfield"
	return mesh


static func _cliff_height(p_x: float, p_z: float, p_noise: FastNoiseLite) -> float:
	# A rising back wall with noise ledges; zero at the field edge so the
	# scene meets the floor.
	var ridge := clampf((p_z + CLIFF_DEPTH_M * 0.5) / CLIFF_DEPTH_M, 0.0, 1.0)
	var base := pow(ridge, 1.6) * CLIFF_HEIGHT_M
	var n := p_noise.get_noise_2d(p_x * 1.6, p_z * 1.6)
	var ledges := roundf(n * 5.0) / 5.0 * 4.5
	return base + ledges


# --- Spline Road -------------------------------------------------------------

## Two kilometres of road and guard rails along a stored Curve3D.
static func build_spline_road() -> Dictionary:
	var root := Node3D.new()
	root.name = "SplineRoad"
	root.set_meta("seed", Scaffold.stream_seed(ROAD_STREAM))
	root.set_meta("length_m", ROAD_LENGTH_M)
	_add_world(root, Color(0.6, 0.7, 0.82), 0.85)
	_add_sun(root, Vector3(-0.35, -0.8, -0.4), Vector3(0.0, 40.0, 0.0))

	var curve := _road_curve()
	var mesh := _road_mesh(curve)
	var road := MeshInstance3D.new()
	road.name = "Road"
	road.mesh = mesh
	var road_mat := Scaffold.make_constant_material("RoadAsphalt", Color(0.12, 0.12, 0.13), 0.88)
	road_mat.uv1_scale = Vector3(1.0, 40.0, 1.0)
	mesh.surface_set_material(0, road_mat)
	mesh.surface_set_material(1, Scaffold.make_constant_material("RoadRail", Color(0.55, 0.56, 0.58), 0.35, 0.8))
	Scaffold.add_owned(root, road, root)

	var path := Path3D.new()
	path.name = "RoadPath"
	path.curve = curve
	Scaffold.add_owned(root, path, root)

	var ground_mesh := PlaneMesh.new()
	ground_mesh.size = Vector2(3000.0, 3000.0)
	ground_mesh.material = Scaffold.make_constant_material("RoadGround", Color(0.2, 0.28, 0.16), 0.95)
	var ground := MeshInstance3D.new()
	ground.name = "Ground"
	ground.mesh = ground_mesh
	Scaffold.add_owned(root, ground, root)

	var flight := Path3D.new()
	flight.name = "FlightPath"
	flight.curve = curve
	Scaffold.add_owned(root, flight, root)
	var start: Vector3 = curve.sample_baked(0.0, true)
	var mid: Vector3 = curve.sample_baked(curve.get_baked_length() * 0.5, true)
	var end: Vector3 = curve.sample_baked(curve.get_baked_length(), true)
	Scaffold.make_camera(root, root, "cam_main", start + Vector3(0.0, 6.0, 0.0), mid, 1200.0, 65.0, true)
	Scaffold.make_camera(root, root, "cam_mid", mid + Vector3(0.0, 30.0, 0.0), end, 1500.0, 55.0)
	Scaffold.make_camera(root, root, "cam_low", mid + Vector3(8.0, 1.6, 8.0), mid + Vector3(60.0, 1.0, 0.0), 800.0, 70.0)
	Scaffold.make_camera(root, root, "cam_end", end + Vector3(0.0, 18.0, 0.0), start, 2500.0, 50.0)

	return {
		"root": root,
		"triangles": int(mesh.get_faces().size() / 3),
		"check": "spline_road",
	}


static func _road_curve() -> Curve3D:
	var curve := Curve3D.new()
	var count := 24
	var points: Array[Vector3] = []
	for i in range(count + 1):
		var t := float(i) / float(count)
		var x := -ROAD_LENGTH_M * 0.5 + t * ROAD_LENGTH_M
		var z := sin(t * TAU * 1.5) * 140.0 + sin(t * TAU * 0.35) * 60.0
		var y := sin(t * TAU * 0.9) * 9.0
		points.append(Vector3(x, y, z))
	for i in range(points.size()):
		var point := points[i]
		var tangent: Vector3
		if i == 0:
			tangent = (points[1] - points[0]).normalized()
		elif i == points.size() - 1:
			tangent = (points[i] - points[i - 1]).normalized()
		else:
			tangent = (points[i + 1] - points[i - 1]).normalized()
		curve.add_point(point, Vector3.ZERO, tangent * 30.0)
	return curve


static func _road_mesh(p_curve: Curve3D) -> ArrayMesh:
	var length := p_curve.get_baked_length()
	var samples := int(length / 1.0)
	var road := SurfaceTool.new()
	road.begin(Mesh.PRIMITIVE_TRIANGLES)
	var rails := SurfaceTool.new()
	rails.begin(Mesh.PRIMITIVE_TRIANGLES)
	var previous_left := Vector3.ZERO
	var previous_right := Vector3.ZERO
	var previous_rail_l_bottom := Vector3.ZERO
	var previous_rail_l_top := Vector3.ZERO
	var previous_rail_r_bottom := Vector3.ZERO
	var previous_rail_r_top := Vector3.ZERO
	var have_previous := false
	for i in range(samples + 1):
		var distance := minf(float(i), length)
		var position := p_curve.sample_baked(distance, true)
		var offset := distance + 1.0
		var tangent := (p_curve.sample_baked(minf(offset, length), true) - position).normalized()
		if tangent.length_squared() < 0.5:
			tangent = Vector3.RIGHT
		var side := tangent.cross(Vector3.UP).normalized()
		var left := position - side * ROAD_HALF_WIDTH_M
		var right := position + side * ROAD_HALF_WIDTH_M
		var rail_l_bottom := left - side * 0.4 + Vector3.UP * 0.8
		var rail_l_top := rail_l_bottom + Vector3.UP * 0.35
		var rail_r_bottom := right + side * 0.4 + Vector3.UP * 0.8
		var rail_r_top := rail_r_bottom + Vector3.UP * 0.35
		if have_previous:
			_road_quad(road, previous_left, previous_right, right, left, Vector3.UP)
			_rail_quad(rails, previous_rail_l_bottom, rail_l_bottom, rail_l_top, previous_rail_l_top, -side)
			_rail_quad(rails, previous_rail_r_bottom, rail_r_bottom, rail_r_top, previous_rail_r_top, side)
		previous_left = left
		previous_right = right
		previous_rail_l_bottom = rail_l_bottom
		previous_rail_l_top = rail_l_top
		previous_rail_r_bottom = rail_r_bottom
		previous_rail_r_top = rail_r_top
		have_previous = true
	var mesh := ArrayMesh.new()
	road.generate_normals()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, road.commit_to_arrays())
	rails.generate_normals()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, rails.commit_to_arrays())
	mesh.resource_name = "SplineRoadMesh"
	return mesh


static func _road_quad(p_tool: SurfaceTool, p_a: Vector3, p_b: Vector3, p_c: Vector3, p_d: Vector3, p_normal: Vector3) -> void:
	p_tool.set_normal(p_normal)
	p_tool.set_uv(Vector2(0.0, 0.0))
	p_tool.add_vertex(p_a)
	p_tool.set_uv(Vector2(1.0, 0.0))
	p_tool.add_vertex(p_b)
	p_tool.set_uv(Vector2(1.0, 1.0))
	p_tool.add_vertex(p_c)
	p_tool.set_uv(Vector2(0.0, 0.0))
	p_tool.add_vertex(p_a)
	p_tool.set_uv(Vector2(1.0, 1.0))
	p_tool.add_vertex(p_c)
	p_tool.set_uv(Vector2(0.0, 1.0))
	p_tool.add_vertex(p_d)


static func _rail_quad(p_tool: SurfaceTool, p_previous_bottom: Vector3, p_bottom: Vector3, p_top: Vector3, p_previous_top: Vector3, p_normal: Vector3) -> void:
	p_tool.set_normal(p_normal)
	p_tool.set_uv(Vector2(0.0, 0.0))
	p_tool.add_vertex(p_previous_bottom)
	p_tool.set_uv(Vector2(1.0, 0.0))
	p_tool.add_vertex(p_bottom)
	p_tool.set_uv(Vector2(1.0, 1.0))
	p_tool.add_vertex(p_top)
	p_tool.set_uv(Vector2(0.0, 0.0))
	p_tool.add_vertex(p_previous_bottom)
	p_tool.set_uv(Vector2(1.0, 1.0))
	p_tool.add_vertex(p_top)
	p_tool.set_uv(Vector2(0.0, 1.0))
	p_tool.add_vertex(p_previous_top)


# --- Shared ------------------------------------------------------------------

static func _ensure_dir(p_path: String) -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(p_path))


static func _add_world(p_root: Node3D, p_sky_color: Color, p_ambient: float) -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	var sky_material := ProceduralSkyMaterial.new()
	sky_material.sky_top_color = p_sky_color
	sky_material.sky_horizon_color = p_sky_color.lerp(Color(0.9, 0.9, 0.85), 0.5)
	sky_material.ground_bottom_color = Color(0.12, 0.13, 0.12)
	sky_material.ground_horizon_color = p_sky_color
	sky.sky_material = sky_material
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.ambient_light_energy = p_ambient
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = environment
	Scaffold.add_owned(p_root, world_env, p_root)


static func _add_sun(p_root: Node3D, p_direction: Vector3, p_position: Vector3) -> void:
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.transform = Transform3D(Basis.looking_at(p_direction.normalized(), Vector3.UP), p_position)
	sun.light_energy = 1.4
	sun.shadow_enabled = true
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
	sun.directional_shadow_max_distance = 800.0
	Scaffold.add_owned(p_root, sun, p_root)


static func _verify(p_root: Node3D, p_built: Dictionary, p_failures: Array[String], p_scene: String) -> bool:
	var before := p_failures.size()
	var check: String = p_built.get("check", "")
	match check:
		"foliage_field":
			var trees := p_root.get_node_or_null("Trees")
			var assemblies := p_root.get_node_or_null("Assemblies")
			if p_root.get_node_or_null("Grass") == null:
				p_failures.append("%s: Grass MultiMeshInstance3D missing." % p_scene)
			if trees == null and assemblies == null:
				p_failures.append("%s: no tree instances." % p_scene)
			if int(p_built["tree_triangles"]) <= 0:
				p_failures.append("%s: tree mesh has no triangles." % p_scene)
		"displacement_cliff":
			if int(p_built["triangles"]) < 60000:
				p_failures.append("%s: cliff has %d triangles, expected at least 60000." % [p_scene, int(p_built["triangles"])])
			if p_root.get_node_or_null("Cliff") == null:
				p_failures.append("%s: Cliff mesh missing." % p_scene)
		"spline_road":
			var road := p_root.get_node_or_null("Road") as MeshInstance3D
			if road == null or road.mesh == null:
				p_failures.append("%s: Road mesh missing." % p_scene)
			elif road.mesh.get_surface_count() != 2:
				p_failures.append("%s: road has %d surfaces, expected road and rails." % [p_scene, road.mesh.get_surface_count()])
			if p_root.get_node_or_null("RoadPath") == null:
				p_failures.append("%s: RoadPath missing." % p_scene)
	return p_failures.size() == before
