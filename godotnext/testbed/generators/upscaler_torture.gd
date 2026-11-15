extends SceneTree

const Scaffold := preload("res://generators/scaffold.gd")
const SEED := 0x6E657874
const SCENE_PATH := "res://scenes/upscaler_torture/upscaler_torture.tscn"
## GT-2 content masks (#190): the moiré grilles and the thin cables.
const MOIRE_LAYER := 1 << 17
const THIN_WIRE_LAYER := 1 << 18
const ALPHA_PARTICLES_LAYER := 1 << 19
const SCROLLING_EMISSIVE_LAYER := 1 << 20
const ALL_TORTURE_LAYERS := 0xFFFFF | SCROLLING_EMISSIVE_LAYER

## The static camera (frames 0-199 and the static variant). The GT-2 grilles and cables
## are sized for it at 1440p internal (#190).
const STATIC_CAMERA_POSITION := Vector3(0.0, 1.75, 4.8)
const CAMERA_TARGET := Vector3(0.0, 1.1, -0.4)
const CAMERA_FOV := 58.0
const GT_INTERNAL_SIZE := Vector2i(2560, 1440)
const GRILLE_PERIOD_PX := Vector2(1.0, 4.0)
const CABLE_WIDTH_PX := Vector2(0.3, 1.0)

## Two low grilles of vertical bars on the floor at either side, receding from the camera at a
## grazing angle, so their projected period runs from about 1 px (far) to 4 px (near). A dark
## backing panel sits behind the bars, so the moiré mask covers the whole grille.
const GRILLES: Array[Dictionary] = [
	{"name": "MoireGrilleLeft", "x": -2.4, "outward": -1.0},
	{"name": "MoireGrilleRight", "x": 2.4, "outward": 1.0},
]
const GRILLE_Z_NEAR := 1.7
const GRILLE_Z_FAR := -1.3
const GRILLE_Y := Vector2(0.02, 0.9)
const GRILLE_PERIOD := 0.0129
const GRILLE_BACKING_OFFSET := 0.03

## Twelve cables, six against the background (sky) above the backdrop and six against the
## checker backdrop. Each one is tilted slightly so it crosses pixel rows. Its diameter gives the
## listed projected width at 1440p internal from the static camera.
const CABLES: Array[Dictionary] = [
	{"against": "sky", "y": 3.045, "z": 1.4, "length": 12.0, "tilt": 0.45, "width_px": 0.34},
	{"against": "sky", "y": 3.057, "z": 1.1, "length": 12.0, "tilt": -0.55, "width_px": 0.453},
	{"against": "sky", "y": 3.052, "z": 0.8, "length": 12.0, "tilt": 0.65, "width_px": 0.566},
	{"against": "sky", "y": 3.029, "z": 0.5, "length": 12.0, "tilt": -0.45, "width_px": 0.679},
	{"against": "sky", "y": 2.989, "z": 0.2, "length": 12.0, "tilt": 0.55, "width_px": 0.792},
	{"against": "sky", "y": 2.93, "z": -0.1, "length": 12.0, "tilt": -0.65, "width_px": 0.904},
	{"against": "geometry", "y": 2.75, "z": -1.9, "length": 5.6, "tilt": 0.9, "width_px": 0.396},
	{"against": "geometry", "y": 2.35, "z": -1.9, "length": 5.6, "tilt": -1.1, "width_px": 0.509},
	{"against": "geometry", "y": 1.95, "z": -1.9, "length": 5.6, "tilt": 1.0, "width_px": 0.622},
	{"against": "geometry", "y": 1.55, "z": -1.9, "length": 5.6, "tilt": -0.9, "width_px": 0.735},
	{"against": "geometry", "y": 1.15, "z": -1.9, "length": 5.6, "tilt": 1.1, "width_px": 0.848},
	{"against": "geometry", "y": 0.75, "z": -1.9, "length": 5.6, "tilt": -1.0, "width_px": 0.96},
]


func _init() -> void:
	call_deferred("_run")


func _run() -> void:
	var built := build()
	var err := Scaffold.save_scene(built["root"], SCENE_PATH)
	built["root"].free()
	print("GODOTNEXT_RESULT " + JSON.stringify({"test": "upscaler_torture_generator", "pass": err == OK, "seed": SEED}))
	quit(err)


