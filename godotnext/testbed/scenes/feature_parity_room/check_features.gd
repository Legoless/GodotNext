extends SceneTree
## Feature Parity Room structural check (#64 step 6).
##
## Loads the generated scene and asserts that every feature node, setting
## and fallback mesh from the issue exists with the required values.
## Exits 0 when everything passes, 1 on the first completed failure list.
##
## Run:
## [code]godot --headless --path godotnext/testbed --script res://scenes/feature_parity_room/check_features.gd[/code]

const SCENE_PATH := "res://scenes/feature_parity_room/feature_parity_room.tscn"
const OVERRIDE_PATH := "res://scenes/feature_parity_room/override.cfg"
const LAYER_2_ONLY := 2

var _failures: Array[String] = []
var _checks := 0


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var packed := load(SCENE_PATH) as PackedScene
	if packed == null:
		printerr("check_features FAIL: cannot load " + SCENE_PATH)
		quit(1)
		return
	var root := packed.instantiate()
	_check_decals(root)
	_check_probe(root)
	_check_lights(root)
	_check_environment(root)
	_check_sh_receiver(root)
	_check_multibounce(root)
	_check_materials(root)
	_check_fallbacks(root)
	_check_cameras(root)
	root.free()
	if _failures.is_empty():
		print("check_features: all %d assertions passed (7 feature items, materials, fallbacks, cameras)." % _checks)
		quit(0)
	else:
		for failure in _failures:
			printerr("check_features FAIL: " + failure)
		quit(1)


func _ok(p_condition: bool, p_message: String) -> bool:
	_checks += 1
	if not p_condition:
		_failures.append(p_message)
	return p_condition


func _check_decals(p_root: Node) -> void:
	for decal_name in ["DecalWall", "DecalFloor", "DecalCulled"]:
		var decal := p_root.get_node_or_null(decal_name) as Decal
		if not _ok(decal != null, "missing Decal node %s." % decal_name):
			continue
		_ok(decal.texture_albedo != null, "%s: texture_albedo is not set." % decal_name)
		_ok(decal.texture_normal != null, "%s: texture_normal is not set." % decal_name)
		_ok(decal.texture_orm != null, "%s: texture_orm is not set." % decal_name)
		_ok(decal.texture_emission != null, "%s: texture_emission is not set." % decal_name)
	var culled := p_root.get_node_or_null("DecalCulled") as Decal
	if culled != null:
		_ok(culled.cull_mask & 1 != 0, "DecalCulled: cull_mask must include layer 1.")
		_ok(culled.cull_mask & LAYER_2_ONLY == 0, "DecalCulled: cull_mask must exclude layer 2.")
	var block := p_root.get_node_or_null("Layer2Block") as MeshInstance3D
	if _ok(block != null, "missing Layer2Block."):
		_ok(block.layers == LAYER_2_ONLY, "Layer2Block must be on layer 2 only, got %d." % block.layers)


func _check_probe(p_root: Node) -> void:
	var probe := p_root.get_node_or_null("ReflectionProbeMetal") as ReflectionProbe
	if _ok(probe != null, "missing ReflectionProbeMetal."):
		_ok(probe.box_projection, "ReflectionProbeMetal: box_projection must be on.")
		_ok(probe.update_mode == ReflectionProbe.UPDATE_ONCE, "ReflectionProbeMetal: update_mode must be UPDATE_ONCE.")
	var panel := p_root.get_node_or_null("MetalPanel") as MeshInstance3D
	if _ok(panel != null, "missing MetalPanel."):
		var material := panel.get_surface_override_material(0) as StandardMaterial3D
		if _ok(material != null, "MetalPanel: surface material must be a StandardMaterial3D."):
			_ok(material.metallic >= 0.9, "MetalPanel: metallic must be >= 0.9, got %0.2f." % material.metallic)
			_ok(material.roughness >= 0.2 and material.roughness <= 0.6,
					"MetalPanel: roughness must be rough-metal (0.2..0.6), got %0.2f." % material.roughness)


