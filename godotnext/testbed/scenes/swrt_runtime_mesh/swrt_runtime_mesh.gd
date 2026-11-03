extends Node3D
## Builds the 20k-triangle procedural mesh at t = 1 s and moves it at
## t = 3 s (#181, GS-6), driven by the frame index.

const Spec = preload("res://scenes/swrt_runtime_mesh/spec.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")
const Scaffold := preload("res://generators/scaffold.gd")

var _hold := -1
var _local_frame := 0
var _saw := false
var _node: MeshInstance3D
var _moved := false


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	process_priority = 1
	_hold = SceneArgs.int_value("--hold-frame", -1)
	print("swrt_runtime_mesh: hold_frame=%d build_frame=%d move_frame=%d" % [
		_hold, Spec.BUILD_FRAME, Spec.MOVE_FRAME,
	])
	_apply(0 if _hold < 0 else _hold)


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, _hold)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	_apply(int(stepped["frame"]))


func _apply(p_frame: int) -> void:
	if Spec.is_built(p_frame) and _node == null:
		_node = MeshInstance3D.new()
		_node.name = "RuntimeMesh"
		_node.mesh = Spec.build_mesh(
				Scaffold.make_constant_material("RuntimeMesh", Color(0.2, 0.55, 0.75)))
		_node.position = Spec.position_for_frame(p_frame)
		_node.gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC
		add_child(_node)
		print("swrt_runtime_mesh: built %d triangles at frame %d (t=%.3f s)" % [
			Spec.triangle_count(), p_frame, float(p_frame) / Spec.FPS,
		])
	if _node != null and p_frame >= Spec.MOVE_FRAME and not _moved:
		_moved = true
		_node.position = Spec.POSITION_B
		print("swrt_runtime_mesh: moved to %s at frame %d (t=%.3f s)" % [
			str(Spec.POSITION_B), p_frame, float(p_frame) / Spec.FPS,
		])
