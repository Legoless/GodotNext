extends Node3D

## Virtual Shadow Map non-Nanite per-page GPU instance culling and indirect raster (#218; plan §21.3 contract S4).
##
## Exercises every non-NextGeo shadow caster class into VSM pages across directional clipmap, spot,
## omni and area lights:
## 1. Regular static MeshInstance3D (including shared-material instancing into merged indirect-count
##    draw groups, reverse_cull_face light, and double-sided shadow surface).
## 2. Skeleton3D skinned meshes (both a 2-bone posed plate verifying dynamic layer 1 depth shifts from
##    w = 6.5 m to w = 3.5 m when Bone 1 poses, and the 12-bone 10k-triangle Character walker).
## 3. Blend-shape MeshInstance3D (verifying dynamic layer 1 depth shifts from w = 6.5 m to w = 5.0 m
##    when the blend-shape weight changes from 0.0 to 1.0).
## 4. MultiMeshInstance3D (both a 10,000-instance 3D MultiMesh with color/custom_data and a 64-instance
##    2D-transform MultiMesh).
## 5. GPUParticles3D (5,000 particles with partial emission so inactive -INF slots and active particles
##    are both culled on the GPU against the CPU sub_instance_box model).
## 6. Alpha-scissor cutout card (checkerboard cutout at w = 5.0 m over Ground at w = 8.0 m, proving
##    discard leaves w = 8.0 m holes while opaque squares write w = 5.0 m via R32_UINT imageAtomicMax)
##    and an alpha-hash card.
## 7. VERTEX-animated ShaderMaterial mesh (displacing VERTEX with TIME and a uniform).
## 8. Sub-texel distant speck caster (proving conservative small-caster culling increments gpu_small_culled).
## 9. Billboards and custom boxes: GPUParticles3D quads with Particle Billboard under ProbeSpot next to a
##    flat quad of the same size (facing each light, they draw as many fragments as the flat quad into
##    ProbeSpot's pages and 1 / |direction.y| times as many into the Sun's), a MultiMesh and a MeshInstance3D
##    of camera-facing quads (dynamic layer, and cull boxes that hold the turned quads), and a MultiMesh
##    whose VERTEX shader lifts the mesh inside its Mesh.custom_aabb. The engine counts every vertex outside
##    the box that culled its pair.
##
## Before each capture every shadowed light's shadow is off for one frame, so every page the view
## requests is new and dirty in the captured frame. The engine check (RenderingServer.
## virtual_shadow_maps_get_raster_check_result()) compares the GPU's instance-page pairs with its CPU
## model, counts each pair's fragments and checks that texels land only in pages a pair drew into.
## This script adds the class checks: every class emits pairs and writes texels, no caster has pairs
## without texels, the four shared-material boxes share their draw groups, the layers and the depth
## probes above.
##
## With --gallery=true, loads res://scenes/shadow_gallery/shadow_gallery.tscn, adds a blend-shape plate
## (the gallery has none) and verifies cam_casters and cam_main: the issue's six classes (skinned, blend
## shape, MultiMesh, particles, foliage, VERTEX-animated) emit pairs and write texels, every shadowed
## light with maps has a caster list, and 0 missing_from_vsm in the automated legacy render_shadows vs
## VSM caster diff, light by light.
## With --expect-active=false (GODOT_DEV_VSM unset), verifies VSM stays completely inactive.
## With --overflow=true (run with GODOT_DEV_VSM_PAIR_CAPACITY set small), keeps the shadows off for the
## first frames, turns them on and captures every frame from then until a capture drops no pair: the first
## must overflow, each must mark the pages of the pairs it dropped, each next frame must draw those pages
## again, and the last must match the CPU model.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const SceneArgs = preload("res://scenes/scene_args.gd")
const Character = preload("res://scenes/character_walk/character.gd")

const RESULT_WAIT_FRAMES := 10
const BILLBOARD_PARTICLE_COUNT := 4
const BILLBOARD_QUAD_SIZE := 0.3
const MAX_OVERFLOW_CAPTURES := 30

var _expect_active := false
var _gallery_mode := false
var _overflow_mode := false
var _overflow_captures: Array = []
var _frame := 0
var _stage := 0
var _stage_frame := 0
var _armed_frame := -1
var _armed_name := ""

var _camera: Camera3D
var _probe_spot: SpotLight3D
var _skinned_skeleton: Skeleton3D
var _skinned_plate: MeshInstance3D
var _walker_mesh: MeshInstance3D
var _blend_plate: MeshInstance3D
var _caster_nodes := {} # label -> VisualInstance3D
var _static_boxes: Array[MeshInstance3D] = [] # Same mesh and material: one draw group per cull variant.
var _shadowed_lights: Array[Light3D] = []

var _gallery_root: Node3D
var _gallery_cameras: Array[Camera3D] = []
var _gallery_casters := {} # label -> VisualInstance3D

var _captures: Array = []
var _failures := PackedStringArray()
var _result := {}


func _ready() -> void:
	_expect_active = SceneArgs.value("--expect-active", "true" if OS.get_environment("GODOT_DEV_VSM") != "" else "false") == "true"
	_gallery_mode = SceneArgs.value("--gallery", "false") == "true"
	_overflow_mode = SceneArgs.value("--overflow", "false") == "true"
	_result["expect_active"] = _expect_active
	_result["gallery"] = _gallery_mode
	_result["overflow"] = _overflow_mode
	_result["driver"] = RenderingServer.get_current_rendering_driver_name()
	_result["device"] = RenderingServer.get_video_adapter_name()
	if _gallery_mode:
		_build_gallery_scene()
	else:
		_build_controlled_scene()
		if _overflow_mode:
			# The shadows stay off while the scene uploads and settles: no pair is drawn, so no read-back grows
			# the list before the first capture. (A raster check's copy of a MultiMesh buffer uploaded in the
			# same frame tripped Vulkan synchronization validation on Windows.)
			_set_shadows(false)


