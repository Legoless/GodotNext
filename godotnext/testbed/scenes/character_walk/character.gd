extends RefCounted

const Scaffold := preload("res://generators/scaffold.gd")
## Original skinned walker for Character Walk and NextGI Perf (#52).
## About 10k triangles so twenty copies are 200k skinned triangles.
## No external mesh: the surface, skeleton and walk cycle are built here.


const TRIANGLES := 10000
const SOAK_COUNT := 20
const TARGET_SOAK_TRIANGLES := 200000
const SOAK_TOLERANCE := 0.05
const HEIGHT_M := 1.8
const WALK_CYCLE_SEC := 1.0
const PATH_FRAMES := 240
const PATH_START := Vector3(3.6, 0.0, 0.0)
const PATH_END := Vector3(0.4, 0.0, 0.0)
const WALL_X := 2.0
const VACATED_X := 2.375
const POSE_FRAMES: Array[int] = [24, 72, 120, 168, 216]
const PERF_SEED := 52
const PERF_COUNT := 4

const BONE_HIPS := 0
const BONE_SPINE := 1
const BONE_CHEST := 2
const BONE_HEAD := 3
const BONE_UARM_L := 4
const BONE_LARM_L := 5
const BONE_UARM_R := 6
const BONE_LARM_R := 7
const BONE_THIGH_L := 8
const BONE_SHIN_L := 9
const BONE_THIGH_R := 10
const BONE_SHIN_R := 11

static var _mesh: ArrayMesh
static var _skin: Skin
static var _animation: Animation
static var _mesh_scaled := {}


static func contract_error() -> String:
	var mesh := shared_mesh()
	var triangles := mesh.get_faces().size() / 3
	if triangles != TRIANGLES:
		return "character has %d triangles, expected %d." % [triangles, TRIANGLES]
	var soaked := triangles * SOAK_COUNT
	var low := int(round(float(TARGET_SOAK_TRIANGLES) * (1.0 - SOAK_TOLERANCE)))
	var high := int(round(float(TARGET_SOAK_TRIANGLES) * (1.0 + SOAK_TOLERANCE)))
	if soaked < low or soaked > high:
		return "soak_20 has %d skinned triangles, expected %d ±5%%." % [soaked, TARGET_SOAK_TRIANGLES]
	if shared_skin().get_bind_count() != 12:
		return "character skin does not have 12 binds."
	if shared_animation().length != WALK_CYCLE_SEC:
		return "walk cycle is not 1 second."
	var furthest := 0.0
	for frame in 600:
		var camera := _perf_camera(frame)
		for index in PERF_COUNT:
			furthest = maxf(furthest, camera.distance_to(perf_position(index, frame)))
	if furthest > 30.0:
		return "a NextGI Perf character is %.2f m from the spline camera." % furthest
	if path_position(0).x <= WALL_X or path_position(PATH_FRAMES - 1).x >= WALL_X:
		return "the walk does not cross the wall."
	return ""


static func shared_mesh() -> ArrayMesh:
	_ensure()
	return _mesh


## A denser body for the PN Character Crowd (#148): every body box is
## subdivided by [param p_segment_scale] per axis, so 8 gives about 561k
## triangles with the same skeleton and skin bindings. Cached per scale.
static func shared_mesh_scaled(p_segment_scale: int) -> ArrayMesh:
	if p_segment_scale <= 1:
		return shared_mesh()
	if not _mesh_scaled.has(p_segment_scale):
		_mesh_scaled[p_segment_scale] = _build_mesh(p_segment_scale, false)["mesh"]
	return _mesh_scaled[p_segment_scale]


static func shared_skin() -> Skin:
	_ensure()
	return _skin


static func shared_animation() -> Animation:
	_ensure()
	return _animation


static func triangle_count() -> int:
	return shared_mesh().get_faces().size() / 3


static func path_position(p_frame: int) -> Vector3:
	var frame := clampi(p_frame, 0, PATH_FRAMES)
	var t := float(frame) / float(PATH_FRAMES)
	return PATH_START.lerp(PATH_END, t)


static func pose_frame(p_index: int) -> int:
	return POSE_FRAMES[clampi(p_index, 0, POSE_FRAMES.size() - 1)]


