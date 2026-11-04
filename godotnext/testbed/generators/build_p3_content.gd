extends RefCounted
## P3 gate scenes (#52): Character Walk, Roughness Corridor, Glass & Water
## and Fog + Emissive Street. Original geometry only. Run from build_all
## or `godot --headless --path godotnext/testbed --script res://generators/build_p3.gd`.

const Scaffold := preload("res://generators/scaffold.gd")
const Character = preload("res://scenes/character_walk/character.gd")

const STRIPS: Array[float] = [0.05, 0.1, 0.175, 0.25, 0.325, 0.4, 0.6]


static func contract_error() -> String:
	var character_err := String(Character.contract_error())
	if not character_err.is_empty():
		return character_err
	if STRIPS.size() != 7:
		return "roughness corridor does not have 7 strips."
	return ""


static func save_all(p_host: SceneTree) -> Array[String]:
	var failures: Array[String] = []
	var contract := contract_error()
	if not contract.is_empty():
		failures.append(contract)
		return failures
	var jobs: Array[Dictionary] = [
		{"built": build_character_walk(), "scene": "res://scenes/character_walk/character_walk.tscn"},
		{"built": build_roughness_corridor(), "scene": "res://scenes/roughness_corridor/roughness_corridor.tscn"},
		{"built": build_glass_water(), "scene": "res://scenes/glass_water/glass_water.tscn"},
		{"built": build_fog_street(), "scene": "res://scenes/fog_emissive_street/fog_emissive_street.tscn"},
	]
	for job in jobs:
		var built: Dictionary = job["built"]
		var root: Node3D = built["root"]
		var scene_path: String = job["scene"]
		p_host.get_root().add_child(root)
		await p_host.physics_frame
		await p_host.physics_frame
		if not _verify_job(root, built, failures, scene_path):
			p_host.get_root().remove_child(root)
			root.free()
			continue
		_write_sidecars(scene_path, built, failures)
		p_host.get_root().remove_child(root)
		if root.has_meta("skip_runtime"):
			root.remove_meta("skip_runtime")
		for camera_name in built["cameras"]:
			var camera := root.get_node_or_null(String(camera_name)) as Camera3D
			if camera != null:
				camera.current = camera.name == &"CaptureCam"
		var err := Scaffold.save_scene(root, scene_path)
		if err != OK:
			failures.append("%s: save failed with error %d." % [scene_path, err])
		else:
			print("build_p3: wrote " + scene_path)
		root.free()
	return failures


static func build_character_walk() -> Dictionary:
	var root := Node3D.new()
	root.name = "CharacterWalk"
	root.set_script(load("res://scenes/character_walk/walk.gd"))
	root.set_meta("skip_runtime", true)
	_add_world(root, false)
	var floor_mat := Scaffold.make_constant_material("Floor", Color(0.7, 0.7, 0.68), 0.92)
	var wall_mat := Scaffold.make_constant_material("Wall", Color(0.82, 0.8, 0.74), 0.9)
	Scaffold.add_box_instance(root, root, "Floor", Vector3(2.0, -0.1, 0.0), Vector3(12.0, 0.2, 8.0), floor_mat)
	Scaffold.add_box_instance(root, root, "Wall", Vector3(Character.WALL_X, 1.5, 0.0), Vector3(0.15, 3.0, 6.0), wall_mat)
	_add_sun(root, Vector3(-0.55, -0.7, -0.15), Vector3(6.0, 4.0, 0.0))
	var character := Character.make_character("Character")
	character.position = Character.path_position(0)
	_adopt(root, character, root)
	var cameras: Array[String] = ["CaptureCam", "CovCam0", "CovCam1", "CovCam2", "CovCam3"]
	Scaffold.make_camera(root, root, "CaptureCam", Vector3(2.0, 1.7, 6.2), Vector3(2.0, 1.0, 0.0), 40.0, 60.0, true)
	Scaffold.make_camera(root, root, "CovCam0", Vector3(3.4, 1.6, 3.2), Vector3(3.2, 1.0, 0.0), 30.0)
	Scaffold.make_camera(root, root, "CovCam1", Vector3(2.6, 1.6, -3.2), Vector3(2.6, 1.0, 0.0), 30.0)
	Scaffold.make_camera(root, root, "CovCam2", Vector3(2.0, 1.8, 4.0), Vector3(2.0, 1.0, 0.0), 30.0)
	Scaffold.make_camera(root, root, "CovCam3", Vector3(0.8, 1.6, 3.4), Vector3(0.8, 1.0, 0.0), 30.0)
	return {
		"root": root,
		"cameras": cameras,
		"patches": _character_patches(),
		"steps": _character_steps(),
		"check": "character_walk",
	}