func _build_controlled_scene() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.18, 0.22, 0.28)
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	# Ground receiver at y = 0 (w = 8.0 m under ProbeSpot at y = 8.0 m).
	var ground_mat := StandardMaterial3D.new()
	ground_mat.albedo_color = Color(0.75, 0.75, 0.72)
	var ground := MeshInstance3D.new()
	ground.name = "Ground"
	var ground_plane := PlaneMesh.new()
	ground_plane.size = Vector2(40.0, 40.0)
	ground_plane.material = ground_mat
	ground.mesh = ground_plane
	add_child(ground)
	_caster_nodes["ground"] = ground

	# Lights: directional sun, overhead ProbeSpot, reverse-cull SpotLight3D, OmniLight3D, AreaLight3D.
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.position = Vector3(0.0, 20.0, 0.0)
	sun.rotation_degrees = Vector3(-65.0, 30.0, 0.0)
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 120.0
	add_child(sun)

	_probe_spot = SpotLight3D.new()
	_probe_spot.name = "ProbeSpot"
	_probe_spot.position = Vector3(0.0, 8.0, 0.0)
	_probe_spot.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	_probe_spot.spot_range = 16.0
	_probe_spot.spot_angle = 22.0
	_probe_spot.light_energy = 2.0
	_probe_spot.shadow_enabled = true
	add_child(_probe_spot)

	var rev_spot := SpotLight3D.new()
	rev_spot.name = "ReverseCullSpot"
	rev_spot.position = Vector3(-6.0, 7.0, 5.0)
	rev_spot.look_at_from_position(Vector3(-6.0, 7.0, 5.0), Vector3(-4.0, 0.5, 0.0), Vector3.UP)
	rev_spot.spot_range = 18.0
	rev_spot.spot_angle = 35.0
	rev_spot.shadow_enabled = true
	rev_spot.shadow_reverse_cull_face = true
	add_child(rev_spot)

	var omni := OmniLight3D.new()
	omni.name = "Omni"
	omni.position = Vector3(4.5, 3.5, 1.0)
	omni.omni_range = 12.0
	omni.shadow_enabled = true
	add_child(omni)

	var area := AreaLight3D.new()
	area.name = "Area"
	area.area_size = Vector2(1.5, 1.0)
	area.area_range = 14.0
	area.shadow_enabled = true
	area.look_at_from_position(Vector3(0.0, 6.0, -5.5), Vector3(0.0, 0.5, -2.5), Vector3.UP)
	add_child(area)

	# 1. Shared-material regular static boxes + double-sided shadow quad.
	var shared_box_mesh := BoxMesh.new()
	shared_box_mesh.size = Vector3(0.8, 1.4, 0.8)
	shared_box_mesh.material = ground_mat
	for i in 4:
		var b := MeshInstance3D.new()
		b.name = "StaticBox%d" % i
		b.mesh = shared_box_mesh
		b.position = Vector3(-5.0 + float(i) * 0.9, 0.7, -2.5)
		add_child(b)
		_static_boxes.append(b)
		if i == 0:
			_caster_nodes["static_box"] = b

	var double_sided_mat := StandardMaterial3D.new()
	double_sided_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	double_sided_mat.albedo_color = Color(0.6, 0.5, 0.4)
	var double_quad := MeshInstance3D.new()
	double_quad.name = "DoubleSidedQuad"
	var dq_mesh := QuadMesh.new()
	dq_mesh.size = Vector2(1.2, 1.2)
	dq_mesh.material = double_sided_mat
	double_quad.mesh = dq_mesh
	double_quad.position = Vector3(-4.5, 1.2, 1.2)
	add_child(double_quad)
	_caster_nodes["double_sided"] = double_quad

	# 2a. Alpha-scissor checkerboard cutout card at y = 3.0 (w = 5.0 m under ProbeSpot).
	var cutout_img := Image.create_empty(64, 64, false, Image.FORMAT_RGBA8)
	for py in 64:
		for px in 64:
			# 8 x 8 checkerboard: 0.20 m squares on a 1.6 m card (~220 mip-0 virtual texels per square).
			var opaque := (((px / 8) + (py / 8)) % 2) == 0
			cutout_img.set_pixel(px, py, Color(0.2, 0.65, 0.25, 1.0 if opaque else 0.0))
	var cutout_tex := ImageTexture.create_from_image(cutout_img)
	var cutout_mat := StandardMaterial3D.new()
	cutout_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	cutout_mat.alpha_scissor_threshold = 0.5
	cutout_mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	cutout_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	cutout_mat.albedo_texture = cutout_tex
	var cutout_card := MeshInstance3D.new()
	cutout_card.name = "AlphaScissorCard"
	var cutout_plane := PlaneMesh.new()
	cutout_plane.size = Vector2(1.6, 1.6)
	cutout_plane.material = cutout_mat
	cutout_card.mesh = cutout_plane
	cutout_card.position = Vector3(-0.95, 3.0, -0.2)
	add_child(cutout_card)
	_caster_nodes["alpha_scissor"] = cutout_card

	# 2b. Alpha-hash card outside ProbeSpot's inner cone (lit by Sun, Omni and Area).
	var hash_img := Image.create_empty(32, 32, false, Image.FORMAT_RGBA8)
	for py in 32:
		for px in 32:
			hash_img.set_pixel(px, py, Color(0.7, 0.35, 0.2, 0.85 if ((px + py) % 2 == 0) else 0.05))
	var hash_mat := StandardMaterial3D.new()
	hash_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_HASH
	hash_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	hash_mat.albedo_texture = ImageTexture.create_from_image(hash_img)
	var hash_card := MeshInstance3D.new()
	hash_card.name = "AlphaHashCard"
	var hash_plane := PlaneMesh.new()
	hash_plane.size = Vector2(1.4, 1.4)
	hash_plane.material = hash_mat
	hash_card.mesh = hash_plane
	hash_card.position = Vector3(3.8, 1.6, 2.2)
	add_child(hash_card)
	_caster_nodes["alpha_hash"] = hash_card

	# 3. Blend-shape plate inside ProbeSpot: rest at y = 1.5 (w = 6.5 m), blend weight 1.0 -> y = 3.0 (w = 5.0 m).
	_blend_plate = _make_blend_plate()
	_blend_plate.position = Vector3(0.85, 1.5, -0.75)
	add_child(_blend_plate)
	_caster_nodes["blend_shape"] = _blend_plate

	# 4a. 2-bone Skeleton3D skinned plate inside ProbeSpot: rest at y = 1.5 (w = 6.5 m), posed -> y = 4.5 (w = 3.5 m).
	var skel_root := _make_skinned_plate()
	skel_root.position = Vector3(0.85, 1.5, 0.75)
	add_child(skel_root)
	_caster_nodes["skinned_plate"] = _skinned_plate

	# 4b. 12-bone 10k-triangle Character walker (outside ProbeSpot cone, lit by Sun and Omni).
	var walker := Character.make_character("SkinnedWalker")
	walker.position = Vector3(4.2, 0.0, -1.5)
	add_child(walker)
	_walker_mesh = _find_first_mesh_instance(walker)
	if _walker_mesh != null:
		_caster_nodes["skinned_walker"] = _walker_mesh

	# 5a. 10,000-instance 3D MultiMeshInstance3D (with color and custom_data).
	var mm3d := MultiMesh.new()
	mm3d.transform_format = MultiMesh.TRANSFORM_3D
	mm3d.use_colors = true
	mm3d.use_custom_data = true
	var mm_box := BoxMesh.new()
	mm_box.size = Vector3(0.08, 0.22, 0.08)
	mm_box.material = ground_mat
	mm3d.mesh = mm_box
	mm3d.instance_count = 10000
	for idx in 10000:
		var gx := idx % 100
		var gz := idx / 100
		var t := Transform3D(Basis(), Vector3(-5.8 + float(gx) * 0.032, 0.15, 1.8 + float(gz) * 0.032))
		mm3d.set_instance_transform(idx, t)
		mm3d.set_instance_color(idx, Color(0.8, 0.85, 0.9, 1.0))
		mm3d.set_instance_custom_data(idx, Color(float(idx) / 10000.0, 0.0, 0.0, 1.0))
	var mmi3d := MultiMeshInstance3D.new()
	mmi3d.name = "MultiMesh3D_10k"
	mmi3d.multimesh = mm3d
	add_child(mmi3d)
	_caster_nodes["multimesh_3d"] = mmi3d

	# 5b. 64-instance 2D-transform MultiMeshInstance3D.
	var mm2d := MultiMesh.new()
	mm2d.transform_format = MultiMesh.TRANSFORM_2D
	var mm2d_box := BoxMesh.new()
	mm2d_box.size = Vector3(0.12, 0.12, 0.25)
	mm2d_box.material = ground_mat
	mm2d.mesh = mm2d_box
	mm2d.instance_count = 64
	for idx in 64:
		var t2 := Transform2D(0.0, Vector2(-2.2 + float(idx % 8) * 0.2, 0.3 + float(idx / 8) * 0.2))
		mm2d.set_instance_transform_2d(idx, t2)
	var mmi2d := MultiMeshInstance3D.new()
	mmi2d.name = "MultiMesh2D_64"
	mmi2d.multimesh = mm2d
	mmi2d.position = Vector3(0.0, 0.0, -3.5)
	add_child(mmi2d)
	_caster_nodes["multimesh_2d"] = mmi2d

	# 6. GPUParticles3D (5,000 particles with amount_ratio = 0.6 so active and -INF inactive slots coexist).
	var particles := GPUParticles3D.new()
	particles.name = "Particles5k"
	particles.position = Vector3(4.5, 1.5, 0.5)
	particles.amount = 5000
	particles.amount_ratio = 0.6
	particles.lifetime = 4.0
	particles.explosiveness = 0.85
	particles.preprocess = 0.5
	var pmat := ParticleProcessMaterial.new()
	pmat.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_BOX
	pmat.emission_box_extents = Vector3(1.2, 0.8, 1.2)
	pmat.gravity = Vector3.ZERO
	pmat.initial_velocity_min = 0.05
	pmat.initial_velocity_max = 0.2
	particles.process_material = pmat
	var pbox := BoxMesh.new()
	pbox.size = Vector3(0.09, 0.09, 0.09)
	pbox.material = ground_mat
	particles.draw_pass_1 = pbox
	add_child(particles)
	_caster_nodes["particles"] = particles

	# 7. VERTEX-animated ShaderMaterial mesh (outside ProbeSpot cone, lit by Sun and Area).
	var wave_shader := Shader.new()
	wave_shader.code = """shader_type spatial;
render_mode cull_disabled;
uniform float lift = 0.2;
void vertex() {
	VERTEX.y += lift + sin(TIME * 3.0 + VERTEX.x * 4.0) * 0.15;
}
void fragment() {
	ALBEDO = vec3(0.65, 0.45, 0.25);
}
"""
	var wave_mat := ShaderMaterial.new()
	wave_mat.shader = wave_shader
	var wave_mesh := MeshInstance3D.new()
	wave_mesh.name = "VertexAnimatedWave"
	var wave_plane := PlaneMesh.new()
	wave_plane.size = Vector2(2.0, 2.0)
	wave_plane.subdivide_width = 8
	wave_plane.subdivide_depth = 8
	wave_plane.material = wave_mat
	wave_mesh.mesh = wave_plane
	# Outside ProbeSpot's square frustum (half-width 0.40 w), so the spot's depth probes see only the plates.
	wave_mesh.position = Vector3(0.0, 1.4, -5.2)
	wave_mesh.extra_cull_margin = 0.6
	add_child(wave_mesh)
	_caster_nodes["vertex_wave"] = wave_mesh

	# 8. Sub-texel tiny speck box (0.001 m extent -> culled on coarse/spot pages by DEFAULT_MIN_CASTER_TEXELS).
	var speck := MeshInstance3D.new()
	speck.name = "TinySpeck"
	var speck_mesh := BoxMesh.new()
	speck_mesh.size = Vector3(0.001, 0.001, 0.001)
	speck_mesh.material = ground_mat
	speck.mesh = speck_mesh
	speck.position = Vector3(0.1, 0.2, 0.1)
	add_child(speck)

	# 9a. Particle Billboard quads low under ProbeSpot (w ~ 7.55 m, below the plates' 6.5 m probe): four quads at
	# one point, and a flat quad of the same size 1 cm below them. Facing ProbeSpot's camera, each particle
	# quad lies flat like the reference and draws as many fragments into the same pages; a quad that faced
	# world +Z instead would be seen nearly edge-on from the spot.
	var bb_particles := _make_billboard_particles(BILLBOARD_PARTICLE_COUNT, 0.0)
	bb_particles.position = Vector3(-1.2, 0.45, 1.2)
	add_child(bb_particles)
	_caster_nodes["billboard_particles"] = bb_particles
	var flat_quad := MeshInstance3D.new()
	flat_quad.name = "FlatReferenceQuad"
	var flat_mesh := QuadMesh.new()
	flat_mesh.size = Vector2(BILLBOARD_QUAD_SIZE, BILLBOARD_QUAD_SIZE)
	flat_mesh.orientation = PlaneMesh.FACE_Y
	var flat_mat := StandardMaterial3D.new()
	flat_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	flat_mesh.material = flat_mat
	flat_quad.mesh = flat_mesh
	flat_quad.position = Vector3(-1.2, 0.44, 1.2)
	add_child(flat_quad)
	_caster_nodes["flat_reference_quad"] = flat_quad

	# 9b. 36 camera-facing quads (Billboard Enabled): their cull boxes must hold the quads as the camera turns them.
	var bbm := _make_billboard_multimesh(6, Vector3(1.45, 0.55, -2.55))
	add_child(bbm)
	_caster_nodes["billboard_multimesh"] = bbm
	# 9b'. One camera-facing quad on a MeshInstance3D (Keep Scale, scaled 1.5): its box record is the mesh's
	# flat box through the transform, so its cull box must add the turned quad.
	var bb_quad_mesh := QuadMesh.new()
	bb_quad_mesh.size = Vector2(0.4, 0.4)
	var bb_quad_mat := StandardMaterial3D.new()
	bb_quad_mat.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	bb_quad_mat.billboard_keep_scale = true
	bb_quad_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	bb_quad_mat.albedo_color = Color(0.3, 0.55, 0.75)
	bb_quad_mesh.material = bb_quad_mat
	var bb_quad := MeshInstance3D.new()
	bb_quad.name = "BillboardMeshInstance"
	bb_quad.mesh = bb_quad_mesh
	bb_quad.position = Vector3(2.6, 0.7, -1.0)
	bb_quad.scale = Vector3(1.5, 1.5, 1.5)
	add_child(bb_quad)
	_caster_nodes["billboard_mesh"] = bb_quad

	# 9c. A MultiMesh whose VERTEX shader lifts each 0.1 m box by 0.45 m, inside its Mesh.custom_aabb.
	var lift_shader := Shader.new()
	lift_shader.code = """shader_type spatial;
render_mode cull_disabled;
void vertex() {
	VERTEX.y += 0.45;
}
void fragment() {
	ALBEDO = vec3(0.4, 0.6, 0.5);
}
"""
	var lift_mat := ShaderMaterial.new()
	lift_mat.shader = lift_shader
	var lift_box := BoxMesh.new()
	lift_box.size = Vector3(0.1, 0.1, 0.1)
	lift_box.material = lift_mat
	lift_box.custom_aabb = AABB(Vector3(-0.05, -0.05, -0.05), Vector3(0.1, 0.55, 0.1))
	var lift_mm := MultiMesh.new()
	lift_mm.transform_format = MultiMesh.TRANSFORM_3D
	lift_mm.mesh = lift_box
	lift_mm.instance_count = 16
	for idx in 16:
		lift_mm.set_instance_transform(idx, Transform3D(Basis(), Vector3(-0.55 + float(idx % 4) * 0.15, 0.05, 2.25 + float(idx / 4) * 0.15)))
	var lift_mmi := MultiMeshInstance3D.new()
	lift_mmi.name = "CustomAabbMultiMesh"
	lift_mmi.multimesh = lift_mm
	add_child(lift_mmi)
	_caster_nodes["custom_aabb_multimesh"] = lift_mmi

	_camera = Camera3D.new()
	_camera.current = true
	_camera.fov = 65.0
	_camera.far = 120.0
	_camera.look_at_from_position(Vector3(0.0, 5.8, 6.2), Vector3(0.0, 0.8, -0.4), Vector3.UP)
	add_child(_camera)


