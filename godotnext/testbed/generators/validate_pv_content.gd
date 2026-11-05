extends SceneTree
## PV content post-generation validator (#213).
##
## Run after `build_pv.gd`, from the repository root:
## `godot --headless --path godotnext/testbed --script res://generators/validate_pv_content.gd`
##
## Loads the five committed scenes and checks the §21.3.3 content contract:
## Shadow Gallery (0.5°/2° suns, 256 lights, all five caster classes), VSM
## Flythrough (16 lights, actor, path), Fog Hall (volumetric fog, 8 window
## spots, glass), RT Shadow Yard (64 lights, 20 caster layers, 20 matrix
## masks) and MegaLights Hall (64 base lights with point/spot/rect/textured
## rect/IES/channel coverage, fog, glass, SSS foliage). Also checks that the
## four IES profiles exist and that the scenes carry a 60 s flight path.

const IES_DIR := "res://assets/ies"
const SHADOW_LIGHTS := 256
const FLYTHROUGH_LIGHTS := 16
const FOG_SPOTS := 8
const YARD_LIGHTS := 64
const YARD_LAYERS := 20
const MEGA_BASE_LIGHTS := 64

var _failures: Array[String] = []


func _init() -> void:
	_run.call_deferred()


func _fail(p_message: String) -> void:
	_failures.append(p_message)
	printerr("validate_pv_content FAIL: " + p_message)


func _check(p_condition: bool, p_message: String) -> bool:
	if not p_condition:
		_fail(p_message)
	return p_condition


func _run() -> void:
	for name in ["cosine_lobe", "batwing", "narrow_spot", "street_asymmetric"]:
		_check(FileAccess.file_exists(IES_DIR + "/" + name + ".ies"), "missing IES profile %s.ies." % name)
	_check_shadow_gallery()
	_check_vsm_flythrough()
	_check_fog_hall()
	_check_rt_shadow_yard()
	_check_mega_lights_hall()
	if _failures.is_empty():
		print("validate_pv_content: all scenes verified.")
		quit(0)
	else:
		quit(1)


func _load_scene(p_path: String) -> Node3D:
	if not ResourceLoader.exists(p_path):
		_fail("%s is missing; run generators/build_pv.gd first." % p_path)
		return null
	var packed: PackedScene = load(p_path)
	if packed == null:
		_fail("%s does not load." % p_path)
		return null
	var root := packed.instantiate() as Node3D
	if root == null:
		_fail("%s does not instantiate as a Node3D." % p_path)
	return root


func _count_lights(p_root: Node3D) -> int:
	var count := 0
	for child in p_root.get_children():
		if child is Light3D and not (child is DirectionalLight3D):
			count += 1
	return count


func _check_path_and_cameras(p_root: Node3D, p_scene: String, p_cameras: int) -> void:
	var cameras := 0
	for child in p_root.get_children():
		if child is Camera3D:
			cameras += 1
	_check(cameras >= p_cameras, "%s has %d cameras, expected at least %d." % [p_scene, cameras, p_cameras])
	var path := p_root.get_node_or_null("FlightPath") as Path3D
	_check(path != null and path.curve != null and path.curve.get_baked_length() > 5.0,
			"%s has no usable flight path." % p_scene)


func _check_shadow_gallery() -> void:
	var scene := "scenes/shadow_gallery/shadow_gallery.tscn"
	var root := _load_scene("res://" + scene)
	if root == null:
		return
	var sun_a := root.get_node_or_null("SunA") as DirectionalLight3D
	var sun_b := root.get_node_or_null("SunB") as DirectionalLight3D
	_check(sun_a != null and is_equal_approx(sun_a.light_angular_distance, 0.5), scene + ": SunA is not 0.5 degrees.")
	_check(sun_b != null and is_equal_approx(sun_b.light_angular_distance, 2.0), scene + ": SunB is not 2 degrees.")
	_check(_count_lights(root) == SHADOW_LIGHTS, "%s: %d local lights, expected %d." % [scene, _count_lights(root), SHADOW_LIGHTS])
	for node_name in ["CasterMultiMesh", "CasterParticles", "CasterFoliage", "CasterVertexAnimated", "GallerySkinned"]:
		_check(root.get_node_or_null(node_name) != null, "%s: %s is missing." % [scene, node_name])
	_check_path_and_cameras(root, scene, 4)
	root.free()


