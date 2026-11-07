extends SceneTree

const Scaffold := preload("res://generators/scaffold.gd")
const SEED := 0x6E657874
const SCENE_PATH := "res://scenes/upscaler_torture/upscaler_torture.tscn"
const ALPHA_PARTICLES_LAYER := 1 << 19
const SCROLLING_EMISSIVE_LAYER := 1 << 20
const ALL_TORTURE_LAYERS := 0xFFFFF | SCROLLING_EMISSIVE_LAYER


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
	shader.code = "shader_type spatial;\nrender_mode unshaded, cull_disabled;\nuniform sampler2D stripes : source_color, filter_linear, repeat_enable;\nuniform float time_offset = 0.0;\nvoid fragment() {\n\tvec3 color = texture(stripes, UV + vec2(TIME * 2.0 + time_offset, 0.0)).rgb;\n\tALBEDO = vec3(0.0);\n\tEMISSION = color * 3.0;\n}\n"
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