# Particle Billboard quads that do not move: p_amount of them in a box of half-size p_spread (0: one point).
func _make_billboard_particles(p_amount: int, p_spread: float) -> GPUParticles3D:
	var particles := GPUParticles3D.new()
	particles.name = "BillboardParticles"
	particles.amount = p_amount
	particles.lifetime = 600.0
	particles.explosiveness = 1.0
	particles.preprocess = 0.2
	particles.visibility_aabb = AABB(Vector3(-1.0, -1.0, -1.0), Vector3(2.0, 2.0, 2.0))
	var pmat := ParticleProcessMaterial.new()
	if p_spread > 0.0:
		pmat.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_BOX
		pmat.emission_box_extents = Vector3(p_spread, 0.02, p_spread)
	else:
		pmat.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_POINT
	pmat.gravity = Vector3.ZERO
	pmat.initial_velocity_min = 0.0
	pmat.initial_velocity_max = 0.0
	particles.process_material = pmat
	var quad := QuadMesh.new()
	quad.size = Vector2(BILLBOARD_QUAD_SIZE, BILLBOARD_QUAD_SIZE)
	var mat := StandardMaterial3D.new()
	mat.billboard_mode = BaseMaterial3D.BILLBOARD_PARTICLES
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.albedo_color = Color(0.9, 0.75, 0.3)
	quad.material = mat
	particles.draw_pass_1 = quad
	return particles


