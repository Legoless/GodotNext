extends RefCounted
## PV content scenes (#213, plan §21.3.3): Shadow Gallery, VSM Flythrough,
## Fog Hall, RT Shadow Yard and MegaLights Hall. Original procedural content
## and generated textures only; the four IES profiles come from
## `tools/ies_gen.py` (self-authored, MIT).
##
## These scenes are committed under `scenes/<name>/` because their geometry
## is light (thousands of nodes, not millions of triangles). Run from
## build_all or
## `godot --headless --path godotnext/testbed --script res://generators/build_pv.gd`.
## Two runs produce byte-identical `.tscn` and `.res` files.

const Scaffold := preload("res://generators/scaffold.gd")
const Character := preload("res://scenes/character_walk/character.gd")

const IES_DIR := "res://assets/ies"

const SHADOW_STREAM := 81
const FLYTHROUGH_STREAM := 82
const FOG_STREAM := 83
const YARD_STREAM := 84
const MEGA_STREAM := 85

const SHADOW_LIGHTS := 256
const FLYTHROUGH_LIGHTS := 16
const FOG_SPOTS := 8
const YARD_LIGHTS := 64
const YARD_CASTER_LAYERS := 20
const MEGA_BASE_LIGHTS := 64
const MEGA_COUNTS: Array[int] = [64, 1024, 4096]


static func contract_error() -> String:
	if SHADOW_LIGHTS != 256:
		return "Shadow Gallery needs 256 local lights."
	if FLYTHROUGH_LIGHTS != 16:
		return "VSM Flythrough needs 16 local lights."
	if FOG_SPOTS != 8:
		return "Fog Hall needs 8 window spots."
	if YARD_LIGHTS != 64 or YARD_CASTER_LAYERS != 20:
		return "RT Shadow Yard needs 64 lights and 20 caster layers."
	if MEGA_BASE_LIGHTS != 64 or MEGA_COUNTS != [64, 1024, 4096]:
		return "MegaLights Hall needs the 64/1024/4096 variants."
	for name in ["cosine_lobe", "batwing", "narrow_spot", "street_asymmetric"]:
		if not FileAccess.file_exists(IES_DIR + "/" + name + ".ies"):
			return "missing IES profile %s.ies; run tools/ies_gen.py." % name
	return ""


static func save_all(p_host: SceneTree) -> Array[String]:
	var failures: Array[String] = []
	var contract := contract_error()
	if not contract.is_empty():
		failures.append(contract)
		return failures
	var jobs: Array[Dictionary] = [
		{"built": build_shadow_gallery(), "scene": "res://scenes/shadow_gallery/shadow_gallery.tscn"},
		{"built": build_vsm_flythrough(), "scene": "res://scenes/vsm_flythrough/vsm_flythrough.tscn"},
		{"built": build_fog_hall(), "scene": "res://scenes/fog_hall/fog_hall.tscn"},
		{"built": build_rt_shadow_yard(), "scene": "res://scenes/rt_shadow_yard/rt_shadow_yard.tscn"},
		{"built": build_mega_lights_hall(), "scene": "res://scenes/mega_lights_hall/mega_lights_hall.tscn"},
	]
	for job in jobs:
		var built: Dictionary = job["built"]
		if built.is_empty() or not built.has("root"):
			failures.append("%s: the builder returned no scene." % String(job["scene"]))
			continue
		var root: Node3D = built["root"]
		var scene_path: String = job["scene"]
		p_host.get_root().add_child(root)
		await p_host.physics_frame
		await p_host.physics_frame
		if not _verify(root, built, failures, scene_path):
			p_host.get_root().remove_child(root)
			root.free()
			continue
		p_host.get_root().remove_child(root)
		if root.has_meta("skip_runtime"):
			root.remove_meta("skip_runtime")
		for camera_name in built.get("cameras", []):
			var camera := root.get_node_or_null(String(camera_name)) as Camera3D
			if camera != null:
				camera.current = camera.name == &"cam_main"
		var err := Scaffold.save_scene(root, scene_path)
		if err != OK:
			failures.append("%s: save failed with error %d." % [scene_path, err])
		else:
			print("build_pv: wrote " + scene_path)
		root.free()
	return failures


# --- Shadow Gallery ----------------------------------------------------------

