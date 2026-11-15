extends Node

const ArgsScript = preload("res://harness/args.gd")
const LAYERS := {
	"alpha_particles": 1 << 19,
	"scrolling_emissive": 1 << 20,
	"moire": 1 << 17,
	"thin_wire": 1 << 18,
}
## The GT-2 masks (#190) are drawn with the rest of the scene as black occluders, so a mask pixel
## is one where that content is visible. The #95 masks keep their layer-only form.
const OCCLUDED_MASKS := {
	"moire": true,
	"thin_wire": true,
}
## Screen-space growth of each side of a dilated mask, in pixels of a 1440p frame (the GT-2 content
## is sized at 1440p internal), so the mask covers the same share of the frame at any output size.
## A thin-wire mask covers the 0.3-1.0 px cable plus one 1440p pixel either side (1.5 px at
## 2160p), where its anti-aliased footprint lies.
const DILATE_PX := {
	"thin_wire": 1.0,
}
const DILATE_SHADER := """shader_type spatial;
render_mode unshaded, cull_disabled;
uniform float dilate_px = 1.0;
uniform float reference_height = 1440.0;
void vertex() {
	vec4 view_pos = MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
	vec3 view_normal = normalize((MODELVIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
	// One pixel of a reference_height frame at this depth, in view-space units.
	float pixel = -view_pos.z * 2.0 / (PROJECTION_MATRIX[1][1] * reference_height);
	view_pos.xyz += view_normal * pixel * dilate_px;
	POSITION = PROJECTION_MATRIX * view_pos;
}
void fragment() {
	ALBEDO = vec3(1.0);
}
"""

var _active := false
var _args: Dictionary = {}
var _prepared_scene_id := 0
## --upscaler=custom (#192): the test-only SampleCopy extension.
var _sample_copy: TemporalUpscalerExtension


func _ready() -> void:
	var parsed: Dictionary = ArgsScript.parse(OS.get_cmdline_user_args(), OS.get_cmdline_args())
	if bool(parsed.get("present", false)) and bool(parsed.get("ok", false)):
		var candidate: Dictionary = parsed["args"]
		if _has_upscaler_or_reference_flags(candidate):
			_args = candidate
			_active = true
	set_process(_active)


static func _has_upscaler_or_reference_flags(args: Dictionary) -> bool:
	for key in ["reference", "upscaler", "msaa", "mask", "scale", "tsr_quality"]:
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
		# GodotNext: the path-traced reference (#263) ignores alpha scissor, so it keeps the scene's materials.
		if String(_args.get("reference", "")) != "pt":
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
		# So does the path-traced reference (#263): its composite fills the internal HDR color.
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
			if _args.has("tsr_quality"):
				viewport.tsr_quality = ArgsScript.VALID_TSR_QUALITIES[String(_args["tsr_quality"])]
		"custom":
			_register_sample_copy()
			viewport.use_taa = false
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_CUSTOM
			viewport.scaling_3d_scale = float(_args.get("scale", 0.75))
		_:
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
			viewport.scaling_3d_scale = float(_args.get("scale", 1.0))
			viewport.use_taa = true


func _register_sample_copy() -> void:
	if _sample_copy != null:
		return
	_sample_copy = load("res://addons/sample_copy_upscaler/copy_upscaler.gd").new()
	ProjectSettings.set_setting("rendering/scaling_3d/custom_upscaler", _sample_copy._get_name())
	RenderingServer.register_temporal_upscaler_extension(_sample_copy)


func _exit_tree() -> void:
	if _sample_copy != null:
		RenderingServer.unregister_temporal_upscaler_extension(_sample_copy)
		_sample_copy = null


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
	var keep_material: Material = white
	if DILATE_PX.has(mask):
		var shader := Shader.new()
		shader.code = DILATE_SHADER
		var dilate := ShaderMaterial.new()
		dilate.shader = shader
		dilate.set_shader_parameter("dilate_px", float(DILATE_PX[mask]))
		keep_material = dilate
	var occluded := OCCLUDED_MASKS.has(mask)
	for node in scene.find_children("*", "Camera3D", true, true):
		(node as Camera3D).cull_mask = selected
	for node in scene.find_children("*", "VisualInstance3D", true, true):
		var instance := node as VisualInstance3D
		var keep: bool = (instance.layers & selected) != 0
		if not keep and occluded and _make_occluder(instance):
			instance.layers = selected
			continue
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
				mesh_instance.material_override = keep_material
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


## GT-2 masks (#190): opaque geometry outside the mask is drawn black, so it still hides the mask
## content behind it. Alpha-blended surfaces and particles do not occlude and stay hidden.
func _make_occluder(instance: VisualInstance3D) -> bool:
	if not (instance is MeshInstance3D) or not instance.is_visible_in_tree():
		return false
	var mesh_instance := instance as MeshInstance3D
	if mesh_instance.mesh == null or mesh_instance.mesh.get_surface_count() == 0:
		return false
	var source := mesh_instance.material_override
	if source == null:
		source = mesh_instance.get_active_material(0)
	var black := StandardMaterial3D.new()
	black.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	black.albedo_color = Color.BLACK
	if source is BaseMaterial3D:
		var base := source as BaseMaterial3D
		if base.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA or base.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_DEPTH_PRE_PASS:
			return false
		black.cull_mode = base.cull_mode
		if base.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR or base.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_HASH:
			black.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
			black.alpha_scissor_threshold = base.alpha_scissor_threshold
			black.albedo_texture = base.albedo_texture
			black.uv1_scale = base.uv1_scale
			black.uv1_offset = base.uv1_offset
	elif source is ShaderMaterial:
		var shader := (source as ShaderMaterial).shader
		if shader != null and shader.code.contains("cull_disabled"):
			black.cull_mode = BaseMaterial3D.CULL_DISABLED
	mesh_instance.material_override = black
	return true