func _make_billboard_multimesh(p_side: int, p_corner: Vector3) -> MultiMeshInstance3D:
	var quad := QuadMesh.new()
	quad.size = Vector2(0.25, 0.25)
	var mat := StandardMaterial3D.new()
	mat.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.albedo_color = Color(0.35, 0.7, 0.35)
	quad.material = mat
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = quad
	mm.instance_count = p_side * p_side
	for idx in p_side * p_side:
		mm.set_instance_transform(idx, Transform3D(Basis(), p_corner + Vector3(float(idx % p_side) * 0.25, 0.0, float(idx / p_side) * 0.25)))
	var mmi := MultiMeshInstance3D.new()
	mmi.name = "BillboardMultiMesh"
	mmi.multimesh = mm
	return mmi


func _make_blend_plate() -> MeshInstance3D:
	var verts := PackedVector3Array([
		Vector3(-0.4, 0.0, -0.4),
		Vector3(0.4, 0.0, -0.4),
		Vector3(0.4, 0.0, 0.4),
		Vector3(-0.4, 0.0, 0.4),
	])
	var norms := PackedVector3Array([Vector3.UP, Vector3.UP, Vector3.UP, Vector3.UP])
	var indices := PackedInt32Array([0, 1, 2, 0, 2, 3])
	var base_arrays := []
	base_arrays.resize(Mesh.ARRAY_MAX)
	base_arrays[Mesh.ARRAY_VERTEX] = verts
	base_arrays[Mesh.ARRAY_NORMAL] = norms
	base_arrays[Mesh.ARRAY_INDEX] = indices

	# Normalized blend shape: delta +1.5 m in Y (so weight 1.0 shifts y from 1.5 to 3.0 -> w = 5.0 m).
	var blend_verts := PackedVector3Array([
		Vector3(-0.4, 1.5, -0.4),
		Vector3(0.4, 1.5, -0.4),
		Vector3(0.4, 1.5, 0.4),
		Vector3(-0.4, 1.5, 0.4),
	])
	var blend_arrays := []
	blend_arrays.resize(Mesh.ARRAY_MAX)
	blend_arrays[Mesh.ARRAY_VERTEX] = blend_verts
	blend_arrays[Mesh.ARRAY_NORMAL] = norms

	var mesh := ArrayMesh.new()
	mesh.add_blend_shape("Lift")
	mesh.blend_shape_mode = Mesh.BLEND_SHAPE_MODE_NORMALIZED
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, base_arrays, [blend_arrays])
	var mat := StandardMaterial3D.new()
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.albedo_color = Color(0.8, 0.55, 0.7)
	mesh.surface_set_material(0, mat)

	var mi := MeshInstance3D.new()
	mi.name = "BlendShapePlate"
	mi.mesh = mesh
	mi.extra_cull_margin = 2.5
	mi.set_blend_shape_value(0, 0.0)
	return mi