## Penumbra row (sun 0.5 and 2 degrees, 10 and 50 cm spheres), thin casters
## (10 cm and 1 cm planks at 50 m), the caster-class row (skinned, MultiMesh
## 10k, particles 5k, alpha foliage, VERTEX-animated material) and 256 local
## lights, 64 of them shadowed.
static func build_shadow_gallery() -> Dictionary:
	var root := Node3D.new()
	root.name = "ShadowGallery"
	root.set_script(load("res://scenes/shadow_gallery/gallery.gd"))
	root.set_meta("skip_runtime", true)
	root.set_meta("seed", Scaffold.stream_seed(SHADOW_STREAM))
	root.set_meta("local_lights", SHADOW_LIGHTS)
	_add_world(root, Color(0.05, 0.05, 0.06), 0.25)
	var sun_a := _add_sun(root, Vector3(-0.45, -0.75, -0.35), 1.6, 0.5, "SunA")
	sun_a.name = "SunA"
	var sun_b := _add_sun(root, Vector3(-0.45, -0.75, -0.35), 1.6, 2.0, "SunB")
	sun_b.name = "SunB"
	sun_b.light_energy = 0.0

	var stone := _mat("GalleryStone", Color(0.55, 0.54, 0.5), 0.9)
	var dark := _mat("GalleryDark", Color(0.18, 0.18, 0.2), 0.85)
	_add_ground(root, Vector2(70.0, 30.0), stone, Vector3(0.0, 0.0, 0.0))
	_add_box(root, root, "BackWall", Vector3(0.0, 5.0, -14.8), Vector3(70.0, 10.0, 0.4), dark)

	# Penumbra row at z = -10: 10 cm and 50 cm spheres at 10 m and 30 m.
	var sphere_small := SphereMesh.new()
	sphere_small.radius = 0.05
	sphere_small.height = 0.1
	var sphere_large := SphereMesh.new()
	sphere_large.radius = 0.25
	sphere_large.height = 0.5
	for index in range(4):
		var mesh := sphere_small if index % 2 == 0 else sphere_large
		var node := MeshInstance3D.new()
		node.name = "PenumbraSphere%d" % index
		node.mesh = mesh
		node.material_override = _mat("Penumbra%d" % index, Color(0.75, 0.75, 0.72), 0.7)
		node.position = Vector3(-9.0 + float(index) * 6.0, 0.5, -10.0)
		Scaffold.add_owned(root, node, root)

	# Thin casters at 50 m: 10 cm and 1 cm planks.
	for index in range(6):
		var thickness := 0.10 if index % 2 == 0 else 0.01
		var plank := _add_box(root, root, "ThinCaster%d" % index,
				Vector3(-12.0 + float(index) * 5.0, 2.5, -12.0),
				Vector3(1.6, 5.0, thickness), _mat("Plank%d" % index, Color(0.62, 0.6, 0.55), 0.85))
		plank.rotation.y = deg_to_rad(18.0)

	# Caster-class row along x.
	_add_box(root, root, "CasterFloor", Vector3(0.0, 0.05, 4.0), Vector3(46.0, 0.1, 6.0), stone)
	var character := Character.make_character("GallerySkinned")
	character.position = Vector3(-16.0, 0.1, 4.0)
	Scaffold.add_owned(root, character, root)
	_add_multi_mesh_casters(root, Vector3(0.0, 0.1, 4.0))
	_add_particle_casters(root, Vector3(9.0, 0.1, 4.0))
	_add_foliage_casters(root, Vector3(15.0, 0.1, 4.0))
	_add_vertex_animated_caster(root, Vector3(20.0, 0.1, 4.0))

	# 256 local lights: a 16x16 grid, every fourth one shadowed.
	var light_rng := Scaffold.make_rng(SHADOW_STREAM + 1)
	for index in range(SHADOW_LIGHTS):
		var column := index % 16
		var row := index / 16
		var omni := OmniLight3D.new()
		omni.name = "Local%03d" % index
		omni.position = Vector3(-30.0 + float(column) * 4.0, 3.2, -12.0 + float(row) * 2.2)
		omni.light_color = Color(1.0, 0.82 + light_rng.randf_range(-0.1, 0.1), 0.62)
		omni.light_energy = 0.35
		omni.omni_range = 3.2
		omni.shadow_enabled = index % 4 == 0
		Scaffold.add_owned(root, omni, root)

	var cameras := _add_cameras(root, [
		{"name": "cam_main", "pos": Vector3(0.0, 2.0, 12.0), "target": Vector3(0.0, 1.5, -4.0), "far": 120.0, "fov": 60.0, "current": true},
		{"name": "cam_penumbra", "pos": Vector3(-8.0, 1.4, -4.0), "target": Vector3(-8.0, 0.5, -10.0), "far": 60.0, "fov": 70.0},
		{"name": "cam_thin", "pos": Vector3(0.0, 2.0, -2.0), "target": Vector3(0.0, 2.0, -12.0), "far": 80.0, "fov": 55.0},
		{"name": "cam_casters", "pos": Vector3(0.0, 2.2, 10.0), "target": Vector3(0.0, 1.0, 4.0), "far": 80.0, "fov": 60.0},
		{"name": "cam_wide", "pos": Vector3(-34.0, 9.0, 14.0), "target": Vector3(0.0, 2.0, -4.0), "far": 140.0, "fov": 50.0},
	], [Vector3(-34.0, 6.0, 13.0), Vector3(-18.0, 2.5, 6.0), Vector3(0.0, 1.8, 2.0),
			Vector3(18.0, 2.5, 6.0), Vector3(34.0, 6.0, 13.0)])
	return {
		"root": root,
		"cameras": cameras,
		"local_lights": SHADOW_LIGHTS,
		"shadowed_lights": SHADOW_LIGHTS / 4,
		"check": "shadow_gallery",
	}


static func _add_multi_mesh_casters(p_root: Node3D, p_origin: Vector3) -> void:
	var box := BoxMesh.new()
	box.size = Vector3(0.12, 0.12, 0.12)
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = box
	mm.instance_count = 10000
	var rng := Scaffold.make_rng(SHADOW_STREAM + 2)
	var buffer := PackedFloat32Array()
	buffer.resize(10000 * 12)
	for index in range(10000):
		var position := p_origin + Vector3(rng.randf_range(-6.0, 6.0), rng.randf_range(0.15, 4.0), rng.randf_range(-1.5, 1.5))
		var angle := rng.randf_range(0.0, TAU)
		var basis := Basis(Vector3.UP, angle)
		var local := p_origin
		var base := index * 12
		buffer[base + 0] = basis.x.x
		buffer[base + 1] = basis.y.x
		buffer[base + 2] = basis.z.x
		buffer[base + 3] = local.x + position.x - p_origin.x
		buffer[base + 4] = basis.x.y
		buffer[base + 5] = basis.y.y
		buffer[base + 6] = basis.z.y
		buffer[base + 7] = position.y
		buffer[base + 8] = basis.x.z
		buffer[base + 9] = basis.y.z
		buffer[base + 10] = basis.z.z
		buffer[base + 11] = local.z + position.z - p_origin.z
	mm.buffer = buffer
	var resource_path := "res://scenes/shadow_gallery/casters.res"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(resource_path.get_base_dir()))
	if ResourceSaver.save(mm, resource_path, ResourceSaver.FLAG_CHANGE_PATH) == OK:
		mm = load(resource_path)
	var node := MultiMeshInstance3D.new()
	node.name = "CasterMultiMesh"
	node.multimesh = mm
	node.material_override = _mat("CasterMultiMesh", Color(0.5, 0.55, 0.6), 0.8)
	Scaffold.add_owned(p_root, node, p_root)


