extends Node3D
## A small scene for the UE exporter's per-row refusals (run_ue_export_check.sh). It holds a box, a
## MultiMesh of three boxes, a GridMap of two boxes, a second GridMap (GridTurned: its own transform,
## non-cubic cells centered on Y and Z only, a cell scale, an item mesh transform and rotated
## cells, whose placements check_ue_export.gd compares with transforms written out by hand), an
## omni light and a one-bone skinned triangle in front of a camera, all still; it exports with
## --approximate=rigid_skin (the triangle as a rigid part) or without it (as a skeletal mesh).
## --case=<name> breaks one thing:
##
##   light_returns      the light moves away on frame 8 and is back on frame 9
##   mesh_blinks        the box is hidden on frame 8 only
##   multimesh_returns  one MultiMesh instance moves on frame 8 and is back on frame 9
##   gridmap_returns    one GridMap cell moves on frame 8 and is back on frame 9
##   skeleton_offset    the skinned triangle's Skeleton3D sits 0.5 m beside the mesh instance
##   camera_cull        the camera's cull mask hides the box's layer
##   camera_far         the camera's far plane is 5 m, so the boxes 5.5 m and more ahead reach past it
##   auto_skin          the skinned triangle has no Skin (the renderer makes one from the rests)
##   skin_scale         the skeleton's bone has a skin scale of 1.5
##   rest_only          the skeleton shows the rest pose only, while its bone is posed
##   bone_disabled      the skeleton's bone is disabled, while it is posed
##   modifier           the skeleton has a SkeletonModifier3D child that turns its bone
##   gridmap_glass      a GridMap item with an alpha-blended material casts shadows
##   gridmap_shadows    two GridMap items share a mesh with different shadow settings
##
## Every motion case is back at its frame-0 state before the last frame, so only a comparison made
## on every row can see it. Without --case (or --case=none) the export must succeed. The nodes are
## built in _init, so the exporter finds the camera before the scene enters the tree.

const SceneArgs := preload("res://scenes/scene_args.gd")

const PERTURB_FRAME := 8
const BOX_LAYER := 2

var _case := ""
var _local_frame := 0
var _saw := false
var _perturbed := false
var _light: OmniLight3D
var _box: MeshInstance3D
var _rocks: MultiMeshInstance3D
var _rock_transform := Transform3D()
var _grid: GridMap


## Turns the skeleton's bone while the skin updates (the modifier case).
class TurnModifier extends SkeletonModifier3D:
	func _process_modification_with_delta(_delta: float) -> void:
		get_skeleton().set_bone_pose_rotation(0, Quaternion(Vector3.UP, 0.5))


## GridTurned's node transform, cells and settings (check_ue_export.gd recomputes its placements).
static func turned_transform() -> Transform3D:
	return Transform3D(Basis(Vector3.UP, 0.3), Vector3(-1.5, 0.5, -3.0))


static func turned_cell_size() -> Vector3:
	return Vector3(0.5, 0.75, 1.0)


## Centered on Y and Z, not on X.
static func turned_center() -> Vector3i:
	return Vector3i(0, 1, 1)


static func turned_cell_scale() -> float:
	return 0.8


static func turned_item_transform() -> Transform3D:
	return Transform3D(Basis(Vector3.BACK, 0.5), Vector3(0.1, 0.2, -0.3))


## [cell, orientation] pairs; each orientation is a product of quarter turns.
static func turned_cells() -> Array:
	return [
		[Vector3i(0, 0, 0), Basis()],
		[Vector3i(2, 1, -1), Basis(Vector3.UP, PI * 0.5)],
		[Vector3i(-1, 0, 1), Basis(Vector3.RIGHT, PI * 0.5)],
		[Vector3i(1, -1, 0), Basis(Vector3.UP, PI) * Basis(Vector3.BACK, PI * 0.5)],
	]