static func build_roughness_corridor() -> Dictionary:
	var root := Node3D.new()
	root.name = "RoughnessCorridor"
	root.set_script(load("res://scenes/roughness_corridor/corridor.gd"))
	root.set_meta("skip_runtime", true)
	var world := _add_world(root, false)
	world.environment.ssr_enabled = false
	var strips := Node3D.new()
	strips.name = "Strips"
	_adopt(root, strips, root)
	var masks: Array = []
	for index in STRIPS.size():
		var roughness := STRIPS[index]
		var x0 := float(index) * 2.0
		var material := Scaffold.make_constant_material(
				"Strip%0.3f" % roughness, Color(0.9, 0.9, 0.9), roughness, 1.0, 0.5)
		var node_name := "Strip%d" % index
		Scaffold.add_box_instance(root, strips, node_name, Vector3(x0 + 1.0, -0.05, 0.0), Vector3(2.0, 0.1, 4.0), material)
		masks.append({
			"bin": _roughness_bin(roughness),
			"camera": "Cam%d" % mini(index, 2),
			"node": node_name,
			"origin": [x0, 0.0, -2.0],
			"roughness": roughness,
			"u": [2.0, 0.0, 0.0],
			"v": [0.0, 0.0, 4.0],
		})
	var walls := [
		["WallNorth", Vector3(7.0, 1.5, -2.15), Vector3(14.0, 3.0, 0.2), Color(0.75, 0.25, 0.22)],
		["WallSouth", Vector3(7.0, 1.5, 2.15), Vector3(14.0, 3.0, 0.2), Color(0.22, 0.35, 0.72)],
	]
	for wall in walls:
		var albedo: Color = wall[3]
		var material := Scaffold.make_constant_material(String(wall[0]), albedo, 0.85)
		Scaffold.add_box_instance(root, root, String(wall[0]), wall[1], wall[2], material)
	var emissive := Scaffold.make_constant_material(
			"OffscreenEmissive", Color(0.02, 0.02, 0.02), 0.4, 0.0, 0.0, Color(1.0, 0.35, 0.05), 8.0)
	Scaffold.add_box_instance(root, root, "OffscreenEmissive", Vector3(-2.5, 1.4, 0.0), Vector3(0.4, 0.8, 0.8), emissive, false)
	_add_sun(root, Vector3(-0.2, -0.85, -0.2), Vector3(7.0, 5.0, 0.0))
	var cameras: Array[String] = ["Cam0", "Cam1", "Cam2", "CaptureCam"]
	Scaffold.make_camera(root, root, "Cam0", Vector3(1.0, 1.45, 0.0), Vector3(6.0, 1.0, 0.0), 30.0, 50.0)
	Scaffold.make_camera(root, root, "Cam1", Vector3(5.0, 1.5, -1.2), Vector3(10.0, 0.8, 0.2), 30.0, 50.0)
	Scaffold.make_camera(root, root, "Cam2", Vector3(9.0, 1.55, 1.2), Vector3(13.0, 0.8, 0.0), 30.0, 50.0)
	Scaffold.make_camera(root, root, "CaptureCam", Vector3(3.0, 1.5, -1.0), Vector3(8.0, 1.0, 0.0), 40.0, 55.0, true)
	root.set_meta("emissive_outside_frustum", true)
	return {
		"root": root,
		"cameras": cameras,
		"patches": _corridor_patches(),
		"masks": {"emissive_node": "OffscreenEmissive", "strips": masks},
		"steps": _static_steps("roughness_corridor", cameras, 52),
		"offscreen_light_node": "OffscreenEmissive",
		"check": "roughness",
	}