func _check_lights(p_root: Node) -> void:
	var area := p_root.get_node_or_null("AreaLight") as AreaLight3D
	if _ok(area != null, "missing AreaLight (AreaLight3D)."):
		_ok(area.area_texture != null, "AreaLight: area_texture is not set.")
		_ok(area.area_size.x > 0.0 and area.area_size.y > 0.0, "AreaLight: area_size must be positive.")
	var spot := p_root.get_node_or_null("SpotProjector") as SpotLight3D
	if _ok(spot != null, "missing SpotProjector (SpotLight3D)."):
		_ok(spot.light_projector != null, "SpotProjector: light_projector is not set.")
	var omni := p_root.get_node_or_null("OmniCulled") as OmniLight3D
	if _ok(omni != null, "missing OmniCulled (OmniLight3D)."):
		_ok(omni.light_cull_mask & 1 != 0, "OmniCulled: light_cull_mask must include layer 1.")
		_ok(omni.light_cull_mask & LAYER_2_ONLY == 0, "OmniCulled: light_cull_mask must exclude layer 2.")


func _check_environment(p_root: Node) -> void:
	var world_env := p_root.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if not _ok(world_env != null and world_env.environment != null, "missing WorldEnvironment with an Environment."):
		return
	var env := world_env.environment
	_ok(env.background_mode == Environment.BG_SKY and env.sky != null, "Environment: background must be a sky.")
	_ok(env.fog_enabled, "Environment: fog must be enabled.")
	_ok(env.fog_mode == Environment.FOG_MODE_DEPTH, "Environment: fog_mode must be FOG_MODE_DEPTH.")
	_ok(env.fog_height_density > 0.0, "Environment: fog_height_density must be > 0.")
	_ok(env.fog_aerial_perspective > 0.0, "Environment: fog_aerial_perspective must be > 0.")
	_ok(env.tonemap_mode == Environment.TONE_MAPPER_LINEAR, "Environment: tonemapper must be linear.")
	_ok(is_equal_approx(env.tonemap_exposure, 1.0), "Environment: tonemap_exposure must be fixed at 1.0.")


func _check_sh_receiver(p_root: Node) -> void:
	_ok(p_root.get_node_or_null("LightmapGI") is LightmapGI, "missing LightmapGI node.")
	var receiver := p_root.get_node_or_null("SHReceiver") as MeshInstance3D
	if _ok(receiver != null, "missing SHReceiver MeshInstance3D."):
		_ok(receiver.gi_mode == GeometryInstance3D.GI_MODE_DYNAMIC,
				"SHReceiver: gi_mode must be GI_MODE_DYNAMIC, got %d." % receiver.gi_mode)


func _check_multibounce(_p_root: Node) -> void:
	if not _ok(FileAccess.file_exists(OVERRIDE_PATH), "missing sibling override.cfg."):
		return
	var text := FileAccess.get_file_as_string(OVERRIDE_PATH)
	_ok("[rendering]" in text and "lights_and_shadows/multi_bounce_occlusion/enabled=true" in text,
			"override.cfg must set rendering/lights_and_shadows/multi_bounce_occlusion/enabled=true.")


func _surface_material(p_root: Node, p_name: String) -> StandardMaterial3D:
	var instance := p_root.get_node_or_null(p_name) as MeshInstance3D
	if not _ok(instance != null, "missing mesh %s." % p_name):
		return null
	var material := instance.get_surface_override_material(0) as StandardMaterial3D
	_ok(material != null, "%s: surface material must be a StandardMaterial3D." % p_name)
	return material


