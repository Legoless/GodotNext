extends SceneTree
## Builds every P0 testbed scene and its patch JSON (#10 steps 3-8).
##
## Run from the repository root:
## [code]godot --headless --path godotnext/testbed --script res://generators/build_all.gd[/code]
##
## Seeded and byte-deterministic: two runs produce identical [code].tscn[/code]
## and [code].json[/code] output. The script asserts the content-spec counts
## (128 line-of-sight-verified Q1 patches per quality scene, 16+16 Q3
## patches per Color Box Room variant, the off-screen light outside every
## camera frustum and the 40 m single-mesh bounds) and exits non-zero on
## any failure, without writing the failing scene.

const Scaffold := preload("res://generators/scaffold.gd")
const ColorBoxRoom := preload("res://generators/build_color_box_room.gd")
const Single40m := preload("res://generators/build_single_40m.gd")
const Courtyard := preload("res://generators/build_courtyard.gd")
const Legacy := preload("res://generators/build_legacy.gd")
const MovingLightDoor := preload("res://generators/build_moving_light_door.gd")
const TimeOfDay := preload("res://generators/build_time_of_day.gd")
const NextGIPerf := preload("res://generators/build_nextgi_perf.gd")
const DoorMotion = preload("res://scenes/moving_light_door/motion.gd")
const SunMotion = preload("res://scenes/time_of_day/motion.gd")
const PerfLayout = preload("res://scenes/nextgi_perf/layout.gd")

const Q1_PATCH_COUNT := 128

var _failures: Array[String] = []


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	_check_contracts()
	var color_box_dir := "res://scenes/color_box_room"
	var jobs: Array[Dictionary] = [
		{"built": ColorBoxRoom.build("a"), "scene": color_box_dir + "/cbr_a.tscn", "stream": 1},
		{"built": ColorBoxRoom.build("b"), "scene": color_box_dir + "/cbr_b.tscn", "stream": 2},
		{"built": ColorBoxRoom.build("offscreen"), "scene": color_box_dir + "/cbr_offscreen.tscn", "stream": 3},
		{"built": ColorBoxRoom.build("lightmap"), "scene": color_box_dir + "/cbr_b_lightmap.tscn", "stream": 4},
		{"built": Single40m.build(), "scene": color_box_dir + "/cbr_single_40m.tscn", "stream": 5},
		{"built": Courtyard.build(), "scene": "res://scenes/concave_courtyard/courtyard.tscn", "stream": 6},
		{"built": Legacy.build_sdfgi(), "scene": "res://scenes/legacy/legacy_sdfgi.tscn", "stream": 7},
		{"built": Legacy.build_voxelgi(), "scene": "res://scenes/legacy/legacy_voxelgi.tscn", "stream": 8},
		{"built": Legacy.build_lightmap(), "scene": "res://scenes/legacy/legacy_lightmap.tscn", "stream": 9},
	]
	for job in jobs:
		await _process_job(job)
	var dynamic_jobs: Array[Dictionary] = [
		{"built": MovingLightDoor.build(), "scene": "res://scenes/moving_light_door/moving_light_door.tscn", "stream": 33},
		{"built": TimeOfDay.build(), "scene": "res://scenes/time_of_day/time_of_day.tscn", "stream": 34},
		{"built": NextGIPerf.build(), "scene": "res://scenes/nextgi_perf/nextgi_perf.tscn", "stream": 35},
	]
	for job in dynamic_jobs:
		await _process_job(job)
	if _failures.is_empty():
		print("build_all: all scenes built and verified.")
		quit(0)
	else:
		for failure in _failures:
			push_error("build_all: " + failure)
		quit(1)


func _fail(p_message: String) -> void:
	_failures.append(p_message)
	printerr("build_all FAIL: " + p_message)


func _check_contracts() -> void:
	var door_err := String(DoorMotion.contract_error())
	if not door_err.is_empty():
		_fail(door_err)
	var sun_err := String(SunMotion.contract_error())
	if not sun_err.is_empty():
		_fail(sun_err)
	var perf_err := String(PerfLayout.contract_error())
	if not perf_err.is_empty():
		_fail(perf_err)