static func _add_particle_casters(p_root: Node3D, p_origin: Vector3) -> void:
	var particles := GPUParticles3D.new()
	particles.name = "CasterParticles"
	particles.amount = 5000
	particles.lifetime = 8.0
	particles.position = p_origin + Vector3(0.0, 0.0, 2.0)
	var process := ParticleProcessMaterial.new()
	process.gravity = Vector3(0.0, 0.0, 0.0)
	process.initial_velocity_min = 0.1
	process.initial_velocity_max = 0.4
	process.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_BOX
	process.emission_box_extents = Vector3(6.0, 2.0, 1.5)
	process.direction = Vector3(0.0, 1.0, 0.0)
	process.spread = 180.0
	particles.process_material = process
	var box := BoxMesh.new()
	box.size = Vector3(0.1, 0.1, 0.1)
	box.material = _mat("CasterParticle", Color(0.45, 0.6, 0.45), 0.8)
	particles.draw_pass_1 = box
	Scaffold.add_owned(p_root, particles, p_root)


static func _add_foliage_casters(p_root: Node3D, p_origin: Vector3) -> void:
	var mesh := _foliage_cards()
	var material := _alpha_material("GalleryFoliage", Color(0.2, 0.42, 0.16))
	var node := MeshInstance3D.new()
	node.name = "CasterFoliage"
	node.mesh = mesh
	node.material_override = material
	node.position = p_origin + Vector3(0.0, 1.2, 2.5)
	Scaffold.add_owned(p_root, node, p_root)


static func _add_vertex_animated_caster(p_root: Node3D, p_origin: Vector3) -> void:
	var plane := PlaneMesh.new()
	plane.size = Vector2(3.0, 3.0)
	var shader := Shader.new()
	shader.code = """shader_type spatial;
uniform vec3 tint = vec3(0.62, 0.42, 0.22);
void vertex() {
	VERTEX.y += sin(TIME * 2.0 + VERTEX.x * 3.0) * 0.12;
}
void fragment() {
	ALBEDO = tint;
	ROUGHNESS = 0.8;
}"""
	var material := ShaderMaterial.new()
	material.shader = shader
	var node := MeshInstance3D.new()
	node.name = "CasterVertexAnimated"
	node.mesh = plane
	node.material_override = material
	node.position = p_origin + Vector3(0.0, 1.5, 2.5)
	node.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	Scaffold.add_owned(p_root, node, p_root)


# --- VSM Flythrough ----------------------------------------------------------

## The phase smoke scene (§22.3): a 600 m street with two building rows, a
## sun, 16 shadowed local lights, one moving 2 m actor and a 60 s flight path.
static func build_vsm_flythrough() -> Dictionary:
	var root := Node3D.new()
	root.name = "VSMFlythrough"
	root.set_script(load("res://scenes/vsm_flythrough/flythrough.gd"))
	root.set_meta("skip_runtime", true)
	root.set_meta("seed", Scaffold.stream_seed(FLYTHROUGH_STREAM))
	root.set_meta("local_lights", FLYTHROUGH_LIGHTS)
	_add_world(root, Color(0.55, 0.68, 0.82), 0.7)
	_add_sun(root, Vector3(-0.35, -0.8, -0.45), 1.5, 0.5, "Sun")

	var asphalt := _mat("StreetAsphalt", Color(0.12, 0.12, 0.13), 0.88)
	var wall_a := _mat("StreetWallA", Color(0.62, 0.58, 0.5), 0.85)
	var wall_b := _mat("StreetWallB", Color(0.38, 0.42, 0.48), 0.8)
	_add_ground(root, Vector2(640.0, 30.0), asphalt, Vector3(0.0, 0.0, 0.0))
	var sidewalk := _mat("StreetSidewalk", Color(0.5, 0.5, 0.48), 0.9)
	_add_box(root, root, "SidewalkA", Vector3(0.0, 0.08, -18.0), Vector3(620.0, 0.16, 6.0), sidewalk)
	_add_box(root, root, "SidewalkB", Vector3(0.0, 0.08, 18.0), Vector3(620.0, 0.16, 6.0), sidewalk)

	var rng := Scaffold.make_rng(FLYTHROUGH_STREAM + 1)
	for index in range(40):
		var x := -300.0 + float(index) * 15.4
		for side in range(2):
			var height := rng.randf_range(8.0, 30.0)
			var depth := rng.randf_range(8.0, 14.0)
			var z := -24.0 - depth * 0.5 if side == 0 else 24.0 + depth * 0.5
			_add_box(root, root, "Building%d_%d" % [side, index],
					Vector3(x, height * 0.5, z), Vector3(12.0, height, depth),
					wall_a if side == 0 else wall_b)
			if index % 3 == 0:
				_add_box(root, root, "Shop%d_%d" % [side, index],
						Vector3(x, 2.0, z + (depth * 0.5 + 0.1) * (1.0 if side == 0 else -1.0)),
						Vector3(9.0, 3.2, 0.2), _mat("ShopFront", Color(0.7, 0.55, 0.3), 0.5))

	# 16 shadowed local lights: alternating omni and spot down the street.
	for index in range(FLYTHROUGH_LIGHTS):
		var x := -280.0 + float(index) * 37.0
		var side := -1.0 if index % 2 == 0 else 1.0
		if index % 4 < 2:
			var omni := OmniLight3D.new()
			omni.name = "StreetOmni%02d" % index
			omni.position = Vector3(x, 7.0, side * 14.0)
			omni.light_color = Color(1.0, 0.86, 0.66)
			omni.light_energy = 3.0
			omni.omni_range = 22.0
			omni.shadow_enabled = true
			Scaffold.add_owned(root, omni, root)
		else:
			var spot := SpotLight3D.new()
			spot.name = "StreetSpot%02d" % index
			var position := Vector3(x, 9.0, side * 15.0)
			spot.transform = Transform3D(Basis.looking_at((Vector3(x + 10.0, 0.0, 0.0) - position).normalized(), Vector3.UP), position)
			spot.light_color = Color(1.0, 0.9, 0.72)
			spot.light_energy = 4.0
			spot.spot_range = 40.0
			spot.spot_angle = 32.0
			spot.shadow_enabled = true
			Scaffold.add_owned(root, spot, root)

	# One moving actor: 2 m cube crossing the street at 5 m/s along +X.
	var actor := MeshInstance3D.new()
	actor.name = "Actor"
	var actor_mesh := BoxMesh.new()
	actor_mesh.size = Vector3(2.0, 2.0, 2.0)
	actor.mesh = actor_mesh
	actor.material_override = _mat("Actor", Color(0.7, 0.25, 0.2), 0.6)
	actor.position = Vector3(-40.0, 1.0, 0.0)
	Scaffold.add_owned(root, actor, root)

	var cameras := _add_cameras(root, [
		{"name": "cam_main", "pos": Vector3(-40.0, 3.0, 9.0), "target": Vector3(20.0, 2.0, 0.0), "far": 400.0, "fov": 65.0, "current": true},
		{"name": "cam_street", "pos": Vector3(0.0, 2.0, 4.0), "target": Vector3(60.0, 3.0, 0.0), "far": 400.0, "fov": 70.0},
		{"name": "cam_high", "pos": Vector3(-20.0, 24.0, 30.0), "target": Vector3(40.0, 4.0, 0.0), "far": 500.0, "fov": 55.0},
		{"name": "cam_actor", "pos": Vector3(-44.0, 2.5, 5.0), "target": Vector3(-40.0, 1.0, 0.0), "far": 120.0, "fov": 60.0},
		{"name": "cam_end", "pos": Vector3(280.0, 6.0, 10.0), "target": Vector3(0.0, 4.0, 0.0), "far": 700.0, "fov": 50.0},
	], [Vector3(-300.0, 6.0, 12.0), Vector3(-160.0, 3.0, 6.0), Vector3(0.0, 2.5, 3.0),
			Vector3(160.0, 4.0, 6.0), Vector3(300.0, 8.0, 12.0)])
	return {
		"root": root,
		"cameras": cameras,
		"local_lights": FLYTHROUGH_LIGHTS,
		"check": "vsm_flythrough",
	}