static func perf_position(p_index: int, p_frame: int) -> Vector3:
	var Layout := preload("res://scenes/nextgi_perf/layout.gd")
	var frame := clampi(p_frame, 0, 599)
	var basis: Basis = Layout.spline_basis(frame)
	var camera: Vector3 = Layout.spline_position(frame)
	var offset: Vector2 = _perf_offsets()[p_index]
	return Vector3(camera.x, 0.0, camera.z) + basis.x * offset.x + (-basis.z) * offset.y


static func make_character(p_name: String, p_mesh: ArrayMesh = null) -> Node3D:
	var root := Node3D.new()
	root.name = p_name
	var skeleton := Skeleton3D.new()
	skeleton.name = "Skeleton3D"
	_build_skeleton(skeleton)
	root.add_child(skeleton)
	var visual := MeshInstance3D.new()
	visual.name = "Visual"
	visual.mesh = p_mesh if p_mesh != null else shared_mesh()
	visual.skin = shared_skin()
	visual.skeleton = NodePath("../Skeleton3D")
	var material := Scaffold.make_constant_material("Walker", Color(0.62, 0.48, 0.38), 0.9, 0.0)
	visual.set_surface_override_material(0, material)
	root.add_child(visual)
	var player := AnimationPlayer.new()
	player.name = "AnimationPlayer"
	var library := AnimationLibrary.new()
	library.add_animation(&"walk", shared_animation())
	player.add_animation_library(&"", library)
	player.assigned_animation = &"walk"
	root.add_child(player)
	root.set_meta("triangles", visual.mesh.get_faces().size() / 3)
	return root


static func apply_walk_frame(p_character: Node3D, p_frame: int, p_phase: int = 0) -> void:
	var player := p_character.get_node_or_null("AnimationPlayer") as AnimationPlayer
	if player == null:
		return
	var time := fmod(float(p_frame + p_phase) / 60.0, WALK_CYCLE_SEC)
	if time < 0.0:
		time += WALK_CYCLE_SEC
	player.play(&"walk")
	player.seek(time, true)
	player.pause()


static func surface_points(p_count: int) -> PackedVector3Array:
	var faces := shared_mesh().get_faces()
	var points := PackedVector3Array()
	var triangles := faces.size() / 3
	if triangles <= 0 or p_count <= 0:
		return points
	var stride := maxi(int(triangles / p_count), 1)
	for index in p_count:
		var tri := (index * stride) % triangles
		var a := faces[tri * 3]
		var b := faces[tri * 3 + 1]
		var c := faces[tri * 3 + 2]
		var center := (a + b + c) / 3.0
		if center.y < 0.4:
			continue
		points.append(center)
		if points.size() >= p_count:
			break
	return points


static func _ensure() -> void:
	if _mesh != null:
		return
	var built := _build_mesh()
	_mesh = built["mesh"]
	_skin = built["skin"]
	_animation = _build_animation()


static func _perf_offsets() -> Array[Vector2]:
	var rng := RandomNumberGenerator.new()
	rng.seed = PERF_SEED
	var offsets: Array[Vector2] = []
	for index in PERF_COUNT:
		var side := 1.0 if index < 2 else -1.0
		offsets.append(Vector2(side * rng.randf_range(1.4, 2.8), rng.randf_range(-1.2, 1.6)))
	return offsets


static func _perf_camera(p_frame: int) -> Vector3:
	var Layout := preload("res://scenes/nextgi_perf/layout.gd")
	return Layout.spline_position(clampi(p_frame, 0, 599))


static func _build_skeleton(p_skeleton: Skeleton3D) -> void:
	var names: PackedStringArray = [
		"hips", "spine", "chest", "head",
		"upper_arm_l", "lower_arm_l", "upper_arm_r", "lower_arm_r",
		"thigh_l", "shin_l", "thigh_r", "shin_r",
	]
	var parents: Array[int] = [-1, 0, 1, 2, 2, 4, 2, 6, 0, 8, 0, 10]
	var origins: Array[Vector3] = [
		Vector3(0.0, 0.92, 0.0),
		Vector3(0.0, 0.14, 0.0),
		Vector3(0.0, 0.20, 0.0),
		Vector3(0.0, 0.28, 0.0),
		Vector3(-0.22, 0.16, 0.0),
		Vector3(0.0, -0.28, 0.0),
		Vector3(0.22, 0.16, 0.0),
		Vector3(0.0, -0.28, 0.0),
		Vector3(-0.09, -0.02, 0.0),
		Vector3(0.0, -0.40, 0.0),
		Vector3(0.09, -0.02, 0.0),
		Vector3(0.0, -0.40, 0.0),
	]
	for index in names.size():
		p_skeleton.add_bone(names[index])
	for index in names.size():
		if parents[index] >= 0:
			p_skeleton.set_bone_parent(index, parents[index])
		p_skeleton.set_bone_rest(index, Transform3D(Basis.IDENTITY, origins[index]))
	p_skeleton.reset_bone_poses()


