extends Node3D
## A small scene for the UE exporter's per-row refusals (run_ue_export_check.sh). It holds a box, a
## MultiMesh of three boxes, an omni light and a one-bone skinned triangle in front of a camera, all
## still, and exports with --approximate=rigid_skin. --case=<name> breaks one thing:
##
##   light_returns      the light moves away on frame 8 and is back on frame 9
##   mesh_blinks        the box is hidden on frame 8 only
##   multimesh_returns  one MultiMesh instance moves on frame 8 and is back on frame 9
##   skeleton_offset    the skinned triangle's Skeleton3D sits 0.5 m beside the mesh instance
##   camera_cull        the camera's cull mask hides the box's layer
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


func _init() -> void:
	_case = SceneArgs.value("--case", "none")
	var material := StandardMaterial3D.new()
	material.diffuse_mode = BaseMaterial3D.DIFFUSE_LAMBERT

	var camera := Camera3D.new()
	camera.name = "Camera"
	camera.position = Vector3(0.0, 1.0, 6.0)
	if _case == "camera_cull":
		camera.cull_mask = 0xFFFFF & ~BOX_LAYER
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

	_light = OmniLight3D.new()
	_light.name = "Light"
	_light.position = Vector3(2.0, 2.0, 2.0)
	add_child(_light)

	var skeleton := Skeleton3D.new()
	skeleton.name = "Skeleton"
	skeleton.add_bone("root")
	skeleton.set_bone_rest(0, Transform3D())
	skeleton.position = Vector3(0.5, 0.0, 0.0) if _case == "skeleton_offset" else Vector3.ZERO
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
	skinned.skin = skin
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