func _check_materials(p_root: Node) -> void:
	var floor_instance := p_root.get_node_or_null("Floor") as MeshInstance3D
	if _ok(floor_instance != null, "missing mesh Floor."):
		var textured := floor_instance.get_surface_override_material(0) as ORMMaterial3D
		if _ok(textured != null, "Floor: textured material must be an ORMMaterial3D (albedo, normal, ORM)."):
			_ok(textured.albedo_texture != null, "textured material: albedo_texture missing.")
			_ok(textured.normal_enabled and textured.normal_texture != null, "textured material: normal map missing.")
			_ok(textured.orm_texture != null, "textured material: orm_texture missing.")
			_ok(not textured.uv1_scale.is_equal_approx(Vector3.ONE), "textured material: uv1_scale must differ from 1.")
			_ok(not textured.uv1_offset.is_equal_approx(Vector3.ZERO), "textured material: uv1_offset must be non-zero.")
	var triplanar := _surface_material(p_root, "TriplanarBlock")
	if triplanar != null:
		_ok(triplanar.uv1_triplanar, "TriplanarBlock: uv1_triplanar must be on.")
		_ok(triplanar.ao_enabled and triplanar.ao_texture != null, "TriplanarBlock: AO texture missing (multi-bounce AO).")
	var anisotropy := _surface_material(p_root, "AnisotropyBlock")
	if anisotropy != null:
		_ok(anisotropy.anisotropy_enabled, "AnisotropyBlock: anisotropy_enabled must be on.")
	var clearcoat := _surface_material(p_root, "ClearcoatBlock")
	if clearcoat != null:
		_ok(clearcoat.clearcoat_enabled, "ClearcoatBlock: clearcoat_enabled must be on.")
	var rim := _surface_material(p_root, "RimBlock")
	if rim != null:
		_ok(rim.rim_enabled, "RimBlock: rim_enabled must be on.")
	var shader_block := p_root.get_node_or_null("ShaderBlock") as MeshInstance3D
	if _ok(shader_block != null, "missing ShaderBlock."):
		var material := shader_block.get_surface_override_material(0) as ShaderMaterial
		if _ok(material != null and material.shader != null, "ShaderBlock: needs a ShaderMaterial with a shader."):
			var code := material.shader.code
			_ok("void vertex()" in code, "ShaderBlock shader: must define vertex().")
			_ok(not ("VERTEX =" in code) and not ("VERTEX +=" in code) and not ("VERTEX *=" in code),
					"ShaderBlock shader: vertex() must not move positions (no VERTEX writes).")


func _check_fallbacks(p_root: Node) -> void:
	var per_vertex := _surface_material(p_root, "fallback_per_vertex")
	if per_vertex != null:
		_ok(per_vertex.shading_mode == BaseMaterial3D.SHADING_MODE_PER_VERTEX,
				"fallback_per_vertex: shading_mode must be SHADING_MODE_PER_VERTEX.")
	_check_reason(p_root, "fallback_per_vertex", "VERTEX_LIGHTING")
	var next_pass := _surface_material(p_root, "fallback_next_pass")
	if next_pass != null:
		_ok(next_pass.next_pass != null, "fallback_next_pass: material must carry a next_pass.")
	_check_reason(p_root, "fallback_next_pass", "NEXT_PASS_OR_OVERLAY")
	var overlay := p_root.get_node_or_null("fallback_overlay") as MeshInstance3D
	if _ok(overlay != null, "missing fallback_overlay."):
		_ok(overlay.material_overlay != null, "fallback_overlay: material_overlay must be set.")
	_check_reason(p_root, "fallback_overlay", "NEXT_PASS_OR_OVERLAY")


func _check_reason(p_root: Node, p_name: String, p_reason: String) -> void:
	var instance := p_root.get_node_or_null(p_name)
	if instance == null:
		return
	_ok(String(instance.get_meta("fallback_reason", "")) == p_reason,
			"%s: fallback_reason metadata must be '%s'." % [p_name, p_reason])


func _check_cameras(p_root: Node) -> void:
	for camera_name in ["cam_0", "cam_1", "cam_2"]:
		_ok(p_root.get_node_or_null(camera_name) is Camera3D, "missing Camera3D %s." % camera_name)
	var room: Vector3 = p_root.get_meta("room_interior_m", Vector3.ZERO)
	_ok(room.is_equal_approx(Vector3(8.0, 3.0, 6.0)), "room interior metadata must be 8 x 3 x 6 m, got %s." % room)
	_ok(p_root.get_script() != null, "scene root must carry the camera/capture script.")
