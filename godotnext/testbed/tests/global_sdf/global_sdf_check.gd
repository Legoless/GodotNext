extends Node3D
## #182: compare the live global-SDF GPU data with the CPU reference.
## The slice and coarse ray-step heatmap are both displayed and saved.
## #257: the coarse level is a lower bound. Its GPU values match the CPU twin built from the GPU
## page table and pools, and none reads more than a quarter voxel past the nearest instance at
## 10000 random points of level 0. Checked again after a mesh instance moves MOVE_OFFSET and the
## camera CAMERA_SHIFT (more than a level-0 brick, so level 0 scrolls), once MOVE_FRAMES have passed
## for the clipmap update.

## The slice row against the CPU twin of the composite (NextGIGlobalSDF::composite_distance):
## half-float storage rounds the stored distances by well under a millimeter (0.25 mm measured).
## Reading the wrong brick row of the slice was off by about one 0.1 m voxel.
const SLICE_TOLERANCE := 0.005
const MOVE_OFFSET := Vector3(1.3, 0.0, 0.7)
const CAMERA_SHIFT := Vector3(1.7, 0.0, -0.9)
const MOVE_FRAMES := 60
const MAX_FRAMES := 900

var _first_pass := {}
var _moved_frame := -1
var _moved_name := ""

var _frames := 0
var _sprite: Sprite3D
var _step_sprite: Sprite3D


func _ready() -> void:
	var room: PackedScene = load("res://scenes/nextgi_perf/nextgi_perf.tscn")
	add_child(room.instantiate())
	_sprite = _make_sprite(Vector3(-2.0, 2.0, -2.0))
	_step_sprite = _make_sprite(Vector3(2.0, 2.0, -2.0))
	var camera := Camera3D.new()
	add_child(camera)
	camera.position = Vector3(0.0, 3.0, 6.0)
	camera.look_at(Vector3(0.0, 2.0, -2.0))
	camera.current = true


func _make_sprite(p_position: Vector3) -> Sprite3D:
	var sprite := Sprite3D.new()
	sprite.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	sprite.position = p_position
	sprite.scale = Vector3(3.0, 3.0, 3.0)
	add_child(sprite)
	return sprite