func _process_job(p_job: Dictionary) -> void:
	var built: Dictionary = p_job["built"]
	var root: Node3D = built["root"]
	var scene_path: String = p_job["scene"]
	var base := scene_path.get_basename()
	var cameras: Array = built.get("cameras", [])

	# Enter the tree so physics bodies register for the line-of-sight rays.
	get_root().add_child(root)
	await physics_frame
	await physics_frame

	var ok := true
	if built.has("q1_surfaces"):
		# Generate a surplus, drop patches no camera can see (for example
		# floor patches under a block), then keep the first 128 survivors.
		var patch_seed := Scaffold.DEFAULT_SEED
		var rng: RandomNumberGenerator
		if built.has("q1_rng_seed"):
			patch_seed = int(built["q1_rng_seed"])
			rng = RandomNumberGenerator.new()
			rng.seed = patch_seed
		else:
			rng = Scaffold.make_rng(int(p_job["stream"]))
		var surplus := Scaffold.generate_q1_patches(built["q1_surfaces"], Q1_PATCH_COUNT * 2, rng)
		var patches := _filter_line_of_sight(root, surplus, cameras)
		if patches.size() < Q1_PATCH_COUNT:
			_fail("%s: %d line-of-sight-verified Q1 patches, expected %d." % [scene_path, patches.size(), Q1_PATCH_COUNT])
			ok = false
		else:
			patches.resize(Q1_PATCH_COUNT)
			_sort_and_renumber(patches)
			if not _write_json(base + "_patches_q1.json", _patch_document(scene_path, patches, patch_seed)):
				ok = false
	if ok and built.has("q3_patches"):
		var q3: Array = built["q3_patches"]
		var lit_count := 0
		var dark_count := 0
		for patch_value in q3:
			var patch: Dictionary = patch_value
			if patch["room"] == "lit":
				lit_count += 1
			elif patch["room"] == "dark":
				dark_count += 1
		if lit_count != 16 or dark_count != 16:
			_fail("%s: Q3 patches lit=%d dark=%d, expected 16+16." % [scene_path, lit_count, dark_count])
			ok = false
		elif not _write_json(base + "_patches_q3.json", _patch_document(scene_path, q3)):
			ok = false
	if ok and built.has("steps"):
		var steps_path := scene_path.get_base_dir().path_join("steps.json")
		if not _write_json(steps_path, built["steps"]):
			ok = false
	if ok and String(built.get("offscreen_light_node", "")) != "":
		if not _verify_offscreen(root, String(built["offscreen_light_node"]), cameras):
			ok = false
	if ok and built.has("interior_wall_thickness_m"):
		if not _verify_single_mesh(root, String(built["mesh_node"]), float(built["interior_wall_thickness_m"])):
			ok = false

	get_root().remove_child(root)
	if root.has_meta("skip_runtime"):
		root.remove_meta("skip_runtime")
	# Entering the tree makes the first camera current; restore the
	# authored state (only CaptureCam current) before packing.
	for camera_name in cameras:
		var camera := root.get_node(String(camera_name)) as Camera3D
		if camera != null:
			camera.current = camera.name == &"CaptureCam"
	if ok:
		var err := Scaffold.save_scene(root, scene_path)
		if err != OK:
			_fail("%s: save failed with error %d." % [scene_path, err])
		else:
			print("build_all: wrote " + scene_path)
	root.free()


func _patch_document(p_scene_path: String, p_patches: Array, p_seed: int = Scaffold.DEFAULT_SEED) -> Dictionary:
	return {
		"version": 1,
		"scene": p_scene_path.get_file(),
		"seed": p_seed,
		"count": p_patches.size(),
		"patches": p_patches,
	}


func _write_json(p_path: String, p_document: Dictionary) -> bool:
	var err := Scaffold.write_text_chunked(p_path, Scaffold.to_json(p_document) + "\n")
	if err != OK:
		_fail("%s: write failed with error %d." % [p_path, err])
		return false
	print("build_all: wrote " + p_path)
	return true