# --- Fog Hall ----------------------------------------------------------------

static func build_fog_hall() -> Dictionary:
	var root := Node3D.new()
	root.name = "FogHall"
	root.set_meta("seed", Scaffold.stream_seed(FOG_STREAM))
	root.set_meta("window_spots", FOG_SPOTS)
	_add_world(root, Color(0.04, 0.05, 0.07), 0.2, 0.08)
	_add_sun(root, Vector3(-0.3, -0.7, -0.5), 0.7, 0.5, "Sun")
	var stone := _mat("FogStone", Color(0.42, 0.41, 0.4), 0.9)
	var wall := _mat("FogWall", Color(0.28, 0.29, 0.31), 0.88)
	_add_ground(root, Vector2(44.0, 24.0), stone, Vector3(0.0, 0.0, 0.0))
	_add_box(root, root, "HallBack", Vector3(0.0, 5.0, -10.0), Vector3(44.0, 10.0, 0.5), wall)
	for index in range(6):
		var x := -20.0 + float(index) * 8.0
		_add_box(root, root, "HallSideA%d" % index, Vector3(x, 5.0, 10.0), Vector3(6.5, 10.0, 0.5), wall)
		_add_box(root, root, "HallSideB%d" % index, Vector3(x, 5.0, -9.5), Vector3(6.5, 10.0, 0.5), wall)

	var glass := _glass("FogGlass", Color(0.55, 0.72, 0.85), 0.22)
	for index in range(FOG_SPOTS):
		var x := -18.0 + float(index) * 5.0
		_add_box(root, root, "Window%d" % index, Vector3(x, 4.2, -9.7), Vector3(2.4, 3.2, 0.08), glass)
		var spot := SpotLight3D.new()
		spot.name = "WindowSpot%d" % index
		var position := Vector3(x, 6.0, -16.0)
		spot.transform = Transform3D(Basis.looking_at((Vector3(x, 2.0, 2.0) - position).normalized(), Vector3.UP), position)
		spot.light_color = Color(1.0, 0.92, 0.78)
		spot.light_energy = 5.0
		spot.spot_range = 30.0
		spot.spot_angle = 22.0
		spot.shadow_enabled = true
		Scaffold.add_owned(root, spot, root)

	var cameras := _add_cameras(root, [
		{"name": "cam_main", "pos": Vector3(0.0, 2.2, 6.0), "target": Vector3(0.0, 2.5, -8.0), "far": 80.0, "fov": 65.0, "current": true},
		{"name": "cam_windows", "pos": Vector3(0.0, 2.0, -2.0), "target": Vector3(0.0, 4.0, -10.0), "far": 60.0, "fov": 70.0},
		{"name": "cam_side", "pos": Vector3(18.0, 2.5, 6.0), "target": Vector3(-6.0, 3.0, -6.0), "far": 80.0, "fov": 60.0},
		{"name": "cam_high", "pos": Vector3(0.0, 9.0, 12.0), "target": Vector3(0.0, 2.0, -6.0), "far": 100.0, "fov": 55.0},
	], [Vector3(-18.0, 3.0, 8.0), Vector3(0.0, 2.5, 3.0), Vector3(18.0, 3.0, 8.0)])
	return {
		"root": root,
		"cameras": cameras,
		"window_spots": FOG_SPOTS,
		"check": "fog_hall",
	}


# --- RT Shadow Yard ----------------------------------------------------------

