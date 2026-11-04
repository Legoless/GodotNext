extends Node3D
## Spawns the seeded NextGI Perf instances and walks the camera spline (#33).
## The packed scene is the shell only. Instances are never saved.

const Layout = preload("res://scenes/nextgi_perf/layout.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")

var _hold := -1
var _local_frame := 0
var _saw := false


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
	print("nextgi_perf: drop when distance > %.1f m and angular radius < %.1f deg (box radius %.6f m)" % [
		Layout.MIN_DISTANCE_M, Layout.MAX_ANGLE_DEG, Layout.box_radius()])
	var culled := Layout.spline_cull_counts(positions, Layout.box_radius())
	for frame in culled.size():
		print("nextgi_perf: spline_camera frame=%d culled=%d considered=%d" % [frame, int(culled[frame]), positions.size()])
	_apply(0 if _hold < 0 else _hold)


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, _hold)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	_apply(int(stepped["frame"]))


func _apply(p_frame: int) -> void:
	var camera := get_node_or_null("CaptureCam") as Camera3D
	if camera == null:
		return
	var frame := clampi(p_frame, 0, Layout.SPLINE_FRAMES - 1)
	camera.transform = Transform3D(Layout.spline_basis(frame), Layout.spline_position(frame))
	camera.set_meta("spline_frame", frame)
