extends Node3D
## Spawns the seeded NextGI Perf instances and walks the camera spline (#33).
## The packed scene is the shell only. Instances are never saved.

const Layout = preload("res://scenes/nextgi_perf/layout.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")
const Character = preload("res://scenes/character_walk/character.gd")

var _hold := -1
var _limit := -1
var _local_frame := 0
var _saw := false
var _p3 := false
var _actors: Array[Node3D] = []


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var world := get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null and bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false)):
		var environment := world.environment.duplicate()
		environment.nextgi_enabled = true
		environment.sdfgi_enabled = false
		world.environment = environment
	process_priority = 1
	_hold = SceneArgs.int_value("--hold-frame", -1)
	_limit = SceneArgs.int_value("--frames", -1)
	var count := SceneArgs.int_value("--instances", Layout.DEFAULT_INSTANCES)
	if count < 1:
		count = Layout.DEFAULT_INSTANCES
	var planned: Dictionary = Layout.plan(count, Layout.SEED)
	var spawned: Dictionary = Layout.spawn(self, planned)
	var positions: PackedVector3Array = planned["positions"]
	print("nextgi_perf: instances mesh=%d multimesh=%d gridmap=%d total=%d seed=%d" % [
		int(spawned["mesh_nodes"]),
		int(spawned["multimesh_instances"]),
		int(spawned["grid_cells"]),
		positions.size(),
		Layout.SEED,
	])
	print("nextgi_perf: positions_sha256=%s" % Layout.positions_sha256(positions))
	_p3 = SceneArgs.value("--variant", "") == "p3"
	if _p3:
		var holder := Node3D.new()
		holder.name = "P3Characters"
		add_child(holder)
		var furthest := 0.0
		for index in Character.PERF_COUNT:
			var actor := Character.make_character("PerfCharacter%d" % index)
			holder.add_child(actor)
			_actors.append(actor)
			for frame in Layout.SPLINE_FRAMES:
				furthest = maxf(furthest, Layout.spline_position(frame).distance_to(Character.perf_position(index, frame)))
		print("nextgi_perf: p3 characters=%d triangles=%d max_distance=%.3f" % [
			_actors.size(), Character.triangle_count(), furthest,
		])
	print("nextgi_perf: drop when distance > %.1f m and angular radius < %.1f deg (box radius %.6f m)" % [
		Layout.MIN_DISTANCE_M, Layout.MAX_ANGLE_DEG, Layout.box_radius()])
	var culled := Layout.spline_cull_counts(positions, Layout.box_radius())
	for frame in culled.size():
		print("nextgi_perf: spline_camera frame=%d culled=%d considered=%d" % [frame, int(culled[frame]), positions.size()])
	var first_frame := 0 if _hold < 0 else _hold
	if _p3:
		call_deferred("_apply", first_frame)
	else:
		_apply(first_frame)


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, _hold)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	_apply(int(stepped["frame"]))
	if _limit >= 0 and _local_frame >= _limit:
		get_tree().quit(0)


func _apply(p_frame: int) -> void:
	var camera := get_node_or_null("CaptureCam") as Camera3D
	if camera == null:
		return
	var frame := clampi(p_frame, 0, Layout.SPLINE_FRAMES - 1)
	camera.transform = Transform3D(Layout.spline_basis(frame), Layout.spline_position(frame))
	camera.set_meta("spline_frame", frame)
	if _p3:
		for index in _actors.size():
			var actor := _actors[index]
			actor.position = Character.perf_position(index, frame)
			var travel := Layout.spline_position(frame) - Layout.spline_position(maxi(frame - 1, 0))
			if travel.length_squared() > 1e-8:
				actor.basis = Basis.looking_at(travel, Vector3.UP)
			Character.apply_walk_frame(actor, frame, index * 7)