func _make_skinned_plate() -> Node3D:
	var root := Node3D.new()
	root.name = "SkinnedPlateRig"
	_skinned_skeleton = Skeleton3D.new()
	_skinned_skeleton.name = "Skeleton3D"
	_skinned_skeleton.add_bone("Root")
	_skinned_skeleton.set_bone_rest(0, Transform3D())
	_skinned_skeleton.add_bone("Mover")
	_skinned_skeleton.set_bone_parent(1, 0)
	_skinned_skeleton.set_bone_rest(1, Transform3D())
	_skinned_skeleton.reset_bone_poses()
	root.add_child(_skinned_skeleton)

	var skin := Skin.new()
	skin.add_bind(0, Transform3D())
	skin.add_bind(1, Transform3D())

	var verts := PackedVector3Array([
		Vector3(-0.4, 0.0, -0.4),
		Vector3(0.4, 0.0, -0.4),
		Vector3(0.4, 0.0, 0.4),
		Vector3(-0.4, 0.0, 0.4),
	])
	var norms := PackedVector3Array([Vector3.UP, Vector3.UP, Vector3.UP, Vector3.UP])
	var bones := PackedInt32Array([
		1, 0, 0, 0,
		1, 0, 0, 0,
		1, 0, 0, 0,
		1, 0, 0, 0,
	])
	var weights := PackedFloat32Array([
		1.0, 0.0, 0.0, 0.0,
		1.0, 0.0, 0.0, 0.0,
		1.0, 0.0, 0.0, 0.0,
		1.0, 0.0, 0.0, 0.0,
	])
	var indices := PackedInt32Array([0, 1, 2, 0, 2, 3])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = norms
	arrays[Mesh.ARRAY_BONES] = bones
	arrays[Mesh.ARRAY_WEIGHTS] = weights
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var mat := StandardMaterial3D.new()
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.albedo_color = Color(0.45, 0.65, 0.85)
	mesh.surface_set_material(0, mat)

	_skinned_plate = MeshInstance3D.new()
	_skinned_plate.name = "SkinnedPlateMesh"
	_skinned_plate.mesh = mesh
	_skinned_plate.skin = skin
	_skinned_plate.skeleton = NodePath("..")
	_skinned_plate.extra_cull_margin = 4.0
	_skinned_skeleton.add_child(_skinned_plate)
	return root


func _build_gallery_scene() -> void:
	var packed := load("res://scenes/shadow_gallery/shadow_gallery.tscn") as PackedScene
	if packed == null:
		_failures.append("could not load shadow_gallery.tscn")
		return
	_gallery_root = packed.instantiate() as Node3D
	_gallery_root.set_meta("skip_runtime", true)
	add_child(_gallery_root)

	var gs := _gallery_root.get_node_or_null("GallerySkinned") as Node3D
	if gs != null:
		if gs.get_child_count() == 0:
			var walker := Character.make_character("SkinnedWalker")
			gs.add_child(walker)
		var wmesh := _find_first_mesh_instance(gs)
		if wmesh != null:
			_gallery_casters["gallery_skinned"] = wmesh
	for pair in [
		["caster_multimesh", "CasterMultiMesh"],
		["caster_particles", "CasterParticles"],
		["caster_foliage", "CasterFoliage"],
		["caster_vertex_animated", "CasterVertexAnimated"],
	]:
		var node := _gallery_root.get_node_or_null(pair[1]) as VisualInstance3D
		if node != null:
			_gallery_casters[pair[0]] = node
			# The gallery's wave is a single-sided plane whose front faces away from both suns: with
			# back faces culled (as in the shadow atlas) its pairs draw nothing. Double-sided shadows let
			# it cast from every light, so the no-pairs-without-texels rule below applies to it too.
			if pair[1] == "CasterVertexAnimated":
				(node as GeometryInstance3D).cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_DOUBLE_SIDED
				# Its VERTEX code lifts the flat plane by up to 0.12 m, outside the plane's box, and the gallery
				# declares no margin: the cull box would not hold the wave (the check counts such vertices).
				(node as GeometryInstance3D).extra_cull_margin = 0.15
		else:
			_failures.append("missing %s in ShadowGallery" % pair[1])
	# ShadowGallery has no blend-shape caster; the issue's class row has one. Add the controlled scene's
	# plate on the floor in view of cam_casters, half lifted, so the gallery covers every class.
	var gallery_blend := _make_blend_plate()
	gallery_blend.name = "GalleryBlendShape"
	gallery_blend.position = Vector3(3.0, 0.6, 5.0)
	gallery_blend.set_blend_shape_value(0, 0.5)
	_gallery_root.add_child(gallery_blend)
	_gallery_casters["gallery_blend_shape"] = gallery_blend
	# Billboards next to it: Particle Billboard quads and a MultiMesh of camera-facing quads.
	var gallery_bb_particles := _make_billboard_particles(48, 0.45)
	gallery_bb_particles.name = "GalleryBillboardParticles"
	gallery_bb_particles.position = Vector3(2.0, 0.8, 5.0)
	_gallery_root.add_child(gallery_bb_particles)
	_gallery_casters["gallery_billboard_particles"] = gallery_bb_particles
	var gallery_bbm := _make_billboard_multimesh(4, Vector3(3.8, 0.8, 4.6))
	gallery_bbm.name = "GalleryBillboardMultiMesh"
	_gallery_root.add_child(gallery_bbm)
	_gallery_casters["gallery_billboard_multimesh"] = gallery_bbm

	for cam_name in ["cam_casters", "cam_main"]:
		var cam := _gallery_root.get_node_or_null(cam_name) as Camera3D
		if cam != null:
			cam.current = false
			_gallery_cameras.append(cam)
		else:
			_failures.append("missing camera %s in ShadowGallery" % cam_name)
	if not _gallery_cameras.is_empty():
		_gallery_cameras[0].current = true


func _find_first_mesh_instance(p_root: Node) -> MeshInstance3D:
	if p_root is MeshInstance3D:
		return p_root as MeshInstance3D
	for child in p_root.get_children():
		var found := _find_first_mesh_instance(child)
		if found != null:
			return found
	return null


func _process(_delta: float) -> void:
	_frame += 1
	if not _expect_active:
		if _frame == 2:
			RenderingServer.virtual_shadow_maps_request_raster_check()
		elif _frame >= 5:
			var info := RenderingServer.virtual_shadow_maps_get_debug_info()
			var rcheck := RenderingServer.virtual_shadow_maps_get_raster_check_result()
			_result["info"] = info
			_result["raster_check"] = rcheck
			if bool(info.get("active", true)):
				_failures.append("GODOT_DEV_VSM is unset but VSM is active")
			if bool(rcheck.get("captured", true)):
				_failures.append("GODOT_DEV_VSM is unset but VSM raster check captured a view")
			_finish()
		return

	if _armed_frame >= 0:
		var res := RenderingServer.virtual_shadow_maps_get_raster_check_result()
		if bool(res.get("captured", false)) and _overflow_mode:
			_evaluate_overflow_capture(res)
			return
		if bool(res.get("captured", false)):
			_evaluate_capture(_armed_name, res)
			_armed_frame = -1
			_stage += 1
			_stage_frame = 0
			return
		if _frame - _armed_frame > RESULT_WAIT_FRAMES:
			_failures.append("timed out waiting for raster check '%s'" % _armed_name)
			_finish()
			return
		return

	_stage_frame += 1
	if _gallery_mode:
		_step_gallery()
	else:
		_step_controlled()


func _arm(p_name: String) -> void:
	RenderingServer.virtual_shadow_maps_request_raster_check()
	_armed_frame = _frame
	_armed_name = p_name


# Pages are rendered once and then cached (#219; #212 adds invalidation): a capture sees only the pages
# that are dirty in its frame. Turning every shadowed light's shadow off for one frame frees its maps,
# so in the captured frame every page that the view requests is new and dirty, for every light.
func _set_shadows(p_enabled: bool) -> void:
	if _shadowed_lights.is_empty():
		for node in find_children("*", "Light3D", true, false):
			var light := node as Light3D
			if light.shadow_enabled:
				_shadowed_lights.append(light)
	for light in _shadowed_lights:
		light.shadow_enabled = p_enabled


