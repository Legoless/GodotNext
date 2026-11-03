extends SceneTree
## Feature Parity Room generator (#64).
##
## Builds [code]res://scenes/feature_parity_room/feature_parity_room.tscn[/code]:
## a sealed room 8 x 6 x 3 m (X x Z x Y) with every forward feature the D7
## material-bucket resolve must match by construction (decals, a boxed
## reflection probe over a rough metal panel, a textured area light, a
## projector spot, a layer-culled omni, depth fog with height density and
## aerial perspective under a sky, an SH-probe receiver next to a
## LightmapGI, and AO-textured materials for multi-bounce occlusion),
## plus three [code]fallback_*[/code] meshes that must keep the legacy
## path. Three fixed cameras all sit on the south side looking north, so
## every feature node is in every frustum.
##
## Run from the repository root:
## [code]godot --headless --path godotnext/testbed --script res://generators/build_feature_parity_room.gd[/code]
##
## Seeded through [code]scaffold.gd[/code] and saved byte-stably: two runs
## produce an identical [code].tscn[/code]. Every texture is a procedural
## [FastNoiseLite]/gradient resource generated here, so the scene tracks
## no binary assets. Multi-bounce occlusion is restart-only and project-
## wide, so it lives in the sibling [code]override.cfg[/code], never in
## [code]project.godot[/code].

const Scaffold := preload("res://generators/scaffold.gd")

const SCENE_PATH := "res://scenes/feature_parity_room/feature_parity_room.tscn"
const ROOM_SCRIPT_PATH := "res://scenes/feature_parity_room/parity_room.gd"
const ROOM_SIZE := Vector3(8.0, 3.0, 6.0)  # X, Y, Z.
const WALL := 0.20
const LAYER_2_ONLY := 2  # VisualInstance3D layers bit for layer 2.
const ALL_LAYERS := (1 << 20) - 1
const WITHOUT_LAYER_2 := ALL_LAYERS & ~LAYER_2_ONLY


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var root := _build()
	var err := Scaffold.save_scene(root, SCENE_PATH)
	root.free()
	if err != OK:
		push_error("build_feature_parity_room: save failed with error %d." % err)
		quit(1)
		return
	print("build_feature_parity_room: wrote " + SCENE_PATH)
	quit(0)


func _build() -> Node3D:
	var root := Node3D.new()
	root.name = "FeatureParityRoom"
	root.set_meta("room_interior_m", ROOM_SIZE)
	root.set_meta("generator_seed", Scaffold.DEFAULT_SEED)
	root.set_script(load(ROOM_SCRIPT_PATH))

	_add_environment(root)

	var tex := _make_textures()
	var mat := _make_materials(tex)
	_add_room_shell(root, mat)
	_add_feature_meshes(root, mat)
	_add_decals(root, tex)
	_add_probe(root)

	# Textured area light on the ceiling, facing down (-Z of the light is
	# its emitting direction once pitched).
	var area := AreaLight3D.new()
	area.name = "AreaLight"
	area.area_texture = tex["emission"]
	area.area_size = Vector2(2.4, 1.2)
	area.light_energy = 1.5
	area.area_range = 8.0
	area.transform = Transform3D(Basis.from_euler(Vector3(deg_to_rad(-90.0), 0.0, 0.0)), Vector3(0.0, 2.7, -0.8))
	Scaffold.add_owned(root, area, root)

	var spot := SpotLight3D.new()
	spot.name = "SpotProjector"
	spot.light_projector = tex["projector"]
	var spot_pos := Vector3(-2.2, 2.6, 1.8)
	var spot_target := Vector3(-2.0, 1.2, -3.0)
	spot.transform = Transform3D(Basis.looking_at((spot_target - spot_pos).normalized(), Vector3.UP), spot_pos)
	spot.light_energy = 2.0
	spot.spot_range = 10.0
	spot.spot_angle = 30.0
	spot.shadow_enabled = true
	Scaffold.add_owned(root, spot, root)

	var omni := OmniLight3D.new()
	omni.name = "OmniCulled"
	omni.position = Vector3(2.0, 2.2, 0.5)
	omni.light_energy = 1.2
	omni.omni_range = 7.0
	omni.shadow_enabled = true
	omni.light_cull_mask = WITHOUT_LAYER_2
	Scaffold.add_owned(root, omni, root)

	# Unbaked on purpose: LightmapGI::bake() is editor-only (same status as
	# the #10 lightmap scenes). The bake settings are recorded here; the
	# .lmbake (at most 4 MB) is produced in the editor and logged in
	# PROVENANCE.md when done.
	var lightmap := LightmapGI.new()
	lightmap.name = "LightmapGI"
	lightmap.bounces = 3
	lightmap.quality = LightmapGI.BAKE_QUALITY_HIGH
	lightmap.use_denoiser = false
	lightmap.use_texture_for_bounces = false
	Scaffold.add_owned(root, lightmap, root)

	# All three cameras sit on the south side looking at the north wall so
	# every feature node is visible from every camera.
	_forced_camera(root, "cam_0", Vector3(-3.2, 2.2, 2.2), Vector3(0.0, 1.1, -1.5), true)
	_forced_camera(root, "cam_1", Vector3(3.2, 2.2, 2.2), Vector3(0.0, 1.1, -1.5), false)
	_forced_camera(root, "cam_2", Vector3(0.0, 2.5, 2.5), Vector3(0.0, 1.0, -1.5), false)
	return root