## Sun + 64 local lights over an outdoor yard with alpha foliage, colored
## glass and a 20-layer caster-mask matrix: matrix light i casts only layer i,
## the other 44 lights cast every layer.
static func build_rt_shadow_yard() -> Dictionary:
	var root := Node3D.new()
	root.name = "RTShadowYard"
	root.set_meta("seed", Scaffold.stream_seed(YARD_STREAM))
	root.set_meta("local_lights", YARD_LIGHTS)
	root.set_meta("caster_layers", YARD_CASTER_LAYERS)
	_add_world(root, Color(0.52, 0.66, 0.8), 0.8)
	_add_sun(root, Vector3(-0.55, -0.55, -0.3), 1.7, 0.5, "Sun")
	var concrete := _mat("YardConcrete", Color(0.42, 0.41, 0.38), 0.95)
	_add_ground(root, Vector2(120.0, 120.0), concrete, Vector3(0.0, 0.0, 0.0))

	# 20 occluders on layers 1..20; 20 matrix lights each cast one layer.
	for layer in range(YARD_CASTER_LAYERS):
		var occluder := _add_box(root, root, "LayerOccluder%02d" % layer,
				Vector3(-19.0 + float(layer) * 2.0, 1.5, -12.0), Vector3(1.2, 3.0, 1.2),
				_mat("LayerCaster%02d" % layer, Color(0.6, 0.6, 0.58), 0.8))
		occluder.layers = 1 << layer
	for index in range(YARD_CASTER_LAYERS):
		var position := Vector3(-19.0 + float(index) * 2.0, 7.0, 2.0)
		var spot := SpotLight3D.new()
		spot.name = "MatrixLight%02d" % index
		spot.transform = Transform3D(Basis.looking_at((Vector3(position.x, 1.0, -12.0) - position).normalized(), Vector3.UP), position)
		spot.light_color = Color(1.0, 0.9, 0.75)
		spot.light_energy = 6.0
		spot.spot_range = 30.0
		spot.spot_angle = 20.0
		spot.shadow_enabled = true
		spot.shadow_caster_mask = 1 << index
		Scaffold.add_owned(root, spot, root)

	var rng := Scaffold.make_rng(YARD_STREAM + 1)
	for index in range(YARD_LIGHTS - YARD_CASTER_LAYERS):
		var x := rng.randf_range(-45.0, 45.0)
		var z := rng.randf_range(-45.0, 45.0)
		var omni := OmniLight3D.new()
		omni.name = "YardOmni%02d" % index
		omni.position = Vector3(x, rng.randf_range(2.0, 6.0), z)
		omni.light_color = Color(1.0, 0.85, 0.65)
		omni.light_energy = 3.0
		omni.omni_range = 18.0
		omni.shadow_enabled = true
		Scaffold.add_owned(root, omni, root)

	_add_foliage_casters(root, Vector3(-6.0, 0.0, 6.0))
	_add_foliage_casters(root, Vector3(8.0, 0.0, 10.0))
	var colors := [Color(0.8, 0.2, 0.2), Color(0.2, 0.7, 0.3), Color(0.2, 0.35, 0.85)]
	for index in range(3):
		_add_box(root, root, "YardGlass%d" % index,
				Vector3(-6.0 + float(index) * 6.0, 3.0, -20.0), Vector3(3.0, 6.0, 0.1),
				_glass("YardGlassMat%d" % index, colors[index], 0.35))
	for index in range(8):
		_add_box(root, root, "YardPillar%d" % index,
				Vector3(-28.0 + float(index) * 8.0, 4.0, 20.0), Vector3(1.5, 8.0, 1.5),
				_mat("YardPillarMat", Color(0.5, 0.48, 0.45), 0.9))

	var cameras := _add_cameras(root, [
		{"name": "cam_main", "pos": Vector3(0.0, 3.0, 26.0), "target": Vector3(0.0, 3.0, -10.0), "far": 200.0, "fov": 60.0, "current": true},
		{"name": "cam_layer", "pos": Vector3(0.0, 4.0, 6.0), "target": Vector3(0.0, 2.0, -12.0), "far": 80.0, "fov": 55.0},
		{"name": "cam_foliage", "pos": Vector3(0.0, 2.5, 16.0), "target": Vector3(4.0, 2.0, 8.0), "far": 80.0, "fov": 60.0},
		{"name": "cam_high", "pos": Vector3(-40.0, 30.0, 50.0), "target": Vector3(0.0, 2.0, 0.0), "far": 300.0, "fov": 50.0},
	], [Vector3(-40.0, 12.0, 40.0), Vector3(-10.0, 4.0, 20.0), Vector3(20.0, 5.0, 16.0), Vector3(40.0, 12.0, 30.0)])
	return {
		"root": root,
		"cameras": cameras,
		"local_lights": YARD_LIGHTS,
		"caster_layers": YARD_CASTER_LAYERS,
		"check": "rt_shadow_yard",
	}


# --- MegaLights Hall ---------------------------------------------------------