static func _build_animation() -> Animation:
	var animation := Animation.new()
	animation.length = WALK_CYCLE_SEC
	animation.loop_mode = Animation.LOOP_LINEAR
	_swing(animation, "thigh_l", 26.0, 0.0)
	_swing(animation, "thigh_r", 26.0, 0.5)
	_swing(animation, "shin_l", 18.0, 0.5)
	_swing(animation, "shin_r", 18.0, 0.0)
	_swing(animation, "upper_arm_l", 20.0, 0.5)
	_swing(animation, "upper_arm_r", 20.0, 0.0)
	return animation


static func _swing(p_animation: Animation, p_bone: String, p_degrees: float, p_phase: float) -> void:
	var track := p_animation.add_track(Animation.TYPE_ROTATION_3D)
	p_animation.track_set_path(track, NodePath("Skeleton3D:" + p_bone))
	p_animation.track_set_interpolation_type(track, Animation.INTERPOLATION_LINEAR)
	var amplitude := deg_to_rad(p_degrees)
	for step in 5:
		var u := float(step) / 4.0
		var angle := sin((u + p_phase) * TAU) * amplitude
		p_animation.rotation_track_insert_key(track, u * WALK_CYCLE_SEC, Quaternion(Vector3.RIGHT, angle))


static func _build_mesh(p_segment_scale: int = 1, p_fill: bool = true) -> Dictionary:
	var skin := Skin.new()
	var parents: Array[int] = [-1, 0, 1, 2, 2, 4, 2, 6, 0, 8, 0, 10]
	var origins: Array[Vector3] = [
		Vector3(0.0, 0.92, 0.0), Vector3(0.0, 0.14, 0.0), Vector3(0.0, 0.20, 0.0), Vector3(0.0, 0.28, 0.0),
		Vector3(-0.22, 0.16, 0.0), Vector3(0.0, -0.28, 0.0), Vector3(0.22, 0.16, 0.0), Vector3(0.0, -0.28, 0.0),
		Vector3(-0.09, -0.02, 0.0), Vector3(0.0, -0.40, 0.0), Vector3(0.09, -0.02, 0.0), Vector3(0.0, -0.40, 0.0),
	]
	var global: Array[Transform3D] = []
	global.resize(origins.size())
	for index in origins.size():
		var local := Transform3D(Basis.IDENTITY, origins[index])
		global[index] = local if parents[index] < 0 else global[parents[index]] * local
		skin.add_bind(index, global[index].affine_inverse())
	var tool := SurfaceTool.new()
	tool.begin(Mesh.PRIMITIVE_TRIANGLES)
	var tris := 0
	tris += _add_box(tool, global[BONE_HIPS] * Vector3(0.0, 0.02, 0.0), Vector3(0.28, 0.16, 0.18), Vector3i(6 * p_segment_scale, 3 * p_segment_scale, 4 * p_segment_scale), BONE_HIPS)
	tris += _add_box(tool, global[BONE_SPINE] * Vector3(0.0, 0.08, 0.0), Vector3(0.24, 0.16, 0.16), Vector3i(4 * p_segment_scale, 4 * p_segment_scale, 3 * p_segment_scale), BONE_SPINE)
	tris += _add_box(tool, global[BONE_CHEST] * Vector3(0.0, 0.08, 0.0), Vector3(0.34, 0.28, 0.18), Vector3i(14 * p_segment_scale, 22 * p_segment_scale, 10 * p_segment_scale), BONE_CHEST)
	tris += _add_box(tool, global[BONE_HEAD] * Vector3(0.0, 0.12, 0.0), Vector3(0.18, 0.22, 0.18), Vector3i(6 * p_segment_scale, 6 * p_segment_scale, 6 * p_segment_scale), BONE_HEAD)
	tris += _add_box(tool, global[BONE_UARM_L] * Vector3(0.0, -0.14, 0.0), Vector3(0.08, 0.26, 0.08), Vector3i(5 * p_segment_scale, 12 * p_segment_scale, 5 * p_segment_scale), BONE_UARM_L)
	tris += _add_box(tool, global[BONE_LARM_L] * Vector3(0.0, -0.13, 0.0), Vector3(0.07, 0.24, 0.07), Vector3i(4 * p_segment_scale, 10 * p_segment_scale, 4 * p_segment_scale), BONE_LARM_L)
	tris += _add_box(tool, global[BONE_UARM_R] * Vector3(0.0, -0.14, 0.0), Vector3(0.08, 0.26, 0.08), Vector3i(5 * p_segment_scale, 12 * p_segment_scale, 5 * p_segment_scale), BONE_UARM_R)
	tris += _add_box(tool, global[BONE_LARM_R] * Vector3(0.0, -0.13, 0.0), Vector3(0.07, 0.24, 0.07), Vector3i(4 * p_segment_scale, 10 * p_segment_scale, 4 * p_segment_scale), BONE_LARM_R)
	tris += _add_box(tool, global[BONE_THIGH_L] * Vector3(0.0, -0.20, 0.0), Vector3(0.12, 0.38, 0.12), Vector3i(6 * p_segment_scale, 14 * p_segment_scale, 6 * p_segment_scale), BONE_THIGH_L)
	tris += _add_box(tool, global[BONE_SHIN_L] * Vector3(0.0, -0.18, 0.0), Vector3(0.09, 0.36, 0.09), Vector3i(5 * p_segment_scale, 12 * p_segment_scale, 5 * p_segment_scale), BONE_SHIN_L)
	tris += _add_box(tool, global[BONE_THIGH_R] * Vector3(0.0, -0.20, 0.0), Vector3(0.12, 0.38, 0.12), Vector3i(6 * p_segment_scale, 14 * p_segment_scale, 6 * p_segment_scale), BONE_THIGH_R)
	tris += _add_box(tool, global[BONE_SHIN_R] * Vector3(0.0, -0.18, 0.0), Vector3(0.09, 0.36, 0.09), Vector3i(5 * p_segment_scale, 12 * p_segment_scale, 5 * p_segment_scale), BONE_SHIN_R)
	tris += _add_box(tool, global[BONE_UARM_L] * Vector3(0.0, -0.28, 0.0), Vector3(0.06, 0.06, 0.06), Vector3i(3 * p_segment_scale, 3 * p_segment_scale, 3 * p_segment_scale), BONE_LARM_L)
	tris += _add_box(tool, global[BONE_UARM_R] * Vector3(0.0, -0.28, 0.0), Vector3(0.06, 0.06, 0.06), Vector3i(3 * p_segment_scale, 3 * p_segment_scale, 3 * p_segment_scale), BONE_LARM_R)
	tris += _add_box(tool, global[BONE_SHIN_L] * Vector3(0.0, -0.38, 0.06), Vector3(0.10, 0.06, 0.18), Vector3i(4 * p_segment_scale, 2 * p_segment_scale, 6 * p_segment_scale), BONE_SHIN_L)
	tris += _add_box(tool, global[BONE_SHIN_R] * Vector3(0.0, -0.38, 0.06), Vector3(0.10, 0.06, 0.18), Vector3i(4 * p_segment_scale, 2 * p_segment_scale, 6 * p_segment_scale), BONE_SHIN_R)
	var column := 0
	var row := 0
	while p_fill and tris < TRIANGLES:
		var center := global[BONE_CHEST] * Vector3(-0.12 + float(column) * 0.012, 0.02 + float(row) * 0.012, 0.10)
		_add_quad(tool, center, Vector3(0.005, 0.0, 0.0), Vector3(0.0, 0.005, 0.0), Vector3(0.0, 0.0, 1.0), BONE_CHEST)
		tris += 2
		column += 1
		if column >= 24:
			column = 0
			row += 1
	var mesh := tool.commit()
	return {"mesh": mesh, "skin": skin}


