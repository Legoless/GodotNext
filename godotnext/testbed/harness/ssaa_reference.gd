extends Node

const ArgsScript = preload("res://harness/args.gd")
const LAYERS := {
	"alpha_particles": 1 << 19,
	"scrolling_emissive": 1 << 20,
}

var _active := false
var _args: Dictionary = {}
var _prepared_scene_id := 0


func _ready() -> void:
	var parsed: Dictionary = ArgsScript.parse(OS.get_cmdline_user_args(), OS.get_cmdline_args())
	if bool(parsed.get("present", false)) and bool(parsed.get("ok", false)):
		var candidate: Dictionary = parsed["args"]
		if _has_upscaler_or_reference_flags(candidate):
			_args = candidate
			_active = true
	set_process(_active)


static func _has_upscaler_or_reference_flags(args: Dictionary) -> bool:
	for key in ["reference", "upscaler", "msaa", "mask", "scale"]:
		if args.has(key):
			return true
	return false


func is_active() -> bool:
	return _active


func apply_if_active(viewport: Viewport, scene: Node) -> void:
	if not _active:
		return
	if viewport != null:
		_apply_viewport(viewport)
	if scene != null and scene.get_instance_id() != _prepared_scene_id:
		_prepared_scene_id = scene.get_instance_id()
		_prepare_alpha_to_coverage(scene)
		_apply_mask(scene)


func _process(_delta: float) -> void:
	if not _active:
		return
	var tree := get_tree()
	if tree == null:
		return
	apply_if_active(get_viewport(), tree.current_scene)


func _apply_viewport(viewport: Viewport) -> void:
	var is_ssaa := String(_args.get("reference", "")) == "ssaa" or (_args.has("msaa") and not _args.has("upscaler"))
	if is_ssaa:
		viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
		viewport.scaling_3d_scale = float(_args.get("scale", 2.0))
		viewport.use_taa = false
		viewport.msaa_3d = Viewport.MSAA_4X if String(_args.get("msaa", "8")) == "4" else Viewport.MSAA_8X
		return
	if not _args.has("upscaler") and not _args.has("scale"):
		# Mask-only run without an upscaler or SSAA reference keeps native 1x un-jittered output.
		viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
		viewport.scaling_3d_scale = 1.0
		viewport.use_taa = false
		viewport.msaa_3d = Viewport.MSAA_DISABLED
		return
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	match String(_args.get("upscaler", "taa")):
		"fsr2":
			viewport.use_taa = false
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
			viewport.scaling_3d_scale = float(_args.get("scale", 0.5))
		"copy", "dlss":
			viewport.use_taa = false
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_DLSS
			viewport.scaling_3d_scale = float(_args.get("scale", 0.75))
		"metalfx":
			viewport.use_taa = false
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_METALFX_TEMPORAL
			viewport.scaling_3d_scale = float(_args.get("scale", 0.5))
		"tsr":
			viewport.use_taa = false
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_TSR
			viewport.scaling_3d_scale = float(_args.get("scale", 0.5))
		_:
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
			viewport.scaling_3d_scale = float(_args.get("scale", 1.0))
			viewport.use_taa = true


func _prepare_alpha_to_coverage(scene: Node) -> void:
	for node in scene.find_children("*", "MeshInstance3D", true, true):
		var instance := node as MeshInstance3D
		if instance.material_override is StandardMaterial3D:
			var override_mat := instance.material_override as StandardMaterial3D
			if override_mat.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR:
				var copy := override_mat.duplicate() as StandardMaterial3D
				copy.alpha_antialiasing_mode = BaseMaterial3D.ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE
				instance.material_override = copy
		if instance.mesh == null:
			continue
		var duplicated_mesh: Mesh = null
		for surface in instance.mesh.get_surface_count():
			var surf_override := instance.get_surface_override_material(surface)
			if surf_override is StandardMaterial3D and (surf_override as StandardMaterial3D).transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR:
				var copy := surf_override.duplicate() as StandardMaterial3D
				copy.alpha_antialiasing_mode = BaseMaterial3D.ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE
				instance.set_surface_override_material(surface, copy)
				continue
			var mesh_mat := instance.mesh.surface_get_material(surface)
			if mesh_mat is StandardMaterial3D and (mesh_mat as StandardMaterial3D).transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR:
				if duplicated_mesh == null:
					duplicated_mesh = instance.mesh.duplicate() as Mesh
					instance.mesh = duplicated_mesh
				var copy := mesh_mat.duplicate() as StandardMaterial3D
				copy.alpha_antialiasing_mode = BaseMaterial3D.ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE
				duplicated_mesh.surface_set_material(surface, copy)


func _apply_mask(scene: Node) -> void:
	var mask := String(_args.get("mask", ""))
	if not LAYERS.has(mask):
		return
	var selected: int = LAYERS[mask]
	var white := StandardMaterial3D.new()
	white.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	white.albedo_color = Color.WHITE
	white.cull_mode = BaseMaterial3D.CULL_DISABLED
	for node in scene.find_children("*", "Camera3D", true, true):
		(node as Camera3D).cull_mask = selected
	for node in scene.find_children("*", "VisualInstance3D", true, true):
		var instance := node as VisualInstance3D
		var keep: bool = (instance.layers & selected) != 0
		instance.layers = selected if keep else 0
		instance.visible = keep
		if not keep:
			continue
		if instance is MeshInstance3D:
			var mesh_instance := instance as MeshInstance3D
			var source := mesh_instance.material_override
			if source == null and mesh_instance.mesh != null and mesh_instance.mesh.get_surface_count() > 0:
				source = mesh_instance.get_active_material(0)
			if source is StandardMaterial3D and (source as StandardMaterial3D).transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR:
				var scissor := white.duplicate() as StandardMaterial3D
				scissor.albedo_texture = (source as StandardMaterial3D).albedo_texture
				scissor.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
				scissor.alpha_scissor_threshold = (source as StandardMaterial3D).alpha_scissor_threshold
				scissor.alpha_antialiasing_mode = BaseMaterial3D.ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE
				mesh_instance.material_override = scissor
			else:
				mesh_instance.material_override = white
		elif instance is GPUParticles3D:
			var particles := instance as GPUParticles3D
			if particles.draw_pass_1 != null:
				var pass_mesh := particles.draw_pass_1.duplicate() as Mesh
				if pass_mesh is PrimitiveMesh:
					(pass_mesh as PrimitiveMesh).material = white
				elif pass_mesh.get_surface_count() > 0:
					pass_mesh.surface_set_material(0, white)
				particles.draw_pass_1 = pass_mesh
	for node in scene.find_children("*", "Light3D", true, true):
		(node as Light3D).visible = false
	for node in scene.find_children("*", "WorldEnvironment", true, true):
		var world := node as WorldEnvironment
		if world.environment == null:
			continue
		var environment := world.environment.duplicate() as Environment
		environment.background_mode = Environment.BG_COLOR
		environment.background_color = Color.BLACK
		environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		environment.ambient_light_color = Color.BLACK
		environment.ambient_light_energy = 0.0
		environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
		environment.fog_enabled = false
		environment.volumetric_fog_enabled = false
		environment.sdfgi_enabled = false
		environment.glow_enabled = false
		environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
		environment.tonemap_exposure = 1.0
		world.environment = environment