## 64 base lights covering point, spot, rect, textured rect, IES and lighting
## channels, with fog, front-layer glass and SSS foliage. The runtime script
## scales the count to the 1024 and 4096 variants with `--lights=N`.
static func build_mega_lights_hall() -> Dictionary:
	var root := Node3D.new()
	root.name = "MegaLightsHall"
	root.set_script(load("res://scenes/mega_lights_hall/hall.gd"))
	root.set_meta("skip_runtime", true)
	root.set_meta("seed", Scaffold.stream_seed(MEGA_STREAM))
	root.set_meta("base_lights", MEGA_BASE_LIGHTS)
	root.set_meta("light_counts", MEGA_COUNTS)
	_add_world(root, Color(0.03, 0.04, 0.06), 0.15, 0.05)
	_add_sun(root, Vector3(-0.4, -0.75, -0.3), 0.5, 0.5, "Sun")
	var stone := _mat("MegaStone", Color(0.3, 0.3, 0.32), 0.9)
	var wall := _mat("MegaWall", Color(0.2, 0.21, 0.24), 0.88)
	_add_ground(root, Vector2(70.0, 60.0), stone, Vector3(0.0, 0.0, 0.0))
	_add_box(root, root, "MegaBack", Vector3(0.0, 4.0, -20.0), Vector3(70.0, 8.0, 0.5), wall)
	_add_box(root, root, "MegaSideA", Vector3(-34.75, 4.0, 0.0), Vector3(0.5, 8.0, 40.0), wall)
	_add_box(root, root, "MegaSideB", Vector3(34.75, 4.0, 0.0), Vector3(0.5, 8.0, 40.0), wall)

	# 64 base lights: 40 point, 8 spot, 8 rect, 4 textured rect, 4 IES.
	var rng := Scaffold.make_rng(MEGA_STREAM + 1)
	var lights := Node3D.new()
	lights.name = "Lights"
	Scaffold.add_owned(root, lights, root)
	for index in range(42):
		var omni := OmniLight3D.new()
		omni.name = "Point%02d" % index
		omni.position = Vector3(-30.0 + float(index % 16) * 4.0, 2.0 + float(index / 16) * 2.0, -14.0 + float(index / 16) * 10.0)
		omni.light_color = Color(1.0, 0.7 + rng.randf_range(0.0, 0.25), 0.5)
		omni.light_energy = 2.0
		omni.omni_range = 7.0
		omni.shadow_enabled = true
		Scaffold.add_owned(lights, omni, root)
	for index in range(8):
		var spot := SpotLight3D.new()
		spot.name = "Spot%02d" % index
		var position := Vector3(-28.0 + float(index) * 8.0, 7.0, 16.0)
		spot.transform = Transform3D(Basis.looking_at((Vector3(position.x, 0.0, -6.0) - position).normalized(), Vector3.UP), position)
		spot.light_color = Color(0.8, 0.85, 1.0)
		spot.light_energy = 8.0
		spot.spot_range = 40.0
		spot.spot_angle = 24.0
		spot.shadow_enabled = true
		Scaffold.add_owned(lights, spot, root)
	var area_texture := _radial_texture("MegaAreaTexture", Color(0.9, 0.9, 0.9), Color(0.0, 0.0, 0.0))
	for index in range(8):
		var area := AreaLight3D.new()
		area.name = "Rect%02d" % index
		area.position = Vector3(-28.0 + float(index) * 8.0, 5.5, -18.0)
		area.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
		area.area_size = Vector2(3.0, 1.5)
		area.light_color = Color(1.0, 0.95, 0.85)
		area.light_energy = 4.0
		area.area_texture = area_texture if index >= 4 else null
		area.shadow_enabled = true
		Scaffold.add_owned(lights, area, root)
	for index in range(4):
		var spot := SpotLight3D.new()
		spot.name = "Ies%02d" % index
		var position := Vector3(-24.0 + float(index) * 16.0, 6.0, 10.0)
		spot.transform = Transform3D(Basis.looking_at((Vector3(position.x, 0.0, -12.0) - position).normalized(), Vector3.UP), position)
		spot.light_color = Color(1.0, 0.9, 0.7)
		spot.light_energy = 10.0
		spot.spot_range = 45.0
		spot.spot_angle = 26.0
		spot.shadow_enabled = true
		spot.light_projector = _ies_projector("street_asymmetric")
		Scaffold.add_owned(lights, spot, root)
	# Two lights reserved for the lighting-channel checks (layers 19 and 20).
	for index in range(2):
		var channel := OmniLight3D.new()
		channel.name = "ChannelLight%d" % index
		channel.position = Vector3(-6.0 + float(index) * 12.0, 1.5, 4.0)
		channel.light_color = Color(0.4, 1.0, 0.5)
		channel.light_energy = 3.0
		channel.omni_range = 8.0
		channel.light_cull_mask = 1 << (18 + index)
		channel.shadow_enabled = true
		Scaffold.add_owned(lights, channel, root)
	root.set_meta("base_light_nodes", 64)

	for index in range(4):
		_add_box(root, root, "FrontGlass%d" % index,
				Vector3(-24.0 + float(index) * 16.0, 3.0, 18.0), Vector3(6.0, 6.0, 0.1),
				_glass("MegaGlass%d" % index, Color(0.3, 0.6, 0.8), 0.25))
	for index in range(6):
		var foliage := MeshInstance3D.new()
		foliage.name = "SssFoliage%d" % index
		foliage.mesh = _foliage_cards()
		var sss := _alpha_material("MegaSss%d" % index, Color(0.18, 0.45, 0.18))
		sss.set_subsurface_scattering_strength(0.8)
		foliage.material_override = sss
		foliage.position = Vector3(-18.0 + float(index) * 7.0, 1.2, 6.0)
		Scaffold.add_owned(root, foliage, root)

	var cameras := _add_cameras(root, [
		{"name": "cam_main", "pos": Vector3(0.0, 2.0, 22.0), "target": Vector3(0.0, 2.5, -10.0), "far": 120.0, "fov": 65.0, "current": true},
		{"name": "cam_lights", "pos": Vector3(0.0, 5.0, 12.0), "target": Vector3(0.0, 3.0, -16.0), "far": 100.0, "fov": 60.0},
		{"name": "cam_side", "pos": Vector3(28.0, 2.5, 12.0), "target": Vector3(-10.0, 2.5, -8.0), "far": 120.0, "fov": 60.0},
		{"name": "cam_high", "pos": Vector3(0.0, 7.5, 30.0), "target": Vector3(0.0, 1.0, -8.0), "far": 150.0, "fov": 55.0},
	], [Vector3(-26.0, 4.0, 16.0), Vector3(-8.0, 2.5, 6.0), Vector3(10.0, 2.5, 4.0), Vector3(26.0, 4.0, 14.0)])
	return {
		"root": root,
		"cameras": cameras,
		"base_lights": MEGA_BASE_LIGHTS,
		"light_counts": MEGA_COUNTS,
		"check": "mega_lights_hall",
	}


# --- IES and texture helpers -------------------------------------------------