func _forced_camera(p_root: Node, p_name: String, p_pos: Vector3, p_target: Vector3, p_current: bool) -> void:
	Scaffold.make_camera(p_root, p_root, p_name, p_pos, p_target, 30.0, 65.0, p_current)


func _add_environment(p_root: Node3D) -> void:
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	environment.tonemap_exposure = 1.0
	environment.fog_enabled = true
	environment.fog_mode = Environment.FOG_MODE_DEPTH
	environment.fog_density = 0.01
	environment.fog_height_density = 0.05
	environment.fog_aerial_perspective = 0.4
	environment.fog_depth_begin = 4.0
	environment.fog_depth_end = 30.0
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = environment
	Scaffold.add_owned(p_root, world_env, p_root)


func _noise_texture(p_name: String, p_seed_offset: int, p_dark: Color, p_light: Color, p_normal_map: bool = false) -> NoiseTexture2D:
	var noise := FastNoiseLite.new()
	noise.seed = Scaffold.DEFAULT_SEED + p_seed_offset
	noise.frequency = 0.08
	noise.fractal_octaves = 3
	var ramp := Gradient.new()
	ramp.set_color(0, p_dark)
	ramp.set_color(1, p_light)
	var texture := NoiseTexture2D.new()
	texture.resource_name = p_name
	texture.width = 128
	texture.height = 128
	texture.noise = noise
	texture.color_ramp = ramp
	texture.seamless = true
	texture.as_normal_map = p_normal_map
	return texture


func _radial_texture(p_name: String, p_inner: Color, p_outer: Color) -> GradientTexture2D:
	var ramp := Gradient.new()
	ramp.set_color(0, p_inner)
	ramp.set_color(1, p_outer)
	var texture := GradientTexture2D.new()
	texture.resource_name = p_name
	texture.gradient = ramp
	texture.fill = GradientTexture2D.FILL_RADIAL
	texture.fill_from = Vector2(0.5, 0.5)
	texture.fill_to = Vector2(1.0, 0.5)
	texture.width = 128
	texture.height = 128
	return texture


func _make_textures() -> Dictionary:
	return {
		"albedo": _noise_texture("ParityAlbedo", 11, Color(0.35, 0.28, 0.22, 1.0), Color(0.85, 0.78, 0.68, 1.0)),
		"normal": _noise_texture("ParityNormal", 12, Color(0.0, 0.0, 0.0, 1.0), Color(1.0, 1.0, 1.0, 1.0), true),
		"orm": _noise_texture("ParityOrm", 13, Color(0.45, 0.25, 0.0, 1.0), Color(1.0, 0.8, 0.2, 1.0)),
		"ao": _noise_texture("ParityAo", 14, Color(0.3, 0.3, 0.3, 1.0), Color(1.0, 1.0, 1.0, 1.0)),
		"emission": _radial_texture("ParityEmission", Color(1.0, 0.75, 0.4, 1.0), Color(0.0, 0.0, 0.0, 1.0)),
		"projector": _radial_texture("ParityProjector", Color(1.0, 1.0, 1.0, 1.0), Color(0.15, 0.25, 0.6, 1.0)),
		"decal_albedo": _radial_texture("DecalAlbedo", Color(0.9, 0.25, 0.15, 1.0), Color(0.9, 0.25, 0.15, 0.0)),
		"decal_emission": _radial_texture("DecalEmission", Color(0.3, 0.9, 0.6, 1.0), Color(0.0, 0.0, 0.0, 1.0)),
	}