static func _add_box(p_tool: SurfaceTool, p_center: Vector3, p_size: Vector3, p_seg: Vector3i, p_bone: int) -> int:
	var hx := p_size.x * 0.5
	var hy := p_size.y * 0.5
	var hz := p_size.z * 0.5
	var nx := maxi(p_seg.x, 1)
	var ny := maxi(p_seg.y, 1)
	var nz := maxi(p_seg.z, 1)
	_add_grid(p_tool, p_center + Vector3(hx, 0.0, 0.0), Vector3(0.0, 0.0, -hz), Vector3(0.0, hy, 0.0), Vector3.RIGHT, nz, ny, p_bone)
	_add_grid(p_tool, p_center + Vector3(-hx, 0.0, 0.0), Vector3(0.0, 0.0, hz), Vector3(0.0, hy, 0.0), Vector3.LEFT, nz, ny, p_bone)
	_add_grid(p_tool, p_center + Vector3(0.0, hy, 0.0), Vector3(hx, 0.0, 0.0), Vector3(0.0, 0.0, -hz), Vector3.UP, nx, nz, p_bone)
	_add_grid(p_tool, p_center + Vector3(0.0, -hy, 0.0), Vector3(hx, 0.0, 0.0), Vector3(0.0, 0.0, hz), Vector3.DOWN, nx, nz, p_bone)
	_add_grid(p_tool, p_center + Vector3(0.0, 0.0, hz), Vector3(hx, 0.0, 0.0), Vector3(0.0, hy, 0.0), Vector3(0.0, 0.0, 1.0), nx, ny, p_bone)
	_add_grid(p_tool, p_center + Vector3(0.0, 0.0, -hz), Vector3(-hx, 0.0, 0.0), Vector3(0.0, hy, 0.0), Vector3(0.0, 0.0, -1.0), nx, ny, p_bone)
	return 4 * (ny * nz + nx * nz + nx * ny)