func _step_controlled() -> void:
	if _overflow_mode:
		# Turning the shadows on makes every page new: the small list overflows in that frame, before a
		# read-back grows it. Capture it, and then every frame (_evaluate_overflow_capture()).
		if _stage_frame == 6:
			_set_shadows(true)
			_arm("overflow_0")
		return
	match _stage:
		0:
			# Wait a couple of frames so GPUParticles3D emit and settle, then make every page dirty.
			if _stage_frame == 2:
				_set_shadows(false)
			elif _stage_frame == 3:
				_set_shadows(true)
				_arm("rest_pose")
		1:
			# Stage 1: set BlendShapePlate weight = 1.0 (+1.5 m -> y = 3.0 -> w = 5.0 m), with fresh pages.
			if _stage_frame == 1:
				_blend_plate.set_blend_shape_value(0, 1.0)
				_set_shadows(false)
			elif _stage_frame == 2:
				_set_shadows(true)
				_arm("blend_lifted")
		2:
			# Stage 2: pose Skeleton3D Bone 1 by +3.0 m in Y (y = 4.5 -> w = 3.5 m), with fresh pages.
			if _stage_frame == 1:
				_skinned_skeleton.set_bone_pose_position(1, Vector3(0.0, 3.0, 0.0))
				_set_shadows(false)
			elif _stage_frame == 2:
				_set_shadows(true)
				_arm("bone_posed")
		_:
			_result["info"] = RenderingServer.virtual_shadow_maps_get_debug_info()
			_result["captures"] = _captures
			_finish()


func _step_gallery() -> void:
	if _stage >= _gallery_cameras.size():
		_result["info"] = RenderingServer.virtual_shadow_maps_get_debug_info()
		_result["captures"] = _captures
		_finish()
		return
	if _stage_frame == 1:
		for i in _gallery_cameras.size():
			_gallery_cameras[i].current = (i == _stage)
	elif _stage_frame == 2:
		_set_shadows(false)
	elif _stage_frame == 3:
		_set_shadows(true)
		_arm(_gallery_cameras[_stage].name)


func _evaluate_overflow_capture(p_res: Dictionary) -> void:
	var n := _overflow_captures.size()
	var summary := {
		"pass": bool(p_res.get("pass", false)),
		"consecutive": bool(p_res.get("consecutive", false)),
		"pair_capacity": int(p_res.get("pair_capacity", 0)),
		"gpu_total_pairs": int(p_res.get("gpu_total_pairs", 0)),
		"gpu_dropped_pairs": int(p_res.get("gpu_dropped_pairs", -1)),
		"redraw_marks": int(p_res.get("redraw_marks", -1)),
		"pair_missing_marked": int(p_res.get("pair_missing_marked", -1)),
		"marked_pages": int(p_res.get("marked_pages", -1)),
		"previous_marked_pages": int(p_res.get("previous_marked_pages", -1)),
		"marked_not_redrawn": int(p_res.get("marked_not_redrawn", -1)),
		"dirty_page_count": int(p_res.get("dirty_page_count", 0)),
		"exact_match": bool(p_res.get("exact_match", false)),
		"exact_missing": int(p_res.get("exact_missing", -1)),
		"vertices_outside_box": int(p_res.get("vertices_outside_box", -1)),
	}
	_overflow_captures.append(summary)
	for f in p_res.get("failures", PackedStringArray()):
		_failures.append("overflow_%d: %s" % [n, f])
	if not bool(p_res.get("pass", false)):
		_failures.append("overflow_%d: engine raster check reported pass=false" % n)
	if n > 0 and not bool(p_res.get("consecutive", false)):
		_failures.append("overflow_%d: the capture was not the frame after the previous one" % n)
	var dropped := int(p_res.get("gpu_dropped_pairs", -1))
	if n == 0 and dropped <= 0:
		_failures.append("overflow_0: the pair list did not overflow (capacity %d, %d pairs)" % [int(p_res.get("pair_capacity", 0)), int(p_res.get("gpu_total_pairs", 0))])
	if dropped == 0 or n + 1 >= MAX_OVERFLOW_CAPTURES or not _failures.is_empty():
		# The engine's pass covers the CPU model comparison (pairs that 1e-5 cannot explain fail).
		if dropped != 0 and _failures.is_empty():
			_failures.append("the pair list still overflowed after %d captures" % (n + 1))
		_armed_frame = -1
		_result["overflow_captures"] = _overflow_captures
		_result["info"] = RenderingServer.virtual_shadow_maps_get_debug_info()
		_finish()
		return
	# The next frame: its marked pages from this one must be drawn again.
	_arm("overflow_%d" % (n + 1))