func _make_materials(p_tex: Dictionary) -> Dictionary:
	# ORM workflow in Godot is its own class: the packed map carries AO (R),
	# roughness (G) and metallic (B), so it also covers the multi-bounce AO
	# input. The triplanar material below carries a separate AO texture.
	var textured := ORMMaterial3D.new()
	textured.resource_name = "ParityTextured"
	textured.albedo_texture = p_tex["albedo"]
	textured.normal_enabled = true
	textured.normal_texture = p_tex["normal"]
	textured.orm_texture = p_tex["orm"]
	textured.uv1_scale = Vector3(2.0, 2.0, 1.0)
	textured.uv1_offset = Vector3(0.25, 0.5, 0.0)

	var triplanar := StandardMaterial3D.new()
	triplanar.resource_name = "ParityTriplanar"
	triplanar.albedo_texture = p_tex["albedo"]
	triplanar.normal_enabled = true
	triplanar.normal_texture = p_tex["normal"]
	triplanar.ao_enabled = true
	triplanar.ao_texture = p_tex["ao"]
	triplanar.ao_light_affect = 0.5
	triplanar.uv1_triplanar = true
	triplanar.uv1_scale = Vector3(1.5, 1.5, 1.5)

	var metal := StandardMaterial3D.new()
	metal.resource_name = "ParityRoughMetal"
	metal.albedo_color = Color(0.72, 0.74, 0.78)
	metal.metallic = 1.0
	metal.roughness = 0.45
	metal.normal_enabled = true
	metal.normal_texture = p_tex["normal"]

	var anisotropy := StandardMaterial3D.new()
	anisotropy.resource_name = "ParityAnisotropy"
	anisotropy.albedo_color = Color(0.55, 0.45, 0.35)
	anisotropy.metallic = 0.9
	anisotropy.roughness = 0.35
	anisotropy.anisotropy_enabled = true
	anisotropy.anisotropy = 0.6
	anisotropy.anisotropy_flowmap = p_tex["normal"]

	var clearcoat := StandardMaterial3D.new()
	clearcoat.resource_name = "ParityClearcoat"
	clearcoat.albedo_color = Color(0.35, 0.1, 0.5)
	clearcoat.roughness = 0.6
	clearcoat.clearcoat_enabled = true
	clearcoat.clearcoat = 0.7
	clearcoat.clearcoat_roughness = 0.25

	var rim := StandardMaterial3D.new()
	rim.resource_name = "ParityRim"
	rim.albedo_color = Color(0.15, 0.3, 0.55)
	rim.roughness = 0.5
	rim.rim_enabled = true
	rim.rim = 0.8
	rim.rim_tint = 0.5

	var shader := Shader.new()
	shader.code = "shader_type spatial;\n" \
			+ "render_mode diffuse_burley, specular_schlick_ggx;\n\n" \
			+ "void vertex() {\n\t// Positions are intentionally left untouched.\n}\n\n" \
			+ "void fragment() {\n" \
			+ "\tfloat bands = 0.5 + 0.5 * sin(UV.x * 40.0);\n" \
			+ "\tALBEDO = mix(vec3(0.15, 0.35, 0.2), vec3(0.85, 0.7, 0.25), bands);\n" \
			+ "\tROUGHNESS = 0.55;\n}\n"
	var shader_material := ShaderMaterial.new()
	shader_material.resource_name = "ParityShader"
	shader_material.shader = shader

	var plain := Scaffold.make_constant_material("ParityGrey", Color(0.65, 0.65, 0.65))

	var per_vertex := StandardMaterial3D.new()
	per_vertex.resource_name = "FallbackPerVertex"
	per_vertex.albedo_color = Color(0.8, 0.5, 0.2)
	per_vertex.shading_mode = BaseMaterial3D.SHADING_MODE_PER_VERTEX

	var next_pass_inner := StandardMaterial3D.new()
	next_pass_inner.resource_name = "FallbackNextPassBase"
	next_pass_inner.albedo_color = Color(0.3, 0.6, 0.3)
	var next_pass_outer := StandardMaterial3D.new()
	next_pass_outer.resource_name = "FallbackNextPassShell"
	next_pass_outer.albedo_color = Color(0.9, 0.85, 0.2, 0.5)
	next_pass_outer.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	next_pass_outer.grow = true
	next_pass_outer.grow_amount = 0.03
	next_pass_inner.next_pass = next_pass_outer

	var overlay := StandardMaterial3D.new()
	overlay.resource_name = "FallbackOverlay"
	overlay.albedo_color = Color(0.25, 0.4, 0.9)
	overlay.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED

	return {
		"textured": textured,
		"triplanar": triplanar,
		"metal": metal,
		"anisotropy": anisotropy,
		"clearcoat": clearcoat,
		"rim": rim,
		"shader": shader_material,
		"plain": plain,
		"per_vertex": per_vertex,
		"next_pass": next_pass_inner,
		"overlay": overlay,
	}