static func build_glass_water() -> Dictionary:
	var root := Node3D.new()
	root.name = "GlassWater"
	root.set_script(load("res://scenes/glass_water/glass_water.gd"))
	root.set_meta("skip_runtime", true)
	_add_world(root, false)
	var plaster := Scaffold.make_constant_material("Plaster", Color(0.78, 0.76, 0.7), 0.9)
	Scaffold.add_box_instance(root, root, "Floor", Vector3(0.0, -0.1, 0.0), Vector3(10.0, 0.2, 8.0), plaster)
	Scaffold.add_box_instance(root, root, "Ceiling", Vector3(0.0, 3.1, 0.0), Vector3(10.0, 0.2, 8.0), plaster)
	Scaffold.add_box_instance(root, root, "WallWest", Vector3(-5.1, 1.5, 0.0), Vector3(0.2, 3.0, 8.0), plaster)
	Scaffold.add_box_instance(root, root, "WallEast", Vector3(5.1, 1.5, 0.0), Vector3(0.2, 3.0, 8.0), plaster)
	Scaffold.add_box_instance(root, root, "WallNorth", Vector3(0.0, 1.5, -4.1), Vector3(10.0, 3.0, 0.2), plaster)
	var glass := _alpha_material("Glass", Color(0.7, 0.85, 0.9, 0.35), 0.05, false)
	var water := _alpha_material("Water", Color(0.15, 0.35, 0.45, 0.6), 0.05, false)
	Scaffold.add_box_instance(root, root, "GlassA", Vector3(-1.2, 1.4, 0.4), Vector3(0.04, 2.2, 1.6), glass, false)
	Scaffold.add_box_instance(root, root, "GlassB", Vector3(1.4, 1.3, -0.6), Vector3(1.4, 1.8, 0.04), glass, false)
	Scaffold.add_box_instance(root, root, "Water", Vector3(0.2, 0.35, 0.2), Vector3(6.0, 0.04, 4.0), water, false)
	_add_fence(root, "FenceScissor", false)
	_add_fence(root, "FenceOpaque", true)
	root.get_node("FenceOpaque").visible = false
	_add_sun(root, Vector3(-0.35, -0.8, -0.25), Vector3(0.0, 4.5, 2.0))
	var omni := OmniLight3D.new()
	omni.name = "Omni"
	omni.position = Vector3(0.0, 2.4, 0.0)
	omni.light_energy = 0.0
	omni.omni_range = 8.0
	omni.shadow_enabled = true
	Scaffold.add_owned(root, omni, root)
	var cameras: Array[String] = ["CaptureCam", "CovCam0", "CovCam1"]
	Scaffold.make_camera(root, root, "CaptureCam", Vector3(0.0, 1.6, 3.2), Vector3(0.0, 1.0, 0.0), 20.0, 60.0, true)
	Scaffold.make_camera(root, root, "CovCam0", Vector3(-2.5, 1.7, 2.4), Vector3(-1.0, 1.2, 0.2), 20.0)
	Scaffold.make_camera(root, root, "CovCam1", Vector3(2.6, 1.7, 2.2), Vector3(1.2, 1.0, -0.4), 20.0)
	var masks: Array = [
		{"node": "GlassA", "origin": [-1.22, 0.3, -0.4], "u": [0.0, 0.0, 1.6], "v": [0.0, 2.2, 0.0]},
		{"node": "GlassB", "origin": [0.7, 0.4, -0.62], "u": [1.4, 0.0, 0.0], "v": [0.0, 1.8, 0.0]},
		{"node": "Water", "origin": [-2.8, 0.37, -1.8], "u": [6.0, 0.0, 0.0], "v": [0.0, 0.0, 4.0]},
	]
	return {
		"root": root,
		"cameras": cameras,
		"patches": _room_floor_patches("Floor", Vector3(-4.7, 0.0, -3.7), Vector3(9.4, 0.0, 0.0), Vector3(0.0, 0.0, 7.4)),
		"masks": {"surfaces": masks},
		"steps": _glass_steps(),
		"check": "glass",
	}


