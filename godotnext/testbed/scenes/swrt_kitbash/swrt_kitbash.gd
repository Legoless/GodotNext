extends Node3D
## Spawns the seeded overlap-heavy kitbash instances (#181). The packed
## scene is the shell only; instances are never saved.

const Layout = preload("res://scenes/swrt_kitbash/layout.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	process_priority = 1
	var count := SceneArgs.int_value("--instances", Layout.DEFAULT_INSTANCES)
	if count < 1:
		count = Layout.DEFAULT_INSTANCES
	var planned: Dictionary = Layout.plan(count, Layout.SEED)
	var spawned: Dictionary = Layout.spawn(self, planned)
	var positions: PackedVector3Array = planned["positions"]
	print("swrt_kitbash: instances mesh=%d multimesh=%d total=%d seed=%d" % [
		int(spawned["mesh_nodes"]), int(spawned["multimesh_instances"]),
		int(spawned["total"]), Layout.SEED,
	])
	print("swrt_kitbash: positions_sha256=%s" % Layout.positions_sha256(positions))
	print("swrt_kitbash: overlap_pairs=%d max_radius_m=%.3f" % [
		Layout.overlap_pairs(planned), Layout.max_field_radius_m(planned),
	])