func _add_room_shell(p_root: Node3D, p_mat: Dictionary) -> void:
	var half := ROOM_SIZE * 0.5
	var span_y := ROOM_SIZE.y + 2.0 * WALL
	var textured: Material = p_mat["textured"]
	var plain: Material = p_mat["plain"]
	Scaffold.add_box_instance(p_root, p_root, "Floor",
			Vector3(0.0, -WALL * 0.5, 0.0),
			Vector3(ROOM_SIZE.x + 2.0 * WALL, WALL, ROOM_SIZE.z + 2.0 * WALL), textured)
	Scaffold.add_box_instance(p_root, p_root, "Ceiling",
			Vector3(0.0, ROOM_SIZE.y + WALL * 0.5, 0.0),
			Vector3(ROOM_SIZE.x + 2.0 * WALL, WALL, ROOM_SIZE.z + 2.0 * WALL), plain)
	Scaffold.add_box_instance(p_root, p_root, "NorthWall",
			Vector3(0.0, ROOM_SIZE.y * 0.5, -half.z - WALL * 0.5),
			Vector3(ROOM_SIZE.x + 2.0 * WALL, span_y, WALL), textured)
	Scaffold.add_box_instance(p_root, p_root, "SouthWall",
			Vector3(0.0, ROOM_SIZE.y * 0.5, half.z + WALL * 0.5),
			Vector3(ROOM_SIZE.x + 2.0 * WALL, span_y, WALL), plain)
	Scaffold.add_box_instance(p_root, p_root, "WestWall",
			Vector3(-half.x - WALL * 0.5, ROOM_SIZE.y * 0.5, 0.0),
			Vector3(WALL, span_y, ROOM_SIZE.z), plain)
	Scaffold.add_box_instance(p_root, p_root, "EastWall",
			Vector3(half.x + WALL * 0.5, ROOM_SIZE.y * 0.5, 0.0),
			Vector3(WALL, span_y, ROOM_SIZE.z), plain)