static func build() -> Dictionary:
	DirAccess.make_dir_recursive_absolute("res://scenes/upscaler_torture")
	var rng := RandomNumberGenerator.new()
	rng.seed = SEED
	var root := Node3D.new()
	root.name = "UpscalerTorture"
	root.set_script(load("res://scenes/upscaler_torture/torture_timeline.gd"))
	_add_environment(root)
	_add_wires(root, rng)
	_add_fence(root)
	_add_checker_and_moire(root)
	_add_particles(root)
	_add_occluders(root)
	_add_emissive(root)
	_add_camera(root)
	_add_moire_grilles(root)
	_add_cables(root)
	return {"root": root}


static func _add(parent: Node, child: Node, root: Node3D) -> void:
	parent.add_child(child)
	child.owner = root


static func _material(color: Color, unshaded: bool = false) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = 0.65
	if unshaded:
		material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	return material


static func _mesh_node(parent: Node3D, root: Node3D, node_name: String, mesh: Mesh, material: Material, pos: Vector3, layers: int = 1) -> MeshInstance3D:
	var instance := MeshInstance3D.new()
	instance.name = node_name
	instance.mesh = mesh
	instance.material_override = material
	instance.position = pos
	instance.layers = layers
	_add(parent, instance, root)
	return instance


static func _texture(tex_name: String, kind: String, with_mipmaps: bool = false) -> ImageTexture:
	var image := Image.create_empty(256, 256, false, Image.FORMAT_RGBA8)
	for y in 256:
		for x in 256:
			var color := Color.BLACK
			match kind:
				"checker":
					color = Color(0.93, 0.91, 0.83, 1.0) if (x + y) % 2 == 0 else Color(0.015, 0.02, 0.025, 1.0)
				"moire":
					var dx := float(x - 128) / 128.0
					var dy := float(y - 128) / 128.0
					var wave := sin(sqrt(dx * dx + dy * dy) * 150.0)
					color = Color(0.82, 0.86, 0.95, 1.0) if wave > 0.0 else Color(0.02, 0.025, 0.04, 1.0)
				"fence":
					var diamond := absf(float(x % 32) - 15.5) + absf(float(y % 32) - 15.5)
					color = Color(0.72, 0.76, 0.8, 1.0) if absf(diamond - 15.5) < 1.5 else Color(0.0, 0.0, 0.0, 0.0)
				"stripes":
					var band := (x / 8) % 4
					if band == 0:
						color = Color(0.95, 0.85, 0.3, 1.0)
					elif band == 2:
						color = Color(0.25, 0.8, 0.95, 1.0)
					else:
						color = Color(0.02, 0.02, 0.03, 1.0)
			image.set_pixel(x, y, color)
	if with_mipmaps:
		image.generate_mipmaps()
	var texture := ImageTexture.create_from_image(image)
	texture.resource_name = tex_name
	texture.resource_path = "res://scenes/upscaler_torture/%s.res" % tex_name
	var err := ResourceSaver.save(texture, texture.resource_path)
	assert(err == OK)
	return texture


static func _add_environment(root: Node3D) -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.12, 0.16, 0.22)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.65, 0.72, 0.85)
	environment.ambient_light_energy = 0.35
	environment.sdfgi_enabled = false
	environment.nextgi_enabled = false
	var world := WorldEnvironment.new()
	world.name = "LegacyLighting"
	world.environment = environment
	_add(root, world, root)
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.rotation_degrees = Vector3(-38, -32, 0)
	sun.light_energy = 1.2
	sun.shadow_enabled = true
	_add(root, sun, root)
	var fill := OmniLight3D.new()
	fill.name = "Fill"
	fill.position = Vector3(-2.0, 3.0, 3.0)
	fill.light_color = Color(0.65, 0.8, 1.0)
	fill.light_energy = 2.0
	fill.omni_range = 9.0
	_add(root, fill, root)


static func _add_wires(root: Node3D, rng: RandomNumberGenerator) -> void:
	var parent := Node3D.new()
	parent.name = "ThinWires"
	_add(root, parent, root)
	var wire_mat := _material(Color(0.76, 0.78, 0.82))
	for index in 24:
		var diameter := 0.002 + float(index % 4) / 3.0 * 0.003
		var cylinder := CylinderMesh.new()
		cylinder.top_radius = diameter * 0.5
		cylinder.bottom_radius = diameter * 0.5
		cylinder.height = 2.5
		cylinder.radial_segments = 8
		cylinder.rings = 1
		if index == 0:
			cylinder.resource_path = "res://scenes/upscaler_torture/wire_2mm.res"
			var err := ResourceSaver.save(cylinder, cylinder.resource_path)
			assert(err == OK)
		var jitter_z := (rng.randf() - 0.5) * 0.08
		var tilt_deg := (rng.randf() - 0.5) * 14.0
		var wire := _mesh_node(
			parent,
			root,
			"Wire%02d" % index,
			cylinder,
			wire_mat,
			Vector3((float(index % 12) - 5.5) * 0.09, 1.35, -0.55 - float(index / 12) * 0.14 + jitter_z)
		)
		wire.rotation_degrees = Vector3(0.0, 0.0, tilt_deg)
		wire.set_meta("diameter_mm", diameter * 1000.0)