## Parses the committed LM-63-2002 profile and maps its vertical candela
## sweep into a circular projector texture, so the scene's IES lights use the
## same analytic distribution the runtime loader will read.
static func _ies_projector(p_name: String) -> Texture2D:
	var text := FileAccess.get_file_as_string(IES_DIR + "/" + p_name + ".ies")
	var lines := PackedStringArray()
	for raw in text.split("\n"):
		var line := raw.strip_edges()
		if not line.is_empty():
			lines.append(line)
	var tilt := lines.find("TILT=NONE")
	if tilt < 0 or tilt + 4 >= lines.size():
		return _radial_texture("IesFallback_" + p_name, Color.WHITE, Color.BLACK)
	var vertical := PackedFloat32Array()
	for value in lines[tilt + 3].split(" ", false):
		vertical.append(float(value))
	var candela := PackedFloat32Array()
	for value in lines[tilt + 5].split(" ", false):
		candela.append(float(value))
	var peak := 0.0
	for value in candela:
		peak = maxf(peak, value)
	if peak <= 0.0:
		return _radial_texture("IesFallback_" + p_name, Color.WHITE, Color.BLACK)
	var size := 64
	var image := Image.create_empty(size, size, false, Image.FORMAT_RGBA8)
	for y in range(size):
		for x in range(size):
			var dx := (float(x) + 0.5 - float(size) * 0.5) / (float(size) * 0.5)
			var dy := (float(y) + 0.5 - float(size) * 0.5) / (float(size) * 0.5)
			var radius := sqrt(dx * dx + dy * dy)
			var intensity := 0.0
			if radius <= 1.0:
				var angle := radius * 90.0
				intensity = _sample_candela(vertical, candela, angle) / peak
			image.set_pixel(x, y, Color(intensity, intensity, intensity, 1.0))
	return ImageTexture.create_from_image(image)


static func _sample_candela(p_angles: PackedFloat32Array, p_values: PackedFloat32Array, p_angle: float) -> float:
	if p_angles.is_empty() or p_values.size() != p_angles.size():
		return 0.0
	for index in range(1, p_angles.size()):
		if p_angle <= p_angles[index]:
			var t := (p_angle - p_angles[index - 1]) / maxf(p_angles[index] - p_angles[index - 1], 0.0001)
			return lerpf(p_values[index - 1], p_values[index], clampf(t, 0.0, 1.0))
	return p_values[p_values.size() - 1]


static func _radial_texture(p_name: String, p_inner: Color, p_outer: Color) -> GradientTexture2D:
	var ramp := Gradient.new()
	ramp.set_color(0, p_inner)
	ramp.set_color(1, p_outer)
	var texture := GradientTexture2D.new()
	texture.resource_name = p_name
	texture.gradient = ramp
	texture.fill = GradientTexture2D.FILL_RADIAL
	texture.fill_from = Vector2(0.5, 0.5)
	texture.fill_to = Vector2(0.5, 0.0)
	texture.width = 64
	texture.height = 64
	return texture


# --- Shared ------------------------------------------------------------------

static func _add_world(p_root: Node3D, p_sky: Color, p_ambient: float, p_volumetric_density: float = 0.0) -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	var sky_material := ProceduralSkyMaterial.new()
	sky_material.sky_top_color = p_sky
	sky_material.sky_horizon_color = p_sky.lerp(Color(0.2, 0.2, 0.22), 0.5)
	sky_material.ground_bottom_color = p_sky * 0.4
	sky_material.ground_horizon_color = p_sky
	sky.sky_material = sky_material
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.ambient_light_energy = p_ambient
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	if p_volumetric_density > 0.0:
		environment.volumetric_fog_enabled = true
		environment.volumetric_fog_density = p_volumetric_density
		environment.volumetric_fog_length = 64.0
		environment.volumetric_fog_albedo = Color(0.9, 0.92, 0.95)
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = environment
	Scaffold.add_owned(p_root, world_env, p_root)


static func _add_sun(p_root: Node3D, p_direction: Vector3, p_energy: float, p_angular_degrees: float, p_name: String) -> DirectionalLight3D:
	var sun := DirectionalLight3D.new()
	sun.name = p_name
	sun.transform = Transform3D(Basis.looking_at(p_direction.normalized(), Vector3.UP), Vector3(0.0, 30.0, 0.0))
	sun.light_energy = p_energy
	sun.light_angular_distance = p_angular_degrees
	sun.shadow_enabled = true
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
	sun.directional_shadow_max_distance = 200.0
	Scaffold.add_owned(p_root, sun, p_root)
	return sun


static func _add_ground(p_root: Node3D, p_size: Vector2, p_material: Material, p_position: Vector3) -> void:
	var mesh := PlaneMesh.new()
	mesh.size = p_size
	mesh.material = p_material
	var node := MeshInstance3D.new()
	node.name = "Ground"
	node.mesh = mesh
	node.position = p_position
	Scaffold.add_owned(p_root, node, p_root)


static func _add_box(p_root: Node3D, p_parent: Node3D, p_name: String, p_position: Vector3, p_size: Vector3, p_material: Material) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = p_size
	mesh.material = p_material
	var node := MeshInstance3D.new()
	node.name = p_name
	node.mesh = mesh
	node.position = p_position
	Scaffold.add_owned(p_parent, node, p_root)
	return node


static func _mat(p_name: String, p_color: Color, p_roughness: float = 0.9, p_metallic: float = 0.0) -> StandardMaterial3D:
	return Scaffold.make_constant_material(p_name, p_color, p_roughness, p_metallic)


static func _glass(p_name: String, p_color: Color, p_alpha: float) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.resource_name = p_name
	material.albedo_color = Color(p_color.r, p_color.g, p_color.b, p_alpha)
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.roughness = 0.05
	material.metallic = 0.0
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	return material


static func _alpha_material(p_name: String, p_color: Color) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.resource_name = p_name
	material.albedo_color = p_color
	material.albedo_texture = _leaf_texture()
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	material.alpha_scissor_threshold = 0.5
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.roughness = 0.85
	return material


static func _leaf_texture() -> ImageTexture:
	var image := Image.create_empty(64, 64, false, Image.FORMAT_RGBA8)
	for y in range(64):
		for x in range(64):
			var dx := (float(x) + 0.5 - 32.0) / 31.0
			var dy := (float(y) + 0.5 - 32.0) / 31.0
			var width := 1.0 - absf(dy) * 0.85
			var inside := absf(dx) < width * 0.9 and absf(dy) < 0.98
			image.set_pixel(x, y, Color(0.3, 0.6, 0.25, 1.0) if inside else Color(0.0, 0.0, 0.0, 0.0))
	return ImageTexture.create_from_image(image)