func _add_feature_meshes(p_root: Node3D, p_mat: Dictionary) -> void:
	# Rough metallic panel on the north wall, under the reflection probe.
	Scaffold.add_box_instance(p_root, p_root, "MetalPanel",
			Vector3(0.4, 1.4, -2.82), Vector3(2.4, 1.6, 0.15), p_mat["metal"], false)
	Scaffold.add_box_instance(p_root, p_root, "TriplanarBlock",
			Vector3(-2.6, 0.5, -0.8), Vector3(1.0, 1.0, 1.0), p_mat["triplanar"])
	Scaffold.add_box_instance(p_root, p_root, "AnisotropyBlock",
			Vector3(-1.0, 0.35, 0.6), Vector3(0.7, 0.7, 0.7), p_mat["anisotropy"])
	Scaffold.add_box_instance(p_root, p_root, "ClearcoatBlock",
			Vector3(0.6, 0.4, 0.5), Vector3(0.8, 0.8, 0.8), p_mat["clearcoat"])
	Scaffold.add_box_instance(p_root, p_root, "RimBlock",
			Vector3(1.9, 0.35, -0.7), Vector3(0.7, 0.7, 0.7), p_mat["rim"])
	Scaffold.add_box_instance(p_root, p_root, "ShaderBlock",
			Vector3(2.9, 0.45, 0.8), Vector3(0.9, 0.9, 0.9), p_mat["shader"])

	# Layer-2 block: the culled decal and the culled omni must not touch it.
	var layer_block := Scaffold.add_box_instance(p_root, p_root, "Layer2Block",
			Vector3(2.6, 0.3, -2.2), Vector3(0.6, 0.6, 0.6), p_mat["plain"])
	layer_block.layers = LAYER_2_ONLY

	var receiver := Scaffold.add_box_instance(p_root, p_root, "SHReceiver",
			Vector3(-0.6, 0.5, -2.0), Vector3(1.0, 1.0, 1.0), p_mat["plain"])
	receiver.gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC

	var per_vertex := Scaffold.add_box_instance(p_root, p_root, "fallback_per_vertex",
			Vector3(-3.4, 0.3, 0.6), Vector3(0.6, 0.6, 0.6), p_mat["per_vertex"])
	per_vertex.set_meta("fallback_reason", "VERTEX_LIGHTING")
	var next_pass := Scaffold.add_box_instance(p_root, p_root, "fallback_next_pass",
			Vector3(-3.4, 0.3, -1.0), Vector3(0.6, 0.6, 0.6), p_mat["next_pass"])
	next_pass.set_meta("fallback_reason", "NEXT_PASS_OR_OVERLAY")
	var overlay_mesh := Scaffold.add_box_instance(p_root, p_root, "fallback_overlay",
			Vector3(-3.4, 1.0, -0.2), Vector3(0.6, 0.6, 0.6), p_mat["plain"])
	overlay_mesh.material_overlay = p_mat["overlay"]
	overlay_mesh.set_meta("fallback_reason", "NEXT_PASS_OR_OVERLAY")


func _set_decal_textures(p_decal: Decal, p_tex: Dictionary) -> void:
	p_decal.texture_albedo = p_tex["decal_albedo"]
	p_decal.texture_normal = p_tex["normal"]
	p_decal.texture_orm = p_tex["orm"]
	p_decal.texture_emission = p_tex["decal_emission"]
	p_decal.emission_energy = 1.5


func _add_decals(p_root: Node3D, p_tex: Dictionary) -> void:
	var wall_decal := Decal.new()
	wall_decal.name = "DecalWall"
	_set_decal_textures(wall_decal, p_tex)
	wall_decal.size = Vector3(1.8, 1.6, 0.8)
	wall_decal.position = Vector3(-2.0, 1.5, -2.7)
	Scaffold.add_owned(p_root, wall_decal, p_root)

	var floor_decal := Decal.new()
	floor_decal.name = "DecalFloor"
	_set_decal_textures(floor_decal, p_tex)
	floor_decal.size = Vector3(2.0, 0.8, 2.0)
	floor_decal.transform = Transform3D(
			Basis.from_euler(Vector3(deg_to_rad(-90.0), 0.0, 0.0)), Vector3(1.5, 0.35, -0.5))
	Scaffold.add_owned(p_root, floor_decal, p_root)

	# Over the layer-2 block and the floor: the cull mask drops layer 2, so
	# only the floor receives this one.
	var culled := Decal.new()
	culled.name = "DecalCulled"
	_set_decal_textures(culled, p_tex)
	culled.size = Vector3(1.6, 1.0, 1.6)
	culled.cull_mask = WITHOUT_LAYER_2
	culled.transform = Transform3D(
			Basis.from_euler(Vector3(deg_to_rad(-90.0), 0.0, 0.0)), Vector3(2.6, 0.45, -2.2))
	Scaffold.add_owned(p_root, culled, p_root)


func _add_probe(p_root: Node3D) -> void:
	var probe := ReflectionProbe.new()
	probe.name = "ReflectionProbeMetal"
	probe.size = Vector3(3.5, 2.6, 2.6)
	probe.position = Vector3(0.4, 1.4, -1.9)
	probe.origin_offset = Vector3(0.0, 0.0, -0.4)
	probe.box_projection = true
	probe.interior = true
	probe.update_mode = ReflectionProbe.UPDATE_ONCE
	Scaffold.add_owned(p_root, probe, p_root)