static func _add_fence(root: Node3D) -> void:
	var material := _material(Color.WHITE)
	material.albedo_texture = _texture("chain_link", "fence", true)
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	material.alpha_scissor_threshold = 0.5
	material.alpha_antialiasing_mode = BaseMaterial3D.ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.uv1_scale = Vector3(4.0, 2.0, 1.0)
	var fence := QuadMesh.new()
	fence.size = Vector2(2.6, 2.2)
	_mesh_node(root, root, "ChainLinkFence", fence, material, Vector3(-1.55, 1.15, -1.35))


static func _add_checker_and_moire(root: Node3D) -> void:
	var checker_tex := _texture("checker", "checker", false)
	var checker := StandardMaterial3D.new()
	checker.albedo_texture = checker_tex
	checker.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	checker.uv1_scale = Vector3(48.0, 48.0, 1.0)
	var floor_plane := PlaneMesh.new()
	floor_plane.size = Vector2(12.0, 12.0)
	_mesh_node(root, root, "CheckerPlane", floor_plane, checker, Vector3.ZERO)

	var backdrop_mat := StandardMaterial3D.new()
	backdrop_mat.albedo_texture = checker_tex
	backdrop_mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	backdrop_mat.uv1_scale = Vector3(16.0, 8.0, 1.0)
	var backdrop_quad := QuadMesh.new()
	backdrop_quad.size = Vector2(6.4, 3.2)
	_mesh_node(root, root, "CheckerBackdrop", backdrop_quad, backdrop_mat, Vector3(0.0, 1.6, -2.2))

	var moire := StandardMaterial3D.new()
	moire.albedo_texture = _texture("concentric_moire", "moire", true)
	moire.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS
	moire.cull_mode = BaseMaterial3D.CULL_DISABLED
	var moire_plane := QuadMesh.new()
	moire_plane.size = Vector2(4.6, 3.4)
	var grazing := _mesh_node(root, root, "MoirePlane", moire_plane, moire, Vector3(0.0, 0.14, 1.95))
	grazing.rotation_degrees = Vector3(-80.0, 0.0, 0.0)


static func _add_particles(root: Node3D) -> void:
	var particles := GPUParticles3D.new()
	particles.name = "AlphaParticles"
	particles.layers = ALPHA_PARTICLES_LAYER
	particles.amount = 96
	particles.lifetime = 3.0
	particles.preprocess = 1.0
	particles.fixed_fps = 60
	particles.interpolate = false
	particles.fract_delta = false
	particles.use_fixed_seed = true
	particles.seed = SEED
	particles.visibility_aabb = AABB(Vector3(-3.0, -1.0, -3.0), Vector3(6.0, 6.0, 6.0))
	var process := ParticleProcessMaterial.new()
	process.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_BOX
	process.emission_box_extents = Vector3(1.5, 0.25, 0.5)
	process.direction = Vector3(0.0, 1.0, 0.0)
	process.spread = 25.0
	process.initial_velocity_min = 0.12
	process.initial_velocity_max = 0.35
	process.gravity = Vector3(0.0, -0.06, 0.0)
	process.color = Color(0.92, 0.68, 0.28, 0.55)
	particles.process_material = process
	var quad := QuadMesh.new()
	quad.size = Vector2(0.08, 0.08)
	var material := _material(Color(0.92, 0.68, 0.28, 0.55))
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.vertex_color_use_as_albedo = true
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	quad.material = material
	particles.draw_pass_1 = quad
	particles.position = Vector3(0.0, 0.65, 0.85)
	_add(root, particles, root)