static func _add_grid(
		p_tool: SurfaceTool, p_center: Vector3, p_u: Vector3, p_v: Vector3, p_normal: Vector3,
		p_nu: int, p_nv: int, p_bone: int) -> void:
	for iu in p_nu:
		for iv in p_nv:
			var u0 := -1.0 + 2.0 * float(iu) / float(p_nu)
			var u1 := -1.0 + 2.0 * float(iu + 1) / float(p_nu)
			var v0 := -1.0 + 2.0 * float(iv) / float(p_nv)
			var v1 := -1.0 + 2.0 * float(iv + 1) / float(p_nv)
			var a := p_center + p_u * u0 + p_v * v0
			var b := p_center + p_u * u1 + p_v * v0
			var c := p_center + p_u * u1 + p_v * v1
			var d := p_center + p_u * u0 + p_v * v1
			_vert(p_tool, a, p_normal, p_bone)
			_vert(p_tool, c, p_normal, p_bone)
			_vert(p_tool, b, p_normal, p_bone)
			_vert(p_tool, a, p_normal, p_bone)
			_vert(p_tool, d, p_normal, p_bone)
			_vert(p_tool, c, p_normal, p_bone)


static func _add_quad(p_tool: SurfaceTool, p_center: Vector3, p_u: Vector3, p_v: Vector3, p_normal: Vector3, p_bone: int) -> void:
	var a := p_center - p_u - p_v
	var b := p_center + p_u - p_v
	var c := p_center + p_u + p_v
	var d := p_center - p_u + p_v
	_vert(p_tool, a, p_normal, p_bone)
	_vert(p_tool, c, p_normal, p_bone)
	_vert(p_tool, b, p_normal, p_bone)
	_vert(p_tool, a, p_normal, p_bone)
	_vert(p_tool, d, p_normal, p_bone)
	_vert(p_tool, c, p_normal, p_bone)


static func _vert(p_tool: SurfaceTool, p_position: Vector3, p_normal: Vector3, p_bone: int) -> void:
	p_tool.set_normal(p_normal)
	p_tool.set_uv(Vector2(p_position.x, p_position.y))
	p_tool.set_bones(PackedInt32Array([p_bone, 0, 0, 0]))
	p_tool.set_weights(PackedFloat32Array([1.0, 0.0, 0.0, 0.0]))
	p_tool.add_vertex(p_position)
