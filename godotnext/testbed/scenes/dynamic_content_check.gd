extends SceneTree
## Headless checks for the #33 motion contracts and the NextGI Perf layout.
## No window and no external tool.


const DoorMotion = preload("res://scenes/moving_light_door/motion.gd")
const SunMotion = preload("res://scenes/time_of_day/motion.gd")
const Layout = preload("res://scenes/nextgi_perf/layout.gd")

var _failures: Array[String] = []


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	_check(String(DoorMotion.contract_error()).is_empty(), String(DoorMotion.contract_error()))
	_check(DoorMotion.first_leak_frame() == 3, "first leak frame")
	_check(is_equal_approx(DoorMotion.door_gap_m("door_open", 0), 0.0), "gap 0")
	_check(is_equal_approx(DoorMotion.door_gap_m("door_open", 60), 0.5), "gap 60")
	_check(is_equal_approx(DoorMotion.door_gap_m("door_open", 120), 1.0), "gap 120")
	_check(is_equal_approx(DoorMotion.door_gap_m("door_teleport", 0), 1.0), "teleport")
	_check(is_equal_approx(DoorMotion.door_gap_m("light_toggle", 240), 0.0), "toggle door stays shut")
	_check(SunMotion.contract_error().is_empty(), String(SunMotion.contract_error()))
	_check(is_equal_approx(SunMotion.elevation_deg("sun_jump", 0) - SunMotion.BASE_ELEVATION_DEG, 15.0), "jump is 15")
	_check(is_equal_approx(SunMotion.elevation_deg("sweep", 0), 10.0), "sweep frame 0")
	_check(is_equal_approx(SunMotion.elevation_deg("sweep", 300), 55.0), "sweep frame 300")
	_check(is_equal_approx(SunMotion.elevation_deg("sweep", 600), 100.0), "sweep frame 600")
	_check(Layout.contract_error().is_empty(), String(Layout.contract_error()))
	_check_plan(50000)
	_check_plan(100000)
	_check_spawn(180)
	if _failures.is_empty():
		print("dynamic_content_check: ok")
		quit(0)
	else:
		for failure in _failures:
			push_error("dynamic_content_check: " + failure)
		quit(1)


func _check_plan(p_count: int) -> void:
	var first: Dictionary = Layout.plan(p_count, Layout.SEED)
	var second: Dictionary = Layout.plan(p_count, Layout.SEED)
	_check(int(first["count"]) == p_count, "plan count %d" % p_count)
	_check(int(first["mesh"]) + int(first["multimesh"]) + int(first["gridmap"]) == p_count, "shares sum %d" % p_count)
	_check(int(first["mesh"]) > int(first["multimesh"]), "mesh majority %d" % p_count)
	var hash_a := Layout.positions_sha256(first["positions"])
	var hash_b := Layout.positions_sha256(second["positions"])
	_check(hash_a == hash_b, "plan hash stable at %d" % p_count)
	var left: PackedVector3Array = first["positions"]
	var right: PackedVector3Array = second["positions"]
	var same := left.size() == right.size()
	if same:
		for index in left.size():
			if left[index] != right[index]:
				same = false
				break
	_check(same, "plan positions stable at %d" % p_count)


func _check_spawn(p_count: int) -> void:
	var planned: Dictionary = Layout.plan(p_count, Layout.SEED)
	var host := Node3D.new()
	get_root().add_child(host)
	var spawned: Dictionary = Layout.spawn(host, planned)
	_check(int(spawned["mesh_nodes"]) == int(planned["mesh"]), "spawned mesh nodes")
	_check(int(spawned["multimesh_instances"]) == int(planned["multimesh"]), "spawned multimesh")
	_check(int(spawned["grid_cells"]) == int(planned["gridmap"]), "spawned grid cells")
	var seen := {}
	var roughness: PackedFloat32Array = planned["roughness"]
	for value in roughness:
		seen[snappedf(float(value), 0.0001)] = true
	_check(seen.size() == Layout.ROUGHNESS.size(), "roughness set covers the table")
	for value in Layout.ROUGHNESS:
		_check(seen.has(snappedf(value, 0.0001)), "roughness %s present" % value)
	host.free()


func _check(p_ok: bool, p_message: String) -> void:
	if p_ok:
		return
	if p_message.is_empty():
		p_message = "check failed"
	_failures.append(p_message)
	print("FAIL: " + p_message)