static func _box(st: SurfaceTool, center: Vector3, size: Vector3, bone: int) -> void:
	var half := size * 0.5
	var corners: Array[Vector3] = []
	for z in [-1.0, 1.0]:
		for y in [-1.0, 1.0]:
			for x in [-1.0, 1.0]:
				corners.append(center + Vector3(x * half.x, y * half.y, z * half.z))
	var faces: Array = [
		[0, 3, 2, 1, Vector3(0, 0, -1)],
		[4, 5, 7, 6, Vector3(0, 0, 1)],
		[0, 4, 5, 1, Vector3(0, -1, 0)],
		[2, 6, 7, 3, Vector3(0, 1, 0)],
		[0, 2, 6, 4, Vector3(-1, 0, 0)],
		[1, 5, 7, 3, Vector3(1, 0, 0)],
	]
	for face in faces:
		for index in [0, 1, 2, 0, 2, 3]:
			st.set_normal(face[4])
			st.set_bones(PackedInt32Array([bone, 0, 0, 0]))
			st.set_weights(PackedFloat32Array([1.0, 0.0, 0.0, 0.0]))
			st.add_vertex(corners[face[index]])


static func _add_character(parent: Node3D, root: Node3D) -> void:
	var character := Node3D.new()
	character.name = "SkinnedCharacter"
	character.position = Vector3(-0.48, 0.0, 0.25)
	_add(parent, character, root)
	var skeleton := Skeleton3D.new()
	skeleton.name = "Skeleton3D"
	_add(character, skeleton, root)
	var rests: Array[Vector3] = [
		Vector3.ZERO,
		Vector3(0.0, 0.65, 0.0),
		Vector3(-0.35, 1.0, 0.0),
		Vector3(0.35, 1.0, 0.0),
		Vector3(-0.16, 0.4, 0.0),
		Vector3(0.16, 0.4, 0.0),
	]
	var names: Array[String] = ["hips", "torso", "arm_l", "arm_r", "leg_l", "leg_r"]
	for index in rests.size():
		skeleton.add_bone(names[index])
		if index > 0:
			skeleton.set_bone_parent(index, 0)
		skeleton.set_bone_rest(index, Transform3D(Basis(), rests[index]))
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	_box(st, Vector3(0.0, 0.85, 0.0), Vector3(0.45, 0.65, 0.25), 1)
	_box(st, Vector3(-0.4, 0.8, 0.0), Vector3(0.14, 0.6, 0.14), 2)
	_box(st, Vector3(0.4, 0.8, 0.0), Vector3(0.14, 0.6, 0.14), 3)
	_box(st, Vector3(-0.14, 0.25, 0.0), Vector3(0.17, 0.55, 0.18), 4)
	_box(st, Vector3(0.14, 0.25, 0.0), Vector3(0.17, 0.55, 0.18), 5)
	_box(st, Vector3(0.0, 1.32, 0.0), Vector3(0.3, 0.28, 0.28), 1)
	var mesh := st.commit()
	mesh.resource_name = "OriginalSkinnedCharacter"
	mesh.resource_path = "res://scenes/upscaler_torture/character_mesh.res"
	var err := ResourceSaver.save(mesh, mesh.resource_path)
	assert(err == OK)
	var instance := MeshInstance3D.new()
	instance.name = "CharacterMesh"
	instance.mesh = mesh
	instance.skeleton = NodePath("../Skeleton3D")
	instance.skin = skeleton.create_skin_from_rest_transforms()
	instance.material_override = _material(Color(0.3, 0.65, 0.85))
	_add(character, instance, root)
	var animation := Animation.new()
	animation.length = 0.5
	animation.loop_mode = Animation.LOOP_LINEAR
	for bone_index in [2, 3, 4, 5]:
		var track := animation.add_track(Animation.TYPE_ROTATION_3D)
		animation.track_set_path(track, NodePath("Skeleton3D:" + names[bone_index]))
		for key in 5:
			var angle := sin(float(key) / 4.0 * TAU) * 0.65 * (1.0 if bone_index % 2 == 0 else -1.0)
			animation.rotation_track_insert_key(track, float(key) * 0.125, Quaternion.from_euler(Vector3(angle, 0.0, 0.0)))
	var library := AnimationLibrary.new()
	library.add_animation("swing", animation)
	var player := AnimationPlayer.new()
	player.name = "AnimationPlayer"
	player.callback_mode_process = AnimationMixer.ANIMATION_CALLBACK_MODE_PROCESS_MANUAL
	_add(character, player, root)
	player.add_animation_library("", library)
	player.autoplay = "swing"
	player.speed_scale = 3.0