func _evaluate_capture(p_name: String, p_res: Dictionary) -> void:
	var summary := {
		"name": p_name,
		"pass": bool(p_res.get("pass", false)),
		"used_indirect_count": bool(p_res.get("used_indirect_count", false)),
		"light_count": int(p_res.get("light_count", 0)),
		"view_vsm_lights": int(p_res.get("view_vsm_lights", 0)),
		"lights_without_casters": int(p_res.get("lights_without_casters", -1)),
		"caster_count": int(p_res.get("caster_count", 0)),
		"surface_element_count": int(p_res.get("surface_element_count", 0)),
		"group_count": int(p_res.get("group_count", 0)),
		"box_record_count": int(p_res.get("box_record_count", 0)),
		"sub_record_count": int(p_res.get("sub_record_count", 0)),
		"dirty_page_count": int(p_res.get("dirty_page_count", 0)),
		"atlas_non_virtual_casters": int(p_res.get("atlas_non_virtual_casters", 0)),
		"virtual_skipped": int(p_res.get("virtual_skipped", 0)),
		"missing_from_vsm": int(p_res.get("missing_from_vsm", -1)),
		"vsm_extra_casters": int(p_res.get("vsm_extra_casters", 0)),
		"gpu_total_pairs": int(p_res.get("gpu_total_pairs", 0)),
		"cpu_total_pairs": int(p_res.get("cpu_total_pairs", 0)),
		"gpu_small_culled": int(p_res.get("gpu_small_culled", 0)),
		"cpu_small_culled": int(p_res.get("cpu_small_culled", 0)),
		"exact_match": bool(p_res.get("exact_match", false)),
		"exact_unexplained": int(p_res.get("exact_unexplained", -1)),
		"exact_missing": int(p_res.get("exact_missing", -1)),
		"exact_difference_samples": p_res.get("exact_difference_samples", []),
		"pair_unexpected": int(p_res.get("pair_unexpected", -1)),
		"pair_missing": int(p_res.get("pair_missing", -1)),
		"gpu_dropped_pairs": int(p_res.get("gpu_dropped_pairs", -1)),
		"pair_capacity": int(p_res.get("pair_capacity", 0)),
		"write_mismatch": int(p_res.get("write_mismatch", -1)),
		"vertices_outside_box": int(p_res.get("vertices_outside_box", -1)),
		"total_fragments": int(p_res.get("total_fragments", 0)),
		"pages_with_pairs": int(p_res.get("pages_with_pairs", 0)),
		"pages_with_texels": int(p_res.get("pages_with_texels", 0)),
		"texels_without_pair": int(p_res.get("texels_without_pair", -1)),
		"casters_without_texels": p_res.get("casters_without_texels", PackedInt64Array()).size(),
		"point_size_skipped": int(p_res.get("point_size_skipped", 0)),
		"nonzero_static_texels": int(p_res.get("nonzero_static_texels", 0)),
		"nonzero_dynamic_texels": int(p_res.get("nonzero_dynamic_texels", 0)),
	}
	if int(p_res.get("gpu_total_pairs", 0)) <= 0 or int(p_res.get("total_fragments", 0)) <= 0:
		_failures.append("%s: the view drew no pair (pairs=%d, fragments=%d)" % [p_name, int(p_res.get("gpu_total_pairs", 0)), int(p_res.get("total_fragments", 0))])
	for f in p_res.get("failures", PackedStringArray()):
		_failures.append("%s: %s" % [p_name, f])
	# Every caster of these scenes is closed or double-sided, so a caster whose pairs drew no fragment
	# missed the pages it touches.
	var outside_names := PackedStringArray()
	for object_id in p_res.get("casters_outside_box", PackedInt64Array()):
		var object := instance_from_id(object_id)
		outside_names.append(str((object as Node).get_path()) if object is Node else str(object_id))
	summary["casters_outside_box"] = outside_names
	var no_texels: PackedInt64Array = p_res.get("casters_without_texels", PackedInt64Array())
	if not no_texels.is_empty():
		_failures.append("%s: %d casters emitted pairs but wrote no texel: %s" % [p_name, no_texels.size(), str(no_texels)])
	if not bool(p_res.get("pass", false)):
		_failures.append("%s: engine raster check reported pass=false" % p_name)
	if not bool(p_res.get("used_indirect_count", false)):
		_failures.append("%s: draw_list_draw_indirect_count was not used" % p_name)
	if int(p_res.get("missing_from_vsm", 1)) != 0:
		_failures.append("%s: missing_from_vsm=%d" % [p_name, int(p_res.get("missing_from_vsm", 1))])
	# Without GODOT_DEV_VSM_PAIR_CAPACITY the list holds every pair; the overflow run tests the recovery.
	if int(p_res.get("gpu_dropped_pairs", 1)) != 0:
		_failures.append("%s: the pair list dropped %d pairs" % [p_name, int(p_res.get("gpu_dropped_pairs", 1))])
	if int(p_res.get("vertices_outside_box", 1)) != 0:
		_failures.append("%s: %d vertices lie outside the box that culled their pair" % [p_name, int(p_res.get("vertices_outside_box", 1))])

	var by_obj := {}
	for cdict in p_res.get("casters", []):
		by_obj[int(cdict.get("object_id", 0))] = cdict

	if _gallery_mode:
		var caster_texels := {}
		for label in _gallery_casters:
			var node: VisualInstance3D = _gallery_casters[label]
			var cinfo: Dictionary = by_obj.get(node.get_instance_id(), {})
			var texels := int(cinfo.get("texels_written", 0))
			caster_texels[label] = texels
			if p_name == "cam_casters":
				if cinfo.is_empty():
					_failures.append("%s: gallery caster '%s' missing from VSM casters" % [p_name, label])
				elif int(cinfo.get("pairs_emitted", 0)) <= 0 or texels <= 0:
					_failures.append("%s: gallery caster '%s' emitted %d pairs and wrote %d texels" % [p_name, label, int(cinfo.get("pairs_emitted", 0)), texels])
		summary["gallery_caster_texels"] = caster_texels
		# Every shadowed light of the view with virtual maps had a caster list (the engine also fails on a
		# light without one); VSM has no tile cap like the shadow atlas.
		if int(p_res.get("light_count", 0)) <= 0 or int(p_res.get("light_count", 0)) != int(p_res.get("view_vsm_lights", -1)):
			_failures.append("%s: %d VSM lights with caster lists, %d shadowed lights with maps in the view" % [p_name, int(p_res.get("light_count", 0)), int(p_res.get("view_vsm_lights", -1))])
		_captures.append(summary)
		return

	# Controlled scene checks:
	if p_name == "rest_pose":
		# The four boxes share mesh, material and pipeline: their pairs go to the same indirect draws.
		var box_groups: Array = []
		for box in _static_boxes:
			var binfo: Dictionary = by_obj.get(box.get_instance_id(), {})
			box_groups.append(Array(binfo.get("groups", PackedInt32Array())))
		summary["static_box_groups"] = box_groups
		if box_groups.is_empty() or (box_groups[0] as Array).is_empty():
			_failures.append("rest_pose: the static boxes drew no group")
		else:
			for groups in box_groups:
				if groups != box_groups[0]:
					_failures.append("rest_pose: the static boxes did not share their draw groups: %s" % str(box_groups))
					break
		if int(p_res.get("gpu_small_culled", 0)) <= 0:
			_failures.append("rest_pose: expected gpu_small_culled > 0 from sub-texel TinySpeck")
		var per_class := {}
		for label in _caster_nodes:
			var node: VisualInstance3D = _caster_nodes[label]
			var cinfo: Dictionary = by_obj.get(node.get_instance_id(), {})
			var texels := int(cinfo.get("texels_written", 0))
			per_class[label] = {
				"pairs": int(cinfo.get("pairs_emitted", 0)),
				"texels": texels,
				"dynamic": bool(cinfo.get("is_dynamic", false)),
				"dynamic_reasons": int(cinfo.get("dynamic_reasons", 0)),
			}
			if cinfo.is_empty():
				_failures.append("rest_pose: caster '%s' missing from VSM caster list" % label)
			elif int(cinfo.get("pairs_emitted", 0)) <= 0 or texels <= 0:
				_failures.append("rest_pose: caster '%s' emitted %d pairs and wrote %d texels" % [label, int(cinfo.get("pairs_emitted", 0)), texels])
		summary["caster_classes"] = per_class

		# The particle quads face the light: they cast in the straight-down ProbeSpot's pages.
		var bb_info: Dictionary = by_obj.get((_caster_nodes["billboard_particles"] as VisualInstance3D).get_instance_id(), {})
		var bb_light_texels: Dictionary = bb_info.get("light_texels", {})
		var probe_spot_id := -1
		for light in p_res.get("lights", []):
			if int(light.get("kind", -1)) == 1 and (light.get("position", Vector3.INF) as Vector3).distance_to(_probe_spot.global_position) < 0.001:
				probe_spot_id = int(light.get("id", -1))
		var bb_spot_texels := int(bb_light_texels.get(probe_spot_id, 0))
		if probe_spot_id < 0:
			_failures.append("rest_pose: ProbeSpot is not among the raster check's lights")
		var flat_info: Dictionary = by_obj.get((_caster_nodes["flat_reference_quad"] as VisualInstance3D).get_instance_id(), {})
		var flat_spot_texels := int((flat_info.get("light_texels", {}) as Dictionary).get(probe_spot_id, 0))
		var bb_ratio := float(bb_spot_texels) / float(maxi(BILLBOARD_PARTICLE_COUNT * flat_spot_texels, 1))
		summary["billboard_particles_probe_spot_texels"] = bb_spot_texels
		summary["flat_reference_probe_spot_texels"] = flat_spot_texels
		summary["billboard_to_flat_ratio"] = bb_ratio
		if flat_spot_texels <= 0 or bb_ratio < 0.8 or bb_ratio > 1.25:
			_failures.append("rest_pose: the Particle Billboard quads drew %d fragments into ProbeSpot's pages, %.3f times %d flat quads of the same size (expected about 1: they face the light)" % [bb_spot_texels, bb_ratio, BILLBOARD_PARTICLE_COUNT])
		# The same quads under the Sun's clipmaps: a quad facing the sun covers 1 / |direction.y| times the
		# flat quad's footprint in the light's view.
		var sun_id := -1
		var sun_dir := Vector3.ZERO
		for light in p_res.get("lights", []):
			if int(light.get("kind", -1)) == 0:
				sun_id = int(light.get("id", -1))
				sun_dir = light.get("direction", Vector3.ZERO) as Vector3
		var bb_sun_texels := int(bb_light_texels.get(sun_id, 0))
		var flat_sun_texels := int((flat_info.get("light_texels", {}) as Dictionary).get(sun_id, 0))
		var sun_expected := 1.0 / maxf(absf(sun_dir.y), 0.05)
		var sun_ratio := float(bb_sun_texels) / float(maxi(BILLBOARD_PARTICLE_COUNT * flat_sun_texels, 1)) / sun_expected
		summary["billboard_particles_sun_texels"] = bb_sun_texels
		summary["flat_reference_sun_texels"] = flat_sun_texels
		summary["billboard_to_flat_sun_ratio_over_expected"] = sun_ratio
		if sun_id < 0:
			_failures.append("rest_pose: the Sun is not among the raster check's lights")
		elif flat_sun_texels <= 0 or sun_ratio < 0.8 or sun_ratio > 1.25:
			_failures.append("rest_pose: the Particle Billboard quads drew %d fragments into the Sun's pages, %.3f times the %.3f expected for %d quads facing the sun against flat quads" % [bb_sun_texels, sun_ratio, sun_expected, BILLBOARD_PARTICLE_COUNT])
		for main_cam_label in ["billboard_multimesh", "billboard_mesh"]:
			var mc_info: Dictionary = by_obj.get((_caster_nodes[main_cam_label] as VisualInstance3D).get_instance_id(), {})
			if (int(mc_info.get("dynamic_reasons", 0)) & 128) == 0:
				_failures.append("rest_pose: the camera-facing '%s' is not marked as following the main camera (dynamic reasons %d)" % [main_cam_label, int(mc_info.get("dynamic_reasons", 0))])

		# Verify static vs dynamic layer classification on our controlled caster classes.
		for dyn_label in ["skinned_plate", "skinned_walker", "blend_shape", "particles", "vertex_wave", "billboard_particles", "billboard_multimesh", "billboard_mesh", "custom_aabb_multimesh"]:
			var cinfo: Dictionary = by_obj.get((_caster_nodes[dyn_label] as VisualInstance3D).get_instance_id(), {})
			if not cinfo.is_empty() and not bool(cinfo.get("is_dynamic", false)):
				_failures.append("rest_pose: expected '%s' to route to dynamic layer 1" % dyn_label)
		for stat_label in ["static_box", "alpha_scissor", "multimesh_3d", "multimesh_2d"]:
			var cinfo: Dictionary = by_obj.get((_caster_nodes[stat_label] as VisualInstance3D).get_instance_id(), {})
			if not cinfo.is_empty() and bool(cinfo.get("is_dynamic", true)):
				_failures.append("rest_pose: expected '%s' to route to static layer 0" % stat_label)

		# Verify alpha-scissor checkerboard cutout on ProbeSpot: at least one spot page's static_row32
		# or static_col32 must contain BOTH opaque card texels (~5.0 m) AND discarded hole texels (~8.0 m Ground).
		var saw_cutout_transition := false
		for pd in p_res.get("dirty_pages", []):
			if int(pd.get("kind", -1)) != 1: # MAP_KIND_SPOT == 1
				continue
			for axis_key in ["static_row32", "static_col32"]:
				if not pd.has(axis_key):
					continue
				var prof: PackedFloat32Array = pd[axis_key]
				var has_card := false
				var has_hole_ground := false
				for dval in prof:
					if dval >= 4.85 and dval <= 5.15:
						has_card = true
					elif dval >= 7.85 and dval <= 8.15:
						has_hole_ground = true
				if has_card and has_hole_ground:
					saw_cutout_transition = true
		summary["saw_cutout_transition"] = saw_cutout_transition
		if not saw_cutout_transition:
			_failures.append("rest_pose: alpha-scissor checkerboard did not show both card (~5.0 m) and discarded hole (~8.0 m) depths on any spot page profile")

	# Verify ProbeSpot minimum dynamic depth across stages:
	# - rest_pose: BlendShapePlate and SkinnedPlate both at y = 1.5 -> w = 6.5 m
	# - blend_lifted: BlendShapePlate weight = 1.0 -> y = 3.0 -> w = 5.0 m
	# - bone_posed: SkinnedPlate Bone 1 +3.0 m -> y = 4.5 -> w = 3.5 m
	var min_spot_dyn := INF
	for pd in p_res.get("dirty_pages", []):
		# ProbeSpot has range 16 m (ReverseCullSpot is aimed at (-4, 0.5, 0) with no dynamic casters near w <= 6.6 m,
		# and we also filter to pages whose dynamic_min_depth < 7.0 m).
		if int(pd.get("kind", -1)) == 1 and int(pd.get("dynamic_nonzero", 0)) > 0:
			var dmin := float(pd.get("dynamic_min_depth", INF))
			if dmin < min_spot_dyn:
				min_spot_dyn = dmin
	summary["min_spot_dynamic_depth"] = min_spot_dyn if is_finite(min_spot_dyn) else -1.0

	var expected_dyn := 6.5
	if p_name == "blend_lifted":
		expected_dyn = 5.0
	elif p_name == "bone_posed":
		expected_dyn = 3.5
	if not is_finite(min_spot_dyn) or absf(min_spot_dyn - expected_dyn) > 0.2:
		_failures.append("%s: expected spot dynamic_min_depth ≈ %.2f m, got %s" % [p_name, expected_dyn, str(min_spot_dyn)])

	_captures.append(summary)


func _finish() -> void:
	_result["failures"] = _failures
	_result["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