static func build_fog_street() -> Dictionary:
	var root := Node3D.new()
	root.name = "FogEmissiveStreet"
	root.set_script(load("res://scenes/fog_emissive_street/street.gd"))
	root.set_meta("skip_runtime", true)
	var world := _add_world(root, true)
	world.environment.volumetric_fog_density = 0.03
	world.environment.volumetric_fog_length = 48.0
	var asphalt := Scaffold.make_constant_material("Asphalt", Color(0.22, 0.22, 0.24), 0.95)
	var facade := Scaffold.make_constant_material("Facade", Color(0.55, 0.52, 0.48), 0.88)
	Scaffold.add_box_instance(root, root, "Road", Vector3(20.0, -0.1, 0.0), Vector3(40.0, 0.2, 10.0), asphalt)
	Scaffold.add_box_instance(root, root, "BlockA", Vector3(8.0, 2.0, -6.2), Vector3(8.0, 4.0, 2.0), facade)
	Scaffold.add_box_instance(root, root, "BlockB", Vector3(24.0, 2.5, 6.2), Vector3(10.0, 5.0, 2.0), facade)
	var volumes := Node3D.new()
	volumes.name = "FogVolumes"
	_adopt(root, volumes, root)
	var volume_specs: Array[Vector3] = [Vector3(8.0, 1.2, 0.0), Vector3(18.0, 1.2, 0.0), Vector3(28.0, 1.2, 0.0)]
	var volume_size := Vector3(4.0, 2.4, 3.0)
	for index in volume_specs.size():
		var volume := FogVolume.new()
		volume.name = "FogVolume%d" % index
		volume.position = volume_specs[index]
		volume.size = volume_size
		var fog_material := FogMaterial.new()
		fog_material.density = 0.12 + float(index) * 0.03
		volume.material = fog_material
		_adopt(volumes, volume, root)
	_add_sign(root, "SignLarge", Vector3(22.0, 2.4, 4.9), Vector3(1.4, 0.7, 0.06), 16.0, Color(1.0, 0.85, 0.4), false)
	_add_sign(root, "SignSmallA", Vector3(6.0, 2.2, -4.9), Vector3(0.02, 0.02, 0.01), 64.0, Color(1.0, 0.2, 0.15), true)
	_add_sign(root, "SignSmallB", Vector3(30.0, 1.8, 4.9), Vector3(0.025, 0.02, 0.01), 32.0, Color(0.3, 0.8, 1.0), true)
	_add_sign(root, "SignMid", Vector3(14.0, 2.6, -4.9), Vector3(0.45, 0.3, 0.05), 48.0, Color(1.0, 0.95, 0.8), false)
	_add_sun(root, Vector3(-0.4, -0.75, 0.1), Vector3(20.0, 6.0, 8.0))
	var cameras: Array[String] = ["CaptureCam", "CovCam0", "CovCam1"]
	Scaffold.make_camera(root, root, "CaptureCam", Vector3(4.0, 1.7, 4.2), Vector3(16.0, 1.2, 0.0), 50.0, 60.0, true)
	Scaffold.make_camera(root, root, "CovCam0", Vector3(2.0, 1.8, -3.5), Vector3(10.0, 1.0, 0.0), 50.0)
	Scaffold.make_camera(root, root, "CovCam1", Vector3(36.0, 1.8, 3.5), Vector3(28.0, 1.2, 0.0), 40.0)
	return {
		"root": root,
		"cameras": cameras,
		"patches": _fog_patches(volume_specs, volume_size),
		"fog_volumes": volume_specs,
		"fog_volume_size": volume_size,
		"steps": _fog_steps(),
		"check": "fog",
	}