static func _add_occluders(root: Node3D) -> void:
	var parent := Node3D.new()
	parent.name = "Occluders"
	parent.position = Vector3(-1.35, 0.0, 0.0)
	_add(root, parent, root)
	_add_character(parent, root)
	var fan := Node3D.new()
	fan.name = "Fan"
	fan.position = Vector3(0.52, 1.15, 0.25)
	_add(parent, fan, root)
	var blade_mat := _material(Color(0.78, 0.34, 0.14))
	for index in 4:
		var blade := BoxMesh.new()
		blade.size = Vector3(0.12, 0.88, 0.04)
		var instance := _mesh_node(fan, root, "Blade%d" % index, blade, blade_mat, Vector3.ZERO)
		instance.rotation.z = float(index) * PI * 0.5
	var hub := CylinderMesh.new()
	hub.top_radius = 0.14
	hub.bottom_radius = 0.14
	hub.height = 0.12
	hub.radial_segments = 16
	var hub_node := _mesh_node(fan, root, "Hub", hub, _material(Color(0.18, 0.2, 0.22)), Vector3.ZERO)
	hub_node.rotation_degrees = Vector3(90.0, 0.0, 0.0)


static func _add_emissive(root: Node3D) -> void:
	var shader := Shader.new()
	# scroll_speed is 2.0 except in the static variant (#190), which sets it to 0.
	# An unshaded material outputs ALBEDO only, so the stripes go there at x3 (#271); EMISSION
	# would never reach the frame.
	shader.code = "shader_type spatial;\nrender_mode unshaded, cull_disabled;\nuniform sampler2D stripes : source_color, filter_linear, repeat_enable;\nuniform float time_offset = 0.0;\nuniform float scroll_speed = 2.0;\nvoid fragment() {\n\tvec3 color = texture(stripes, UV + vec2(TIME * scroll_speed + time_offset, 0.0)).rgb;\n\tALBEDO = color * 3.0;\n}\n"
	var material := ShaderMaterial.new()
	material.shader = shader
	material.set_shader_parameter("stripes", _texture("emissive_stripes", "stripes", false))
	var panel := QuadMesh.new()
	panel.size = Vector2(1.35, 0.6)
	_mesh_node(root, root, "ScrollingEmissive", panel, material, Vector3(1.55, 1.75, -0.35), SCROLLING_EMISSIVE_LAYER)


static func _add_camera(root: Node3D) -> void:
	var path := Path3D.new()
	path.name = "CameraPath"
	var curve := Curve3D.new()
	curve.add_point(Vector3(0.0, 1.75, 4.8))
	curve.add_point(Vector3(2.6, 2.05, 3.4))
	curve.add_point(Vector3(-2.4, 1.45, 3.1))
	curve.add_point(Vector3(0.35, 1.85, 4.4))
	path.curve = curve
	_add(root, path, root)
	var follow := PathFollow3D.new()
	follow.name = "CameraFollow"
	follow.loop = false
	follow.rotation_mode = PathFollow3D.ROTATION_NONE
	_add(path, follow, root)
	var camera := Camera3D.new()
	camera.name = "Camera3D"
	camera.current = true
	camera.fov = 58.0
	camera.cull_mask = ALL_TORTURE_LAYERS
	_add(follow, camera, root)


static func _add_moire_grilles(root: Node3D) -> void:
	var bar_mat := _material(Color(0.88, 0.88, 0.86))
	bar_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	var backing_mat := _material(Color(0.02, 0.022, 0.026))
	backing_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	for spec in GRILLES:
		var grille := Node3D.new()
		grille.name = String(spec["name"])
		_add(root, grille, root)
		var x := float(spec["x"])
		var bars := _mesh_node(grille, root, "Bars", _grille_bars_mesh(x, String(spec["name"]).to_snake_case()), bar_mat, Vector3.ZERO, MOIRE_LAYER)
		bars.set_meta("period_m", GRILLE_PERIOD)
		var backing_quad := QuadMesh.new()
		backing_quad.size = Vector2(GRILLE_Z_NEAR - GRILLE_Z_FAR, GRILLE_Y.y - GRILLE_Y.x)
		var backing_pos := Vector3(x + float(spec["outward"]) * GRILLE_BACKING_OFFSET, (GRILLE_Y.x + GRILLE_Y.y) * 0.5, (GRILLE_Z_NEAR + GRILLE_Z_FAR) * 0.5)
		var backing := _mesh_node(grille, root, "Backing", backing_quad, backing_mat, backing_pos, MOIRE_LAYER)
		backing.rotation_degrees = Vector3(0.0, 90.0, 0.0)