func _sort_and_renumber(p_patches: Array) -> void:
	p_patches.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		if a["node"] != b["node"]:
			return String(a["node"]) < String(b["node"])
		var pa: Array = a["pos"]
		var pb: Array = b["pos"]
		for axis in range(3):
			if not is_equal_approx(float(pa[axis]), float(pb[axis])):
				return float(pa[axis]) < float(pb[axis])
		return false)
	for i in range(p_patches.size()):
		var patch: Dictionary = p_patches[i]
		patch["id"] = i


## Casts a physics ray from each patch's assigned camera (then the other
## cameras, in order) to the patch. Returns the patches some camera can
## see, with the patch camera field updated to that camera.
func _filter_line_of_sight(p_root: Node3D, p_patches: Array, p_cameras: Array) -> Array:
	var space := p_root.get_world_3d().direct_space_state
	var camera_nodes: Array[Camera3D] = []
	for camera_name in p_cameras:
		var camera := p_root.get_node(String(camera_name)) as Camera3D
		if camera != null:
			camera_nodes.append(camera)
	var visible: Array = []
	for patch_value in p_patches:
		var patch: Dictionary = patch_value
		var pos_array: Array = patch["pos"]
		var normal_array: Array = patch["normal"]
		var pos := Vector3(float(pos_array[0]), float(pos_array[1]), float(pos_array[2]))
		var normal := Vector3(float(normal_array[0]), float(normal_array[1]), float(normal_array[2]))
		var ordered: Array[Camera3D] = []
		for camera in camera_nodes:
			if camera.name == String(patch["camera"]):
				ordered.push_front(camera)
			else:
				ordered.append(camera)
		var visible_from := ""
		for camera in ordered:
			var from := camera.global_position
			var to := pos + normal * 0.02
			var query := PhysicsRayQueryParameters3D.create(from, to)
			query.collide_with_areas = false
			query.collide_with_bodies = true
			var hit := space.intersect_ray(query)
			if hit.is_empty() or Vector3(hit["position"]).distance_to(pos) < 0.06:
				visible_from = camera.name
				break
		if not visible_from.is_empty():
			patch["camera"] = visible_from
			visible.append(patch)
	return visible


func _verify_offscreen(p_root: Node3D, p_light_node: String, p_cameras: Array) -> bool:
	var light := p_root.get_node(p_light_node) as Node3D
	if light == null:
		_fail("off-screen light node %s not found." % p_light_node)
		return false
	for camera_name in p_cameras:
		var camera := p_root.get_node(String(camera_name)) as Camera3D
		if camera == null:
			continue
		if Scaffold.is_in_frustum(camera, light.global_position):
			_fail("off-screen light is inside the frustum of %s." % camera.name)
			return false
		if camera.is_inside_tree() and camera.is_position_in_frustum(light.global_position):
			_fail("off-screen light is inside the engine frustum of %s." % camera.name)
			return false
	print("build_all: off-screen light verified outside every camera frustum.")
	return true


func _verify_single_mesh(p_root: Node3D, p_mesh_node: String, p_wall_thickness: float) -> bool:
	var instance := p_root.get_node(p_mesh_node) as MeshInstance3D
	if instance == null or instance.mesh == null:
		_fail("single-mesh node %s not found." % p_mesh_node)
		return false
	var aabb := instance.mesh.get_aabb()
	var longest := maxf(aabb.size.x, maxf(aabb.size.y, aabb.size.z))
	if longest < 40.0:
		_fail("single mesh longest AABB side is %0.3f m, expected >= 40 m." % longest)
		return false
	if not is_equal_approx(p_wall_thickness, 0.10):
		_fail("single mesh interior wall thickness is %0.3f m, expected 0.10 m." % p_wall_thickness)
		return false
	if not is_equal_approx(float(instance.get_meta("interior_wall_thickness_m", -1.0)), 0.10):
		_fail("single mesh is missing its interior_wall_thickness_m metadata.")
		return false
	print("build_all: single mesh verified (longest side %0.2f m, walls 0.10 m)." % longest)
	return true