static func _verify_job(p_root: Node3D, p_built: Dictionary, p_failures: Array[String], p_scene: String) -> bool:
	var check := String(p_built.get("check", ""))
	if check == "roughness":
		var emissive := p_root.get_node_or_null("OffscreenEmissive") as Node3D
		if emissive == null:
			p_failures.append(p_scene + ": emissive sign is missing.")
			return false
		for camera_name in p_built["cameras"]:
			var camera := p_root.get_node(String(camera_name)) as Camera3D
			if Scaffold.is_in_frustum(camera, emissive.global_position):
				p_failures.append("%s: emissive is inside %s." % [p_scene, camera_name])
				return false
		var strips := p_root.get_node("Strips")
		if strips.get_child_count() != STRIPS.size():
			p_failures.append(p_scene + ": strip count.")
			return false
		for index in STRIPS.size():
			var mesh := strips.get_child(index) as MeshInstance3D
			var material := mesh.get_surface_override_material(0) as StandardMaterial3D
			if material == null or not is_equal_approx(material.roughness, STRIPS[index]) or not is_equal_approx(material.metallic, 1.0):
				p_failures.append("%s: strip %d material." % [p_scene, index])
				return false
	elif check == "glass":
		for node_name in ["GlassA", "GlassB", "Water"]:
			var mesh := p_root.get_node(node_name) as MeshInstance3D
			var material := mesh.get_surface_override_material(0) as StandardMaterial3D
			if material == null or material.transparency != BaseMaterial3D.TRANSPARENCY_ALPHA:
				p_failures.append("%s: %s is not alpha blended." % [p_scene, node_name])
				return false
			if node_name == "Water" and not is_equal_approx(material.roughness, 0.05):
				p_failures.append(p_scene + ": water roughness.")
				return false
		if not bool(p_root.get_node("FenceScissor").get_meta("nextgi_rt_treat_as_opaque")) and bool(p_root.get_node("FenceOpaque").get_meta("nextgi_rt_treat_as_opaque")):
			pass
		else:
			p_failures.append(p_scene + ": fence opaque flags.")
			return false
		var scissor_mat := (p_root.get_node("FenceScissor/Bar0") as MeshInstance3D).get_surface_override_material(0) as StandardMaterial3D
		if scissor_mat == null or scissor_mat.transparency != BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR:
			p_failures.append(p_scene + ": fence is not alpha scissor.")
			return false
	elif check == "fog":
		var volumes := p_root.get_node("FogVolumes")
		if volumes.get_child_count() != 3:
			p_failures.append(p_scene + ": expected 3 fog volumes.")
			return false
		if (p_built["patches"] as Array).size() != 128:
			p_failures.append(p_scene + ": expected 128 fog patches.")
			return false
		var margin := 1.0
		var size: Vector3 = p_built["fog_volume_size"]
		for patch_value in p_built["patches"]:
			var patch: Dictionary = patch_value
			var pos_array: Array = patch["pos"]
			var pos := Vector3(float(pos_array[0]), float(pos_array[1]), float(pos_array[2]))
			for center_value in p_built["fog_volumes"]:
				var center: Vector3 = center_value
				var expanded := AABB(center - size * 0.5 - Vector3(margin, margin, margin), size + Vector3(margin, margin, margin) * 2.0)
				if expanded.has_point(pos):
					p_failures.append(p_scene + ": a Q1 patch sits inside a fog volume margin.")
					return false
		for sign_name in ["SignSmallA", "SignSmallB"]:
			var sign := p_root.get_node(sign_name) as MeshInstance3D
			if sign.mesh.get_aabb().get_longest_axis_size() >= 0.04:
				p_failures.append(p_scene + ": small sign is not under 4 cm.")
				return false
			if p_root.get_node_or_null(sign_name + "Light") == null:
				p_failures.append(p_scene + ": small sign has no authored light.")
				return false
	elif check == "character_walk":
		var visual := p_root.get_node("Character/Visual") as MeshInstance3D
		if visual == null or visual.mesh.get_faces().size() / 3 != Character.TRIANGLES:
			p_failures.append(p_scene + ": packed character triangle count.")
			return false
		var counts := {}
		for patch_value in p_built["patches"]:
			var patch: Dictionary = patch_value
			var key := str(patch.get("pose", ""))
			counts[key] = int(counts.get(key, 0)) + 1
		for pose in Character.POSE_FRAMES.size():
			var count := int(counts.get(str(pose), 0))
			if count < 64 or count > 256:
				p_failures.append("%s: pose %d has %d patches." % [p_scene, pose, count])
				return false
	var light_name := String(p_built.get("offscreen_light_node", ""))
	if not light_name.is_empty():
		var light := p_root.get_node_or_null(light_name) as Node3D
		for camera_name in p_built["cameras"]:
			var camera := p_root.get_node(String(camera_name)) as Camera3D
			if light != null and Scaffold.is_in_frustum(camera, light.global_position):
				p_failures.append("%s: %s is inside %s." % [p_scene, light_name, camera_name])
				return false
	return true