static func _foliage_cards() -> ArrayMesh:
	var tool := SurfaceTool.new()
	tool.begin(Mesh.PRIMITIVE_TRIANGLES)
	for card in range(4):
		var yaw := TAU * float(card) / 4.0
		var basis := Basis(Vector3.UP, yaw)
		var u := basis * Vector3.RIGHT * 1.1
		var v := Vector3.UP * 1.1
		var center := Vector3(0.0, 1.1, 0.0)
		_quad(tool, center - u - v, center + u - v, center + u + v, center - u + v, basis * Vector3.BACK)
	tool.generate_normals()
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, tool.commit_to_arrays())
	mesh.resource_name = "FoliageCards"
	return mesh


static func _quad(p_tool: SurfaceTool, p0: Vector3, p1: Vector3, p2: Vector3, p3: Vector3, p_normal: Vector3) -> void:
	p_tool.set_normal(p_normal)
	p_tool.set_uv(Vector2(0.0, 1.0))
	p_tool.add_vertex(p0)
	p_tool.set_uv(Vector2(1.0, 1.0))
	p_tool.add_vertex(p1)
	p_tool.set_uv(Vector2(1.0, 0.0))
	p_tool.add_vertex(p2)
	p_tool.set_uv(Vector2(0.0, 1.0))
	p_tool.add_vertex(p0)
	p_tool.set_uv(Vector2(1.0, 0.0))
	p_tool.add_vertex(p2)
	p_tool.set_uv(Vector2(0.0, 0.0))
	p_tool.add_vertex(p3)


static func _add_cameras(p_root: Node3D, p_specs: Array, p_path: Array) -> Array[String]:
	var names: Array[String] = []
	for spec in p_specs:
		var camera := Scaffold.make_camera(p_root, p_root, String(spec["name"]),
				spec["pos"], spec["target"], float(spec.get("far", 200.0)),
				float(spec.get("fov", 70.0)), bool(spec.get("current", false)))
		names.append(String(camera.name))
	var flight := Path3D.new()
	flight.name = "FlightPath"
	var curve := Curve3D.new()
	for point in p_path:
		curve.add_point(point)
	flight.curve = curve
	Scaffold.add_owned(p_root, flight, p_root)
	return names


static func _verify(p_root: Node3D, p_built: Dictionary, p_failures: Array[String], p_scene: String) -> bool:
	var before := p_failures.size()
	match String(p_built.get("check", "")):
		"shadow_gallery":
			var sun_a := p_root.get_node_or_null("SunA") as DirectionalLight3D
			var sun_b := p_root.get_node_or_null("SunB") as DirectionalLight3D
			if sun_a == null or not is_equal_approx(sun_a.light_angular_distance, 0.5):
				p_failures.append("%s: SunA is missing or not 0.5 degrees." % p_scene)
			if sun_b == null or not is_equal_approx(sun_b.light_angular_distance, 2.0):
				p_failures.append("%s: SunB is missing or not 2 degrees." % p_scene)
			var lights := _count_lights(p_root)
			if lights != int(p_built["local_lights"]):
				p_failures.append("%s: %d local lights, expected %d." % [p_scene, lights, int(p_built["local_lights"])])
			for node_name in ["CasterMultiMesh", "CasterParticles", "CasterFoliage", "CasterVertexAnimated", "GallerySkinned"]:
				if p_root.get_node_or_null(node_name) == null:
					p_failures.append("%s: %s is missing." % [p_scene, node_name])
		"vsm_flythrough":
			if _count_lights(p_root) != int(p_built["local_lights"]):
				p_failures.append("%s: %d local lights, expected %d." % [p_scene, _count_lights(p_root), int(p_built["local_lights"])])
			if p_root.get_node_or_null("Actor") == null:
				p_failures.append("%s: Actor is missing." % p_scene)
			if p_root.get_node_or_null("FlightPath") == null:
				p_failures.append("%s: FlightPath is missing." % p_scene)
		"fog_hall":
			var world_env := p_root.get_node_or_null("WorldEnvironment") as WorldEnvironment
			if world_env == null or not world_env.environment.volumetric_fog_enabled:
				p_failures.append("%s: volumetric fog is not enabled." % p_scene)
			if _count_spots(p_root) < int(p_built["window_spots"]):
				p_failures.append("%s: %d window spots, expected %d." % [p_scene, _count_spots(p_root), int(p_built["window_spots"])])
		"rt_shadow_yard":
			if _count_lights(p_root) < int(p_built["local_lights"]):
				p_failures.append("%s: %d local lights, expected %d." % [p_scene, _count_lights(p_root), int(p_built["local_lights"])])
			var layers := 0
			for child in p_root.get_children():
				if child is MeshInstance3D and String(child.name).begins_with("LayerOccluder"):
					layers += 1
			if layers != int(p_built["caster_layers"]):
				p_failures.append("%s: %d caster layers, expected %d." % [p_scene, layers, int(p_built["caster_layers"])])
			var matrix_masks := 0
			for child in p_root.get_children():
				if child is SpotLight3D and String(child.name).begins_with("MatrixLight"):
					matrix_masks += 1
			if matrix_masks != int(p_built["caster_layers"]):
				p_failures.append("%s: %d matrix lights, expected %d." % [p_scene, matrix_masks, int(p_built["caster_layers"])])
		"mega_lights_hall":
			var lights := p_root.get_node_or_null("Lights")
			if lights == null or lights.get_child_count() != int(p_built["base_lights"]):
				p_failures.append("%s: %d base lights, expected %d." % [p_scene, lights.get_child_count() if lights else 0, int(p_built["base_lights"])])
			var world_env := p_root.get_node_or_null("WorldEnvironment") as WorldEnvironment
			if world_env == null or not world_env.environment.volumetric_fog_enabled:
				p_failures.append("%s: volumetric fog is not enabled." % p_scene)
	return p_failures.size() == before


static func _count_lights(p_root: Node3D) -> int:
	var count := 0
	for child in p_root.get_children():
		if child is Light3D and not (child is DirectionalLight3D):
			count += 1
	return count


static func _count_spots(p_root: Node3D) -> int:
	var count := 0
	for child in p_root.get_children():
		if child is SpotLight3D:
			count += 1
	return count