func _process(_delta: float) -> void:
	_frames += 1
	# The debug info runs the CPU comparisons on every read: read it only when it is checked.
	if _moved_frame >= 0 and _frames - _moved_frame < MOVE_FRAMES:
		return
	if _moved_frame < 0 and _frames > MAX_FRAMES:
		print("GLOBAL_SDF_FAIL: no GPU slice arrived")
		get_tree().quit(1)
		return
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	if _moved_frame >= 0:
		if not info.has("global_sdf_coarse_checked"):
			return
		var moved := _coarse_result(info)
		print("GLOBAL_SDF_COARSE_MOVED instance=%s checked=%d overestimates=%d twin_mismatches=%d" % [_moved_name, moved["checked"], moved["overestimates"], moved["twin_mismatches"]])
		get_tree().quit(0 if bool(_first_pass["passed"]) and bool(moved["passed"]) else 1)
		return
	if not info.has("global_sdf_slice"):
		if _frames % 60 == 0:
			print("GLOBAL_SDF_WAIT frame=%d instances=%s jobs=%s pending=%s resident=%s rt_fed=%s" % [_frames, info.get("sdf_instance_count", -1), info.get("mesh_sdf_jobs", -1), info.get("mesh_sdf_pending", -1), info.get("mesh_sdf_resident_bytes", -1), info.get("rt_fed_instance_count", -1)])
		return
	if int(info.get("global_sdf_levels", 0)) < 6:
		return
	var slice: PackedFloat32Array = info["global_sdf_slice"]
	var coverage: PackedByteArray = info["global_sdf_coverage"]
	var step_counts: PackedByteArray = info["global_sdf_step_counts"]
	var image := Image.create(128, 128, false, Image.FORMAT_RGB8)
	var step_image := Image.create(128, 128, false, Image.FORMAT_RGB8)
	var nonzero_steps := 0
	for y in range(128):
		for x in range(128):
			var index := x + y * 128
			var magnitude := clampf(absf(slice[index]), 0.0, 1.0)
			var color := Color(magnitude, 0.15, 1.0 - magnitude)
			if coverage[index] != 0:
				color = Color(0.1, 0.9, 0.25)
			image.set_pixel(x, y, color)
			var count := int(step_counts[index])
			nonzero_steps += int(count > 0)
			var heat := clampf(float(count) / 64.0, 0.0, 1.0)
			step_image.set_pixel(x, y, Color(heat, 0.2, 1.0 - heat))
	_sprite.texture = ImageTexture.create_from_image(image)
	_step_sprite.texture = ImageTexture.create_from_image(step_image)
	var slice_error := image.save_png("user://global_sdf_slice.png")
	var step_error := step_image.save_png("user://global_sdf_step_counts.png")
	var random_checked := int(info.get("global_sdf_random_checked", 0))
	var random_error := float(info.get("global_sdf_random_max_error", 1e30))
	var sign_mismatches := int(info.get("global_sdf_sign_mismatches", -1))
	var slice_checked := int(info.get("global_sdf_checked", 0))
	var slice_max_error := float(info.get("global_sdf_max_error", 1e30))
	var coarse := _coarse_result(info)
	var passed := bool(info.get("global_sdf_random_valid", false)) and random_checked == 10000 and random_error <= 0.1 and sign_mismatches == 0 and slice_checked > 0 and slice_max_error <= SLICE_TOLERANCE and bool(info.get("global_sdf_coarse_valid", false)) and nonzero_steps > 0 and slice_error == OK and step_error == OK and bool(coarse["passed"])
	print("GLOBAL_SDF_COARSE checked=%d overestimates=%d twin_mismatches=%d" % [coarse["checked"], coarse["overestimates"], coarse["twin_mismatches"]])
	print("GLOBAL_SDF_SUMMARY random_checked=%d random_max_error=%.6f sign_mismatches=%d slice_checked=%d slice_max_error=%.6f levels=%d bricks=%d steps=%d instances=%d origin=%s slice_y=%d" % [random_checked, random_error, sign_mismatches, slice_checked, slice_max_error, int(info.get("global_sdf_levels", 0)), int(info.get("global_sdf_used_bricks", 0)), nonzero_steps, int(info.get("global_sdf_instances", 0)), str(info.get("global_sdf_origin", Vector3i())), int(info.get("global_sdf_slice_y", -1))])
	var voxel_counters: Dictionary = RenderingServer.nextgi_get_counters()
	print("VOXEL_LIGHTING retrace=%d coverage_ppm=%d volume_bytes=%d" % [int(voxel_counters.get("voxel_lighting_retrace", 0)), int(voxel_counters.get("voxel_lighting_coverage_ppm", 0)), int(voxel_counters.get("voxel_lighting_volume_bytes", 0))])
	print("GLOBAL_SDF_IMAGES dir=%s slice_error=%d step_error=%d" % [OS.get_user_data_dir(), slice_error, step_error])
	_first_pass = {"passed": passed}
	var mover := _find_mesh_instance(self)
	if mover == null:
		print("GLOBAL_SDF_FAIL: no mesh instance to move")
		get_tree().quit(1)
		return
	_moved_name = str(mover.get_path())
	mover.global_position += MOVE_OFFSET
	get_viewport().get_camera_3d().global_position += CAMERA_SHIFT
	_moved_frame = _frames


func _coarse_result(p_info: Dictionary) -> Dictionary:
	var checked := int(p_info.get("global_sdf_coarse_checked", -1))
	var overestimates := int(p_info.get("global_sdf_coarse_overestimates", -1))
	var twin_mismatches := int(p_info.get("global_sdf_coarse_twin_mismatches", -1))
	return {"checked": checked, "overestimates": overestimates, "twin_mismatches": twin_mismatches, "passed": checked == 10000 and overestimates == 0 and twin_mismatches == 0}


func _find_mesh_instance(p_node: Node) -> MeshInstance3D:
	# The first mesh instance that is not one of this check's sprites.
	for child in p_node.get_children():
		if child is MeshInstance3D and (child as MeshInstance3D).mesh != null:
			return child
		var found := _find_mesh_instance(child)
		if found != null:
			return found
	return null