static func _write_sidecars(p_scene_path: String, p_built: Dictionary, p_failures: Array[String]) -> void:
	var base := p_scene_path.get_basename()
	if p_built.has("patches"):
		var patches: Array = p_built["patches"]
		var document := {
			"version": 1,
			"scene": p_scene_path.get_file(),
			"seed": Scaffold.stream_seed(52),
			"count": patches.size(),
			"patches": patches,
		}
		if Scaffold.write_text_chunked(base + "_patches_q1.json", Scaffold.to_json(document) + "\n") != OK:
			p_failures.append(base + ": patch write failed.")
	if p_built.has("masks"):
		if Scaffold.write_text_chunked(base + "_masks.json", Scaffold.to_json(p_built["masks"]) + "\n") != OK:
			p_failures.append(base + ": mask write failed.")
	if p_built.has("steps"):
		var steps_path := p_scene_path.get_base_dir().path_join("steps.json")
		if Scaffold.write_text_chunked(steps_path, Scaffold.to_json(p_built["steps"]) + "\n") != OK:
			p_failures.append(steps_path + ": write failed.")


static func _character_patches() -> Array:
	var patches: Array = []
	var next_id := 0
	var rng := Scaffold.make_rng(52)
	for pose in Character.POSE_FRAMES.size():
		var origin := Character.path_position(Character.pose_frame(pose))
		var placed := 0
		var attempt := 0
		while placed < 16 and attempt < 400:
			attempt += 1
			var offset := Vector3(rng.randf_range(-0.95, 0.95), 0.0, rng.randf_range(-0.95, 0.95))
			if offset.length() > 1.0:
				continue
			var pos := origin + offset
			if not _far_enough(patches, pos, 0.22, str(pose)):
				continue
			patches.append(_patch(next_id, pos, Vector3.UP, "Floor", "CaptureCam", pose, "floor"))
			next_id += 1
			placed += 1
		var points := Character.surface_points(120)
		var used := 0
		for point in points:
			if used >= 48:
				break
			var pos := origin + point
			patches.append(_patch(next_id, pos, Vector3.UP, "Character", "CaptureCam", pose, "character"))
			next_id += 1
			used += 1
	var spot := Vector3(Character.VACATED_X, 0.0, 0.0)
	for index in 16:
		var pos := spot + Vector3(rng.randf_range(-0.4, 0.4), 0.0, rng.randf_range(-0.6, 0.6))
		patches.append(_patch(next_id, pos, Vector3.UP, "Floor", "CaptureCam", -1, "vacated"))
		next_id += 1
	for index in 16:
		var pos := Vector3(Character.WALL_X + 0.08, 0.4 + float(index) * 0.12, rng.randf_range(-0.5, 0.5))
		patches.append(_patch(next_id, pos, Vector3.RIGHT, "Wall", "CaptureCam", -1, "vacated"))
		next_id += 1
	return patches


static func _corridor_patches() -> Array:
	var patches := _room_floor_patches("Strip0", Vector3(0.4, 0.0, -1.7), Vector3(13.2, 0.0, 0.0), Vector3(0.0, 0.0, 3.4))
	for patch_value in patches:
		var patch: Dictionary = patch_value
		var x := float((patch["pos"] as Array)[0])
		var index := clampi(int(x / 2.0), 0, STRIPS.size() - 1)
		patch["node"] = "Strip%d" % index
		patch["roughness"] = STRIPS[index]
		patch["bin"] = _roughness_bin(STRIPS[index])
	return patches


static func _room_floor_patches(p_node: String, p_origin: Vector3, p_u: Vector3, p_v: Vector3) -> Array:
	var rng := Scaffold.make_rng(53)
	var surface := Scaffold.make_patch_surface(p_node, p_origin, p_u, p_v, Vector3.UP, "CaptureCam")
	var generated: Array = Scaffold.generate_q1_patches([surface], 128, rng)
	if generated.size() > 128:
		generated.resize(128)
	return generated


static func _fog_patches(p_centers: Array[Vector3], p_size: Vector3) -> Array:
	var patches: Array = []
	var rng := Scaffold.make_rng(55)
	var next_id := 0
	while patches.size() < 128:
		var x := rng.randf_range(0.6, 3.4) if patches.size() < 64 else rng.randf_range(36.2, 39.2)
		var z := rng.randf_range(-4.2, 4.2)
		var pos := Vector3(x, 0.0, z)
		if not _outside_fog(pos, p_centers, p_size, 1.0):
			continue
		if not _far_enough(patches, pos, 0.45, ""):
			continue
		patches.append(_patch(next_id, pos, Vector3.UP, "Road", "CaptureCam", -1, "outside_fog"))
		next_id += 1
		if next_id > 4000:
			break
	return patches