## One flat quad per bar in the plane x = [param x], half a period wide along z.
static func _grille_bars_mesh(x: float, res_name: String) -> ArrayMesh:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var normal := Vector3(-signf(x), 0.0, 0.0)
	for index in grille_bar_count():
		var z0 := GRILLE_Z_NEAR - float(index) * GRILLE_PERIOD
		var z1 := z0 - GRILLE_PERIOD * 0.5
		var corners: Array[Vector3] = [
			Vector3(x, GRILLE_Y.x, z0),
			Vector3(x, GRILLE_Y.x, z1),
			Vector3(x, GRILLE_Y.y, z1),
			Vector3(x, GRILLE_Y.y, z0),
		]
		for corner in [0, 1, 2, 0, 2, 3]:
			st.set_normal(normal)
			st.add_vertex(corners[corner])
	var mesh := st.commit()
	mesh.resource_name = res_name
	mesh.resource_path = "res://scenes/upscaler_torture/%s.res" % res_name
	var err := ResourceSaver.save(mesh, mesh.resource_path)
	assert(err == OK)
	return mesh


static func grille_bar_count() -> int:
	return int(floor((GRILLE_Z_NEAR - GRILLE_Z_FAR) / GRILLE_PERIOD))


static func _add_cables(root: Node3D) -> void:
	var parent := Node3D.new()
	parent.name = "ThinCables"
	_add(root, parent, root)
	var cable_mat := _material(Color(0.86, 0.86, 0.84))
	for index in CABLES.size():
		var spec: Dictionary = CABLES[index]
		var diameter := cable_diameter(spec)
		var cylinder := CylinderMesh.new()
		cylinder.top_radius = diameter * 0.5
		cylinder.bottom_radius = diameter * 0.5
		cylinder.height = float(spec["length"])
		cylinder.radial_segments = 6
		cylinder.rings = 1
		var cable := _mesh_node(parent, root, "Cable%02d" % index, cylinder, cable_mat, Vector3(0.0, float(spec["y"]), float(spec["z"])), THIN_WIRE_LAYER)
		# The cylinder's axis is +Y: -90 degrees about Z lays it along +X, plus the tilt.
		cable.rotation_degrees = Vector3(0.0, 0.0, -90.0 + float(spec["tilt"]))
		cable.set_meta("against", String(spec["against"]))
		cable.set_meta("diameter_mm", snappedf(diameter * 1000.0, 0.001))


## The diameter that projects to the cable's width_px at its center, from the static camera at
## 1440p internal. It is snapped to 1 µm, so a last-bit difference in a platform's trigonometry
## cannot change the generated scene.
static func cable_diameter(spec: Dictionary) -> float:
	var center := Vector3(0.0, float(spec["y"]), float(spec["z"]))
	var per_meter := projected_width_px(center, cable_axis(spec), 1.0, GT_INTERNAL_SIZE)
	return snappedf(float(spec["width_px"]) / per_meter, 0.000001)


static func cable_axis(spec: Dictionary) -> Vector3:
	var tilt := deg_to_rad(float(spec["tilt"]))
	return Vector3(cos(tilt), sin(tilt), 0.0)


static func static_camera_transform() -> Transform3D:
	return Transform3D(Basis(), STATIC_CAMERA_POSITION).looking_at(CAMERA_TARGET, Vector3.UP)


## Pixel position and view depth of [param world] from the static camera at [param size]
## (vertical field of view: Camera3D keeps the height). A depth <= 0 is behind the camera.
static func project_static(world: Vector3, size: Vector2i) -> Vector3:
	var view := static_camera_transform().affine_inverse() * world
	var depth := -view.z
	if depth <= 0.0:
		return Vector3(0.0, 0.0, depth)
	var fy := 1.0 / tan(deg_to_rad(CAMERA_FOV) * 0.5)
	var fx := fy * float(size.y) / float(size.x)
	return Vector3((fx * view.x / depth + 1.0) * 0.5 * float(size.x), (1.0 - fy * view.y / depth) * 0.5 * float(size.y), depth)


## Screen width of a cylinder of [param diameter] around [param point] with axis [param axis]:
## its silhouette points lie half a diameter either side, perpendicular to the axis and to the
## view ray.
static func projected_width_px(point: Vector3, axis: Vector3, diameter: float, size: Vector2i) -> float:
	var side := axis.cross(point - STATIC_CAMERA_POSITION).normalized()
	var a := project_static(point + side * diameter * 0.5, size)
	var b := project_static(point - side * diameter * 0.5, size)
	return Vector2(a.x - b.x, a.y - b.y).length()