func _check_vsm_flythrough() -> void:
	var scene := "scenes/vsm_flythrough/vsm_flythrough.tscn"
	var root := _load_scene("res://" + scene)
	if root == null:
		return
	_check(_count_lights(root) == FLYTHROUGH_LIGHTS, "%s: %d local lights, expected %d." % [scene, _count_lights(root), FLYTHROUGH_LIGHTS])
	_check(root.get_node_or_null("Actor") != null, scene + ": Actor is missing.")
	_check(root.get_script() != null, scene + ": no flythrough runtime script.")
	_check_path_and_cameras(root, scene, 4)
	root.free()


func _check_fog_hall() -> void:
	var scene := "scenes/fog_hall/fog_hall.tscn"
	var root := _load_scene("res://" + scene)
	if root == null:
		return
	var world_env := root.get_node_or_null("WorldEnvironment") as WorldEnvironment
	_check(world_env != null and world_env.environment.volumetric_fog_enabled, scene + ": volumetric fog is not enabled.")
	var spots := 0
	var glass := 0
	for child in root.get_children():
		if child is SpotLight3D:
			spots += 1
		if child is MeshInstance3D and String(child.name).begins_with("Window"):
			glass += 1
	_check(spots >= FOG_SPOTS, "%s: %d window spots, expected %d." % [scene, spots, FOG_SPOTS])
	_check(glass >= FOG_SPOTS, "%s: %d glass windows, expected %d." % [scene, glass, FOG_SPOTS])
	_check_path_and_cameras(root, scene, 4)
	root.free()


func _check_rt_shadow_yard() -> void:
	var scene := "scenes/rt_shadow_yard/rt_shadow_yard.tscn"
	var root := _load_scene("res://" + scene)
	if root == null:
		return
	_check(_count_lights(root) >= YARD_LIGHTS, "%s: %d local lights, expected %d." % [scene, _count_lights(root), YARD_LIGHTS])
	var layers := 0
	var masks := {}
	for child in root.get_children():
		if child is MeshInstance3D and String(child.name).begins_with("LayerOccluder"):
			layers += 1
		if child is SpotLight3D and String(child.name).begins_with("MatrixLight"):
			masks[child.shadow_caster_mask] = true
	_check(layers == YARD_LAYERS, "%s: %d caster layers, expected %d." % [scene, layers, YARD_LAYERS])
	_check(masks.size() == YARD_LAYERS, "%s: %d distinct matrix masks, expected %d." % [scene, masks.size(), YARD_LAYERS])
	_check_path_and_cameras(root, scene, 4)
	root.free()


func _check_mega_lights_hall() -> void:
	var scene := "scenes/mega_lights_hall/mega_lights_hall.tscn"
	var root := _load_scene("res://" + scene)
	if root == null:
		return
	var lights := root.get_node_or_null("Lights")
	_check(lights != null and lights.get_child_count() == MEGA_BASE_LIGHTS,
			"%s: %d base lights, expected %d." % [scene, lights.get_child_count() if lights != null else 0, MEGA_BASE_LIGHTS])
	var areas := 0
	var projectors := 0
	var channels := 0
	var omnis := 0
	var spots := 0
	if lights != null:
		for child in lights.get_children():
			if child is AreaLight3D:
				areas += 1
			elif child is SpotLight3D:
				spots += 1
				if child.light_projector != null:
					projectors += 1
			elif child is OmniLight3D:
				omnis += 1
			if child is Light3D and child.light_cull_mask != 0xFFFFFFFF:
				channels += 1
	_check(areas >= 8, "%s: %d rect lights, expected at least 8." % [scene, areas])
	_check(projectors >= 4, "%s: %d IES projectors, expected at least 4." % [scene, projectors])
	_check(channels >= 2, "%s: %d lighting-channel lights, expected at least 2." % [scene, channels])
	_check(omnis > 0 and spots > 0, scene + ": point and spot lights are both expected.")
	var world_env := root.get_node_or_null("WorldEnvironment") as WorldEnvironment
	_check(world_env != null and world_env.environment.volumetric_fog_enabled, scene + ": volumetric fog is not enabled.")
	var glass := 0
	var sss := 0
	for child in root.get_children():
		if child is MeshInstance3D and String(child.name).begins_with("FrontGlass"):
			glass += 1
		if child is MeshInstance3D and String(child.name).begins_with("SssFoliage"):
			sss += 1
	_check(glass >= 4, "%s: %d front glass panes, expected at least 4." % [scene, glass])
	_check(sss >= 6, "%s: %d SSS foliage cards, expected at least 6." % [scene, sss])
	_check_path_and_cameras(root, scene, 4)
	root.free()