static func _outside_fog(p_pos: Vector3, p_centers: Array[Vector3], p_size: Vector3, p_margin: float) -> bool:
	for center in p_centers:
		var expanded := AABB(center - p_size * 0.5 - Vector3.ONE * p_margin, p_size + Vector3.ONE * p_margin * 2.0)
		if expanded.has_point(p_pos):
			return false
	return true


static func _far_enough(p_patches: Array, p_pos: Vector3, p_distance: float, p_pose: String) -> bool:
	var limit := p_distance * p_distance
	for patch_value in p_patches:
		var patch: Dictionary = patch_value
		if p_pose != "" and str(patch.get("pose", "")) != p_pose:
			continue
		var pos_array: Array = patch["pos"]
		var other := Vector3(float(pos_array[0]), float(pos_array[1]), float(pos_array[2]))
		if other.distance_squared_to(p_pos) < limit:
			return false
	return true


static func _patch(p_id: int, p_pos: Vector3, p_normal: Vector3, p_node: String, p_camera: String, p_pose: int, p_kind: String) -> Dictionary:
	var patch := {
		"id": p_id,
		"pos": [p_pos.x, p_pos.y, p_pos.z],
		"normal": [p_normal.x, p_normal.y, p_normal.z],
		"radius": Scaffold.Q1_PATCH_RADIUS,
		"node": p_node,
		"camera": p_camera,
		"kind": p_kind,
	}
	if p_pose >= 0:
		patch["pose"] = p_pose
	return patch


static func _character_steps() -> Dictionary:
	var variants: Array = [
		{"id": "walk", "form": "timing", "frame0_action": "cross the sun/shadow threshold", "end_frame": 239, "motion_frames": 240, "motion_start": 0, "motion_end": 240},
		{"id": "vacated", "form": "Q4a", "frame0_action": "character removed 0.3 m from the wall", "end_frame": 0, "motion_frames": 0, "motion_start": 0, "motion_end": 0},
		{"id": "empty", "form": "Q4a", "frame0_action": "empty endpoint", "end_frame": 240, "motion_frames": 0, "motion_start": 240, "motion_end": 240},
		{"id": "soak_20", "form": "timing", "frame0_action": "20 skinned characters", "end_frame": 599, "motion_frames": 600, "motion_start": 0, "motion_end": 600},
	]
	for pose in Character.POSE_FRAMES.size():
		variants.append({
			"id": "pose_%d" % pose,
			"form": "Q1",
			"frame0_action": "hold path frame %d" % Character.pose_frame(pose),
			"end_frame": Character.pose_frame(pose),
			"motion_frames": 0,
			"motion_start": Character.pose_frame(pose),
			"motion_end": Character.pose_frame(pose),
		})
	return {"name": "character_walk", "variants": variants, "cameras": ["CaptureCam", "CovCam0", "CovCam1", "CovCam2", "CovCam3"], "patch_set": "q1", "seed": Scaffold.stream_seed(52)}


static func _glass_steps() -> Dictionary:
	return {
		"name": "glass_water",
		"variants": [
			{"id": "light_toggle", "form": "Q4a", "frame0_action": "omni energy 0 to 8", "end_frame": 240, "motion_frames": 0, "motion_start": 0, "motion_end": 0},
			{"id": "fence_scissor", "form": "static", "frame0_action": "alpha scissor, treat_as_opaque false", "end_frame": 0, "motion_frames": 0, "motion_start": 0, "motion_end": 0},
			{"id": "fence_opaque", "form": "static", "frame0_action": "alpha scissor, treat_as_opaque true", "end_frame": 0, "motion_frames": 0, "motion_start": 0, "motion_end": 0},
		],
		"cameras": ["CaptureCam", "CovCam0", "CovCam1"],
		"patch_set": "q1",
		"seed": Scaffold.stream_seed(54),
	}