func _init() -> void:
	_case = SceneArgs.value("--case", "none")
	var material := StandardMaterial3D.new()
	material.diffuse_mode = BaseMaterial3D.DIFFUSE_LAMBERT

	var camera := Camera3D.new()
	camera.name = "Camera"
	camera.position = Vector3(0.0, 1.0, 6.0)
	if _case == "camera_cull":
		camera.cull_mask = 0xFFFFF & ~BOX_LAYER
	if _case == "camera_far":
		camera.far = 5.0
	add_child(camera)

	var box_mesh := BoxMesh.new()
	box_mesh.material = material
	_box = MeshInstance3D.new()
	_box.name = "Box"
	_box.mesh = box_mesh
	_box.layers = BOX_LAYER
	add_child(_box)

	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.mesh = box_mesh
	multimesh.instance_count = 3
	for index in 3:
		multimesh.set_instance_transform(index, Transform3D(Basis(), Vector3(-2.0 + 2.0 * index, -1.5, -2.0)))
	_rock_transform = multimesh.get_instance_transform(1)
	_rocks = MultiMeshInstance3D.new()
	_rocks.name = "Rocks"
	_rocks.multimesh = multimesh
	add_child(_rocks)

	var library := MeshLibrary.new()
	library.create_item(0)
	library.set_item_mesh(0, box_mesh)
	_grid = GridMap.new()
	_grid.name = "Grid"
	_grid.mesh_library = library
	_grid.cell_size = Vector3(1.0, 1.0, 1.0)
	_grid.position = Vector3(0.0, 2.5, -2.0)
	_grid.set_cell_item(Vector3i(-1, 0, 0), 0)
	_grid.set_cell_item(Vector3i(1, 0, 0), 0)
	if _case == "gridmap_glass":
		var glass := StandardMaterial3D.new()
		glass.diffuse_mode = BaseMaterial3D.DIFFUSE_LAMBERT
		glass.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		glass.albedo_color = Color(1.0, 1.0, 1.0, 0.4)
		var glass_mesh := BoxMesh.new()
		glass_mesh.material = glass
		library.create_item(1)
		library.set_item_mesh(1, glass_mesh)
		_grid.set_cell_item(Vector3i(0, 1, 0), 1)
	elif _case == "gridmap_shadows":
		library.create_item(1)
		library.set_item_mesh(1, box_mesh)
		library.set_item_mesh_cast_shadow(1, RenderingServer.SHADOW_CASTING_SETTING_OFF)
		_grid.set_cell_item(Vector3i(0, 1, 0), 1)
	add_child(_grid)

	var turned_library := MeshLibrary.new()
	turned_library.create_item(0)
	turned_library.set_item_mesh(0, box_mesh)
	turned_library.set_item_mesh_transform(0, turned_item_transform())
	var turned := GridMap.new()
	turned.name = "GridTurned"
	turned.mesh_library = turned_library
	turned.transform = turned_transform()
	turned.cell_size = turned_cell_size()
	turned.cell_center_x = turned_center().x != 0
	turned.cell_center_y = turned_center().y != 0
	turned.cell_center_z = turned_center().z != 0
	turned.cell_scale = turned_cell_scale()
	for cell: Array in turned_cells():
		turned.set_cell_item(cell[0], 0, turned.get_orthogonal_index_from_basis(cell[1]))
	add_child(turned)

	_light = OmniLight3D.new()
	_light.name = "Light"
	_light.position = Vector3(2.0, 2.0, 2.0)
	add_child(_light)

	var skeleton := Skeleton3D.new()
	skeleton.name = "Skeleton"
	skeleton.add_bone("root")
	skeleton.set_bone_rest(0, Transform3D())
	skeleton.position = Vector3(0.5, 0.0, 0.0) if _case == "skeleton_offset" else Vector3.ZERO
	if _case in ["rest_only", "bone_disabled"]:
		skeleton.set_bone_pose_rotation(0, Quaternion(Vector3.UP, 0.5))
	match _case:
		"skin_scale":
			skeleton.set_bone_skin_scale(0, Vector3(1.5, 1.5, 1.5))
		"rest_only":
			skeleton.show_rest_only = true
		"bone_disabled":
			skeleton.set_bone_enabled(0, false)
		"modifier":
			var modifier := TurnModifier.new()
			modifier.name = "Turn"
			skeleton.add_child(modifier)
	add_child(skeleton)
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	for corner: Vector3 in [Vector3(1.0, 0.0, 0.0), Vector3(2.0, 0.0, 0.0), Vector3(1.0, 1.0, 0.0)]:
		st.set_bones(PackedInt32Array([0, 0, 0, 0]))
		st.set_weights(PackedFloat32Array([1.0, 0.0, 0.0, 0.0]))
		st.add_vertex(corner)
	var skin := Skin.new()
	skin.add_bind(0, Transform3D())
	var skinned := MeshInstance3D.new()
	skinned.name = "Skinned"
	skinned.mesh = st.commit()
	skinned.material_override = material
	skinned.skin = null if _case == "auto_skin" else skin
	skinned.skeleton = NodePath("../Skeleton")
	add_child(skinned)


func _process(_delta: float) -> void:
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, -1)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	var perturb := int(stepped["frame"]) == PERTURB_FRAME
	if perturb == _perturbed:
		return
	_perturbed = perturb
	match _case:
		"light_returns":
			_light.position = Vector3(3.0, 2.0, 2.0) if perturb else Vector3(2.0, 2.0, 2.0)
		"mesh_blinks":
			_box.visible = not perturb
		"multimesh_returns":
			var moved := _rock_transform.translated(Vector3(0.0, 1.0, 0.0))
			_rocks.multimesh.set_instance_transform(1, moved if perturb else _rock_transform)
		"gridmap_returns":
			# The same number of cells, one of them elsewhere for one frame.
			_grid.set_cell_item(Vector3i(1, 0, 0), -1 if perturb else 0)
			_grid.set_cell_item(Vector3i(1, 1, 0), 0 if perturb else -1)