static func _fog_steps() -> Dictionary:
	return {
		"name": "fog_emissive_street",
		"variants": [
			{"id": "fog_on", "form": "timing", "frame0_action": "volumetric fog and 3 volumes", "end_frame": 599, "motion_frames": 600, "motion_start": 0, "motion_end": 600},
			{"id": "fog_off", "form": "static", "frame0_action": "fog disabled", "end_frame": 0, "motion_frames": 0, "motion_start": 0, "motion_end": 0},
		],
		"cameras": ["CaptureCam", "CovCam0", "CovCam1"],
		"patch_set": "q1",
		"seed": Scaffold.stream_seed(55),
	}


static func _static_steps(p_name: String, p_cameras: Array[String], p_stream: int) -> Dictionary:
	return {
		"name": p_name,
		"variants": [
			{"id": "static", "form": "timing", "frame0_action": "fixed cameras", "end_frame": 599, "motion_frames": 600, "motion_start": 0, "motion_end": 600},
		],
		"cameras": p_cameras,
		"patch_set": "q1",
		"seed": Scaffold.stream_seed(p_stream),
	}


static func _roughness_bin(p_roughness: float) -> String:
	if p_roughness < 0.1:
		return "[0, 0.1)"
	if p_roughness < 0.25:
		return "[0.1, 0.25)"
	if p_roughness <= 0.4:
		return "[0.25, 0.4]"
	return "wrc"


static func _add_world(p_root: Node3D, p_fog: bool) -> WorldEnvironment:
	var sky_material := ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_material
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	environment.ssr_enabled = false
	environment.volumetric_fog_enabled = p_fog
	var world := WorldEnvironment.new()
	world.name = "WorldEnvironment"
	world.environment = environment
	Scaffold.add_owned(p_root, world, p_root)
	return world


static func _add_sun(p_root: Node3D, p_direction: Vector3, p_position: Vector3) -> void:
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.transform = Transform3D(Basis.looking_at(p_direction.normalized(), Vector3.UP), p_position)
	sun.light_energy = 1.0
	sun.shadow_enabled = true
	Scaffold.add_owned(p_root, sun, p_root)


static func _alpha_material(p_name: String, p_albedo: Color, p_roughness: float, p_scissor: bool) -> StandardMaterial3D:
	var material := Scaffold.make_constant_material(p_name, Color(p_albedo.r, p_albedo.g, p_albedo.b), p_roughness)
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR if p_scissor else BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_color.a = p_albedo.a
	if p_scissor:
		material.alpha_scissor_threshold = 0.5
	return material


static func _add_fence(p_root: Node3D, p_name: String, p_opaque: bool) -> void:
	var fence := Node3D.new()
	fence.name = p_name
	fence.position = Vector3(3.2, 0.0, -2.4)
	fence.set_meta("nextgi_rt_treat_as_opaque", p_opaque)
	_adopt(p_root, fence, p_root)
	var material := _alpha_material(p_name + "Mat", Color(0.72, 0.72, 0.7, 1.0), 0.8, true)
	for index in 5:
		Scaffold.add_box_instance(p_root, fence, "Bar%d" % index, Vector3(0.0, 0.8, float(index) * 0.22), Vector3(0.03, 1.6, 0.04), material, false)


static func _add_sign(p_root: Node3D, p_name: String, p_center: Vector3, p_size: Vector3, p_energy: float, p_color: Color, p_with_light: bool) -> void:
	var material := Scaffold.make_constant_material(p_name, Color(0.02, 0.02, 0.02), 0.45, 0.0, 0.0, p_color, p_energy)
	var sign := Scaffold.add_box_instance(p_root, p_root, p_name, p_center, p_size, material, false)
	sign.set_meta("emission_energy", p_energy)
	sign.set_meta("under_four_card_texels", p_size.x < 0.04 and p_size.y < 0.04)
	if p_with_light:
		var light := OmniLight3D.new()
		light.name = p_name + "Light"
		light.position = p_center + Vector3(0.0, 0.0, 0.15 if p_center.z > 0.0 else -0.15)
		light.light_color = p_color
		light.light_energy = p_energy / 16.0
		light.omni_range = 2.5
		light.shadow_enabled = false
		Scaffold.add_owned(p_root, light, p_root)


static func _adopt(p_parent: Node, p_child: Node, p_root: Node) -> void:
	p_parent.add_child(p_child)
	_own(p_child, p_root)


static func _own(p_node: Node, p_root: Node) -> void:
	if p_node != p_root:
		p_node.owner = p_root
	for child in p_node.get_children():
		_own(child, p_root)
